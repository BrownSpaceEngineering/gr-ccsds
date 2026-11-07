"""Optional UDP interoperability check against independent cfdp-py 0.7.0.

Run with a Python environment containing cfdp-py: make test-interop PYTHON=...
"""

from datetime import timedelta
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from cfdppy import CfdpState, CfdpUserBase, LocalEntityConfig, PutRequest
from cfdppy.exceptions import PduIgnoredForSource, PduIgnoredForSourceReason
from cfdppy.handler import DestHandler, RemoteEntityConfigTable, SourceHandler
from cfdppy.mib import (CheckTimerProvider, DefaultFaultHandlerBase, EntityType,
                        IndicationConfig, RemoteEntityConfig)
from spacepackets.cfdp import ChecksumType, TransmissionMode
from spacepackets.cfdp.pdu import FileDataPdu, PduFactory
from spacepackets.cfdp.tlv import (FileStoreRequestTlv, FilestoreActionCode,
                                  FilestoreResponseStatusCode)
from spacepackets.countdown import Countdown
from spacepackets.seqcount import SeqCountProvider
from spacepackets.util import ByteFieldU8


BIN = Path(__file__).resolve().parents[1] / "bin" / "cfdp"
CONTENT = bytes(range(256)) * 8


class User(CfdpUserBase):
    def __init__(self):
        super().__init__()
        self.finished = []

    def transaction_indication(self, params): pass
    def eof_sent_indication(self, transaction_id): pass
    def transaction_finished_indication(self, params): self.finished.append(params)
    def metadata_recv_indication(self, params): pass
    def file_segment_recv_indication(self, params): pass
    def report_indication(self, transaction_id, status_report): pass
    def suspended_indication(self, transaction_id, cond_code): pass
    def resumed_indication(self, transaction_id, progress): pass
    def fault_indication(self, transaction_id, cond_code, progress):
        raise AssertionError(f"peer fault: {cond_code}")
    def abandoned_indication(self, transaction_id, cond_code, progress):
        raise AssertionError(f"peer abandoned: {cond_code}")
    def eof_recv_indication(self, transaction_id): pass


class Faults(DefaultFaultHandlerBase):
    def notice_of_suspension_cb(self, transaction_id, cond, progress):
        raise AssertionError(f"peer suspended: {cond}")
    def notice_of_cancellation_cb(self, transaction_id, cond, progress):
        raise AssertionError(f"peer canceled: {cond}")
    def abandoned_cb(self, transaction_id, cond, progress):
        raise AssertionError(f"peer abandoned: {cond}")
    def ignore_cb(self, transaction_id, cond, progress):
        raise AssertionError(f"peer ignored fault: {cond}")


class CheckTimers(CheckTimerProvider):
    def provide_check_timer(self, local_entity_id, remote_entity_id,
                            entity_type: EntityType):
        return Countdown(timedelta(seconds=5))


def config(local_id, remote_id, mode, crc=False):
    local = LocalEntityConfig(ByteFieldU8(local_id), IndicationConfig(), Faults())
    remote = RemoteEntityConfig(
        entity_id=ByteFieldU8(remote_id), max_file_segment_len=512,
        max_packet_len=1200, closure_requested=False,
        crc_on_transmission=crc, default_transmission_mode=mode,
        crc_type=ChecksumType.MODULAR,
        positive_ack_timer_interval_seconds=1.0,
        nak_timer_interval_seconds=1.0)
    return local, RemoteEntityConfigTable([remote])


def udp_socket():
    result = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    result.bind(("127.0.0.1", 0))
    result.settimeout(0.05)
    return result


def available_port():
    with udp_socket() as probe:
        return probe.getsockname()[1]


def drain(handler, sock, peer, trace=None, drop=None, reorder=None,
          drop_control=None):
    while handler.packets_ready:
        packet = handler.get_next_packet()
        assert packet is not None
        if trace is not None:
            trace.append(("send", type(packet.pdu).__name__, len(packet.pack()),
                          packet.pack()[:24].hex()))
        if drop is not None and isinstance(packet.pdu, FileDataPdu) and drop[0]:
            drop[0] = False
            continue
        if (drop_control is not None and not drop_control["done"] and
                type(packet.pdu).__name__ == drop_control["type"]):
            drop_control["done"] = True
            continue
        if reorder is not None and isinstance(packet.pdu, FileDataPdu) and not reorder["done"]:
            if reorder["held"] is None:
                reorder["held"] = packet.pack()
                continue
            sock.sendto(packet.pack(), peer)
            sock.sendto(reorder["held"], peer)
            reorder["held"] = None
            reorder["done"] = True
            continue
        sock.sendto(packet.pack(), peer)


