# PVDXos USLP Implementation

The USLP implementation for PVDXos strictly supports USLP version 1 (TFVN `0b1100`) without COP-1 or COP-P. The system operates with minimal state: each virtual channel holds an accumulation buffer, a frame counter, and a completion timer. It is capable of segmenting upper-layer packets across multiple transfer frames and reassembling them on the receiving end, including packets that span frame boundaries. All frames are expedited (bypass flag set), so no frame is ever retransmitted.

[Flowchart](https://www.figma.com/design/AVev5VfAdXI5oYwxy4eFBS/USLP-Flowchart?node-id=0-1&t=cpO6N68t0oMtqRdh-1)

## Supported SAPs (Service Access Points)

* VC Packet Service, intended for CFDP packets
  * Sending side accepts one packet per `VCPRequest` call with a GVCID, PVN, SDU ID, and service type
  * Receiving side reassembles Space Packets from the transfer frame data zone using the First Header Pointer
  * Transmission is confirmed to the caller through a registered notify callback, fired once per SDU when the frame carrying its last byte is released

## Unsupported SAPs

* MAP Packet Service: MAP channels are structurally defined in the codebase but never populated, and MAPID is always 0
* MAP Access Service: `MAPA_SDU` is structurally defined in the codebase but not processed
* VC Access Service: `VCA_SDU` is structurally defined in the codebase but not processed
* Octet Stream Service: `OCTET_STREAM` is structurally defined in the codebase but not processed
* Insert Service: the insert zone is structurally defined and packed as an empty field, but never populated
* COP Management Service: not present

## Multiplexing Scheme

* Virtual channel multiplexing only. There is a single master channel and no master channel multiplexing step.
* A packing thread polls the three user virtual channels every 20 milliseconds in ascending VCID order (0, then 1, then 2). A channel is served when its accumulation buffer holds a full data zone, or when its completion timer has expired and the buffer is not empty.
* Service is strict priority by VCID, not round robin. A channel receiving a continuous stream can starve the channels after it.
* Frames from all channels enter a single FIFO queue and are released first come, first served by a separate multiplexer thread.
* When that queue stays empty for 20 milliseconds, the multiplexer builds an OID frame on VCID 63 with the data zone filled with `0x55`. The frame is discarded before transmission (see Known Deviations), though the VCID 63 frame counter still increments.

## Choices

* Fixed length transfer frames with length of 1024 bytes
  * Implies OID transfer frames
* No COP
  * Bypass Sequence Control Flag is always 1 (expedited)
  * Protocol Command Control Flag is always 0 (user data)
  * No Operational Control Field, so no CLCW is carried
* No SDLS
  * Security header and trailer are defined and returned as empty buffers, occupying no bytes in the frame
* No use of master channels
  * Single SCID of 42
  * Source/Destination Identifier is 1 (destination); the flight software build must set this to 0
* 3 virtual channels
  * VCID 0, 1, and 2 for user data, plus VCID 63 reserved for OID
  * Channel count is fixed at compile time by `VC_COUNT` in `data.h`
* Ignoring Insert zone
* TM Synchronization and Channel Coding sublayer (CCSDS 131.0-B)
  * Attached Sync Marker `1A CF FC 1D` prepended to every frame
  * Pseudo-randomizer and convolutional encoder implemented, both currently bypassed
* FECF, 32-bit
  * Computed over every byte of the frame except the trailing 4 bytes
* Fixed 1-byte VC frame count per channel, wrapping at 255
* Partial data zones padded with 1-octet CCSDS Encapsulation Idle Packets (`0xE0`) rather than zeros

## Transfer Frame Format

| Offset | Length | Field |
|---|---|---|
| 0 | 7 | Primary header, fixed part |
| 7 | 1 | VC frame count |
| 8 | 3 | TFDF header (construction rule, UPID, First Header Pointer) |
| 11 | 1009 | Transfer frame data zone |
| 1020 | 4 | FECF |

* A user data frame carries construction rule 0 and UPID 0. An OID frame carries construction rule 1 and UPID 31.
* The First Header Pointer gives the offset of the first packet starting in the data zone, or 65535 when no packet starts in the frame.

## Protocol Timers

* TFDF Completion Timeout (USLP-143): releases a partial frame when a channel has held data past its timeout. Configured at 100 ms on VCID 1, 20 ms on VCID 0, and 500 ms on VCID 2 and VCID 63. The timer is measured from the release of the previous frame on that channel, not from the arrival of the first octet into an empty buffer as the standard specifies.
* Inter-Frame Delay (USLP-144): configurable in the managed parameters and copied into each virtual channel, but never read. No heartbeat frame is produced.
* Thread tick rates: 20 ms for the packing thread, the multiplexer, and the frame reception thread; 50 ms for the completed packet writer.

## System Constraints and Limitations

The implementation operates under strict memory, size, and feature constraints due to the nature of the environment.

### Storage and Memory Constraints

* The software has no filesystem. Received packets are appended to `PacketOutput.txt` and transmitted frames to `TestOutput.txt` for inspection only.
* Frames, headers, and payloads use a fixed-capacity `BitBuffer` backed by `std::array`, so no heap allocation occurs on the frame path. Three members still allocate: the SDU ID list on each frame, the queue inside each thread-safe queue, and the vector returned by the coding sublayer.
* Each virtual channel holds one accumulation buffer of 65535 bytes.
* Each virtual channel tracks at most 128 queued packets in a ring. A packet arriving when the ring is full is dropped without a message and without a callback. The `BUFFER_OVERFLOW_DISCARDED` notification type is defined but never sent.
* The maximum accepted packet size is 65535 bytes. The configured `maxPacketLength` of 4096 is not enforced.
* The transmitted frame history array holds 2048 entries and consumes 6.65 MB. Its index does not wrap, so the stack writes past the end of the array after 2048 frames.
* The complete `USLP` object measures 7.2 MB on a 64-bit host.

### Operational Constraints

* Concurrency: the system supports exactly 3 user virtual channels and 1 idle channel, fixed at compile time. Four background threads run for the lifetime of the object: packing, multiplexing, frame reception, and packet writing.
* Entity Identifiers: the spacecraft identifier is 42 and the virtual channel identifiers are 0, 1, and 2, with 63 reserved for idle data. MAPID is always 0. All are configurable in the managed parameters.
* Networking: one UDP socket transmits frames to `127.0.0.1:5000` for the GNU Radio flowgraph. There is no receive socket. Nothing in the codebase reads frames back from the radio.
* Loopback: every frame released by the sending path is pushed directly onto the receive queue of the same object, before coding. This exercises the receiving path in one process and means the FECF check always passes during testing.
* Data Integrity: the 32-bit FECF uses the IEEE 802.3 CRC polynomial in reflected form, not the generator polynomial defined by CCSDS. A 16-bit CRC-CCITT option is available and does match the standard.
* Packet Delivery: completed packets are written to a file. There is no callback or queue that hands a received packet to user code.
* Loss Reporting: a gap in the VC frame count is logged and clears that channel's partial packet buffer. No loss flag is delivered to the upper layer.
* Padding Ambiguity: a packet whose first octet of user data is `0xE0` is indistinguishable from Encapsulation Idle Packet padding and will be discarded by the receiver.
* State Control Limitations: the implementation has no transaction control. It does not support sequence-controlled service, frame retransmission, truncated frames, or variable-length frames.

### Coding and Synchronization Sublayer

* Attached Sync Marker: `1A CF FC 1D` is prepended to every frame, producing a 1028-byte physical frame.
* Supported randomizer: the CCSDS additive scrambler with polynomial `x^17 + x^14 + 1`. Its shift register starts from a constant that is not the all-ones state the standard specifies.
* Supported coding: rate 1/2, constraint length 7 convolutional coding, doubling the frame to 2056 bytes. The encoder state persists across frames and is never reset.
* Unsupported coding methods: Reed-Solomon, concatenated, turbo, and LDPC are structurally defined in the codebase but not processed. A short randomizer is defined but not implemented.
* Both the randomizer and the encoder are currently set to `NONE`. This is temporary, to let the QPSK demodulator lock onto unscrambled frames during bring-up.

## Known Deviations

These points differ from CCSDS 732.1-B, CCSDS 131.0-B, or from behavior the code appears to intend:

* The Transfer Frame Length field carries 1027 instead of 1023, because the constant used includes the 4-byte ASM. No receiver in this codebase reads the field.
* OID frames are generated by the multiplexer but discarded in `PrepareTransferFrame`, where the transmission call is commented out. No idle frames reach the radio.
* The USLP-143 timer starts at the previous frame release rather than at the first octet of a new accumulation.
* USLP-144 is configured but not implemented.
* The 32-bit FECF polynomial is not the CCSDS polynomial.
* The randomizer shift register does not start from the all-ones state.
* The frame history index is unbounded and overwrites adjacent memory after 2048 frames.
* `RunUslpTemporalStandardsTest` is not called from `main`, and asserts zero padding and a heartbeat frame. It will fail if enabled.
