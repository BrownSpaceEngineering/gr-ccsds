#!/usr/bin/env python3
"""Loopback smoke tests for the UDP CLI and multi-peer server."""

import socket
import subprocess
import tempfile
import time
from pathlib import Path

PROGRAM = Path(__file__).resolve().parent.parent / "bin" / "cfdp"


def unused_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run(mode, filestore):
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        source = base / "source.bin"
        source.write_bytes(bytes(range(256)) * 32)
        destination = base / "received"
        destination.mkdir()
        sender_port, receiver_port = unused_udp_port(), unused_udp_port()
        receiver = subprocess.Popen(
            [str(PROGRAM), "recv", str(receiver_port), str(destination), "2",
             "--filestore", filestore],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            time.sleep(0.2)
            command = [
                str(PROGRAM),
                "send",
                str(sender_port),
                "127.0.0.1",
                str(receiver_port),
                str(source),
                "result.bin",
                "1",
                "2",
            ]
            if mode == "unack":
                command.append("unack")
            sender = subprocess.run(
                command, capture_output=True, text=True, timeout=20, check=False
            )
            output, error = receiver.communicate(timeout=20)
            assert sender.returncode == 0, (sender.stdout, sender.stderr)
            assert receiver.returncode == 0, (output, error)
            assert (destination / "result.bin").read_bytes() == source.read_bytes()
            assert not list(destination.glob("*.part"))
        finally:
            if receiver.poll() is None:
                receiver.kill()
                receiver.wait()


def run_service(filestore):
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        destination = base / "received"
        destination.mkdir()
        receiver_port = unused_udp_port()
        server = subprocess.Popen(
            [str(PROGRAM), "serve", str(receiver_port), str(destination), "2",
             "--filestore", filestore],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        senders = []
        try:
            time.sleep(0.2)
            assert server.poll() is None, server.communicate()
            for source_id in (1, 3):
                source = base / f"source-{source_id}.bin"
                content = bytes([source_id]) * 4096
                source.write_bytes(content)
                senders.append(
                    subprocess.Popen(
                        [
                            str(PROGRAM),
                            "send",
                            str(unused_udp_port()),
                            "127.0.0.1",
                            str(receiver_port),
                            str(source),
                            f"result-{source_id}.bin",
                            str(source_id),
                            "2",
                            "--state",
                            str(base / f"sequence-{source_id}"),
                        ],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                )
            for source_id, sender in zip((1, 3), senders):
                output, error = sender.communicate(timeout=20)
                assert sender.returncode == 0, (output, error)
                content = bytes([source_id]) * 4096
                assert (destination / f"result-{source_id}.bin").read_bytes() == content
            source = base / "source-1.bin"
            source.write_bytes(b"second transfer from entity 1")
            repeat = subprocess.run(
                [
                    str(PROGRAM),
                    "send",
                    str(unused_udp_port()),
                    "127.0.0.1",
                    str(receiver_port),
                    str(source),
                    "repeat.bin",
                    "1",
                    "2",
                    "--state",
                    str(base / "sequence-1"),
                ],
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert repeat.returncode == 0, (repeat.stdout, repeat.stderr)
            assert (destination / "repeat.bin").read_bytes() == source.read_bytes()
            assert (base / "sequence-1").read_text() == f"{3:020d}"
            state = (destination / ".cfdp-filestore-state" if filestore == "memory"
                     else destination / ".cfdp-store" / "catalog")
            assert state.is_file()
            assert server.poll() is None, server.communicate()
            assert not list(destination.glob("*.part"))
        finally:
            for sender in senders:
                if sender.poll() is None:
                    sender.kill()
                    sender.wait()
            if server.poll() is None:
                server.kill()
            server.communicate(timeout=5)


def run_storage_failure(command, filestore):
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        destination = base / "received"
        destination.mkdir()
        # Directories at these paths force rename to fail without relying on
        # permission bits (the tests can also run as root).
        (destination / "result.bin").mkdir()
        source = base / "source.bin"
        source.write_bytes(b"must not acknowledge failed storage")
        receiver_port = unused_udp_port()
        receiver = subprocess.Popen(
            [str(PROGRAM), command, str(receiver_port), str(destination), "2",
             "--filestore", filestore],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            time.sleep(0.2)
            sender = subprocess.run(
                [str(PROGRAM), "send", str(unused_udp_port()), "127.0.0.1",
                 str(receiver_port), str(source), "result.bin", "1", "2"],
                capture_output=True, text=True, timeout=15,
            )
            assert sender.returncode != 0, sender.stdout
            assert "CFDP condition 4" in sender.stderr, sender.stderr
            assert not list(destination.glob("*.part"))
        finally:
            if receiver.poll() is None:
                receiver.kill()
            receiver.communicate(timeout=5)


for storage_mode in ("memory", "disk"):
    for transfer_mode in ("ack", "unack"):
        run(transfer_mode, storage_mode)
        print(f"{transfer_mode} UDP transfer passed ({storage_mode} filestore)")

    run_service(storage_mode)
    print(f"multi-peer UDP service passed ({storage_mode} filestore)")

    for receiver_command in ("recv", "serve"):
        run_storage_failure(receiver_command, storage_mode)
        print(f"{receiver_command} storage failure reported ({storage_mode} filestore)")