def cpp_to_python(mode, drop_data=False, crc=False, reorder_data=False,
                  drop_control=None, linger=0):
    with tempfile.TemporaryDirectory(prefix="cfdp-interop-") as work:
        root = Path(work)
        (root / "source.bin").write_bytes(CONTENT)
        local, remotes = config(2, 1, mode, crc)
        user = User()
        handler = DestHandler(local, user, remotes, CheckTimers())
        with udp_socket() as sock:
            remote_port = available_port()
            args = [str(BIN), "send", str(remote_port), "127.0.0.1",
                    str(sock.getsockname()[1]), str(root / "source.bin"),
                    str(root / "received.bin"), "1", "2"]
            if mode == TransmissionMode.UNACKNOWLEDGED:
                args.append("unack")
            if crc:
                args.append("--crc")
            if linger:
                args.extend(("--linger", str(linger)))
            proc = subprocess.Popen(args, cwd=root, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            try:
                drop = drop_data
                dropped = {"type": drop_control[1], "done": False} if drop_control else None
                control_trace = []
                held = None
                reordered = False
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    handler.state_machine()
                    drain(handler, sock, ("127.0.0.1", remote_port),
                          drop_control=dropped if drop_control and drop_control[0] == "python" else None)
                    try:
                        wire, address = sock.recvfrom(65536)
                    except socket.timeout:
                        wire = None
                    if wire is not None:
                        packet = PduFactory.from_raw_to_holder(wire).pdu
                        if drop_control:
                            control_trace.append(("cpp", type(packet).__name__))
                        if (drop_control and drop_control[0] == "cpp" and
                                not dropped["done"] and
                                type(packet).__name__ == dropped["type"]):
                            dropped["done"] = True
                            continue
                        if drop and isinstance(packet, FileDataPdu):
                            drop = False
                        elif reorder_data and isinstance(packet, FileDataPdu) and not reordered:
                            if held is None:
                                held = packet
                                continue
                            handler.state_machine(packet)
                            drain(handler, sock, address,
                                  drop_control=dropped if drop_control and drop_control[0] == "python" else None)
                            handler.state_machine(held)
                            held = None
                            reordered = True
                        else:
                            handler.state_machine(packet)
                        drain(handler, sock, address,
                              drop_control=dropped if drop_control and drop_control[0] == "python" else None)
                    if proc.poll() is not None and handler.state == CfdpState.IDLE:
                        break
                else:
                    raise AssertionError(f"C++ sender to cfdp-py timed out: "
                                         f"{drop_control}, {handler.state}, "
                                         f"{proc.poll()}, {control_trace[:12]}, "
                                         f"{control_trace[-12:]}")
                stdout, stderr = proc.communicate(timeout=1)
                assert proc.returncode == 0, (stdout, stderr)
                assert not drop, "no File Data PDU was dropped"
                assert not reorder_data or reordered, "File Data PDUs were not reordered"
                assert dropped is None or dropped["done"], "target control PDU was not dropped"
                assert (root / "received.bin").read_bytes() == CONTENT
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()


def python_to_cpp(mode, drop_data=False, crc=False, reorder_data=False,
                  filestore=False, drop_control=None, storage="memory"):
    with tempfile.TemporaryDirectory(prefix="cfdp-interop-") as work:
        root = Path(work)
        (root / "source.bin").write_bytes(CONTENT)
        output = root / "output"
        output.mkdir()
        local, remotes = config(1, 2, mode, crc)
        user = User()
        handler = SourceHandler(local, user, remotes, CheckTimers(),
                                SeqCountProvider(8))
        with udp_socket() as sock:
            remote_port = available_port()
            proc = subprocess.Popen([str(BIN), "recv", str(remote_port),
                                     str(output), "2", "--filestore", storage], cwd=root,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            try:
                # Wait for the CLI process to bind before sending its first PDU.
                time.sleep(0.2)
                assert proc.poll() is None, proc.communicate()
                trace = []
                drop = [drop_data]
                reorder = {"held": None, "done": False} if reorder_data else None
                dropped = {"type": drop_control[1], "done": False} if drop_control else None
                handler.put_request(PutRequest(
                    destination_id=ByteFieldU8(2), source_file=root / "source.bin",
                    dest_file=Path("received.bin"), trans_mode=mode,
                    closure_requested=False,
                    fs_requests=[FileStoreRequestTlv(
                        FilestoreActionCode.CREATE_FILE_SNM, "created.bin")]
                    if filestore else None))
                deadline = time.monotonic() + 15
                peer = ("127.0.0.1", remote_port)
                while time.monotonic() < deadline:
                    handler.state_machine()
                    drain(handler, sock, peer, trace, drop, reorder,
                          dropped if drop_control and drop_control[0] == "python" else None)
                    try:
                        wire, _ = sock.recvfrom(65536)
                    except socket.timeout:
                        wire = None
                    if wire is not None:
                        trace.append(("recv", len(wire), wire.hex()))
                        packet = PduFactory.from_raw_to_holder(wire).pdu
                        if (drop_control and drop_control[0] == "cpp" and
                                not dropped["done"] and
                                type(packet).__name__ == dropped["type"]):
                            dropped["done"] = True
                            continue
                        try:
                            handler.state_machine(packet)
                        except PduIgnoredForSource as ignored:
                            if (ignored.reason != PduIgnoredForSourceReason.NOT_WAITING_FOR_ACK or
                                    type(packet).__name__ not in ("AckPdu", "FinishedPdu")):
                                raise
                        drain(handler, sock, peer, trace, drop, reorder,
                              dropped if drop_control and drop_control[0] == "python" else None)
                    if proc.poll() is not None and handler.state == CfdpState.IDLE:
                        break
                else:
                    raise AssertionError(f"cfdp-py sender to C++ timed out: "
                                         f"{drop_control}, {handler.state}, {proc.poll()}, "
                                         f"{[(x[0], x[1]) for x in trace[-12:]]}")
                stdout, stderr = proc.communicate(timeout=1)
                assert proc.returncode == 0, (stdout, stderr, trace)
                assert not drop[0], "no File Data PDU was dropped"
                assert not reorder_data or reorder["done"], "File Data PDUs were not reordered"
                assert dropped is None or dropped["done"], "target control PDU was not dropped"
                assert (output / "received.bin").read_bytes() == CONTENT
                if filestore:
                    assert len(user.finished) == 1, user.finished
                    responses = user.finished[0].finished_params.file_store_responses
                    assert len(responses) == 1, responses
                    assert responses[0].action_code == FilestoreActionCode.CREATE_FILE_SNM
                    assert responses[0].status_code == FilestoreResponseStatusCode.SUCCESS
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()


if __name__ == "__main__":
    for transfer_mode in (TransmissionMode.UNACKNOWLEDGED,
                          TransmissionMode.ACKNOWLEDGED):
        cpp_to_python(transfer_mode)
        python_to_cpp(transfer_mode)
        print(f"cfdp-py interop passed: {transfer_mode.name}")
    cpp_to_python(TransmissionMode.ACKNOWLEDGED, drop_data=True)
    python_to_cpp(TransmissionMode.ACKNOWLEDGED, drop_data=True)
    print("cfdp-py Class 2 dropped-data recovery passed in both directions")
    cpp_to_python(TransmissionMode.ACKNOWLEDGED, crc=True)
    python_to_cpp(TransmissionMode.ACKNOWLEDGED, crc=True)
    print("cfdp-py Class 2 CRC transfer passed in both directions")
    cpp_to_python(TransmissionMode.ACKNOWLEDGED, reorder_data=True)
    python_to_cpp(TransmissionMode.ACKNOWLEDGED, reorder_data=True)
    print("cfdp-py Class 2 reordered-data transfer passed in both directions")
    python_to_cpp(TransmissionMode.ACKNOWLEDGED, filestore=True)
    print("cfdp-py Class 2 filestore request and response passed")
    python_to_cpp(TransmissionMode.ACKNOWLEDGED, drop_data=True,
                  filestore=True, storage="disk")
    print("cfdp-py disk filestore and dropped-data recovery passed")
    for label, lost in (
            ("F2-S1 Metadata", ("python", "MetadataPdu")),
            ("F2-S2 EOF", ("python", "EofPdu")),
            ("F2-S3 Finished", ("cpp", "FinishedPdu"))):
        python_to_cpp(TransmissionMode.ACKNOWLEDGED, drop_control=lost)
        print(f"cfdp-py control PDU recovery passed: {label}")
    cpp_to_python(TransmissionMode.ACKNOWLEDGED,
                  drop_control=("python", "AckPdu"))
    print("cfdp-py control PDU recovery passed: F2-S4 ACK(EOF)")
    cpp_to_python(TransmissionMode.ACKNOWLEDGED,
                  drop_control=("cpp", "AckPdu"), linger=3)
    print("cfdp-py control PDU recovery passed: F2-S5 ACK(Finished)")
