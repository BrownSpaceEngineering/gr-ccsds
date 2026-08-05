# Unified Space Data Link Protocol (USLP) Stack

This repository contains a C++20 implementation of the CCSDS Unified Space Data Link Protocol (USLP) designed for a university-class satellite mission. The software manages the multiplexing, packetization, and framing of uplink and downlink data, specifically supporting critical telecommands, solar cell research telemetry, and student-drawn images.

The software is engineered under strict timing, memory, and hardware constraints, aiming for alignment with NASA launch-readiness and flight software integration requirements.

---

## Building and Running

The project requires a compiler that supports **C++20** (such as GCC 10+).

### Compilation

Compile using this simple command:
```bash
make clean all
```

### Running Tests

Run the current demo simulation:
```bash
./bin/uslp
```

---

## Architecture Overview

The USLP stack operates asynchronously using a multi-threaded architecture to decouple packet ingestion from physical layer transmission:

```
[VCPRequest] (Uplink/Downlink Packets)
     │
     ▼
┌──────────────────────────────────────────────┐
│  AccumulationBuffer (Statically Allocated)   │
└────────────────────┬─────────────────────────┘
                     │ (Packet Metadata & Octet stream)
                     ▼
┌──────────────────────────────────────────────┐
│  VCPacketThread (Asynchronous Processor)     │ ◄── Monitors USLP-143 & USLP-144 Timers
└────────────────────┬─────────────────────────┘
                     │ (Constructed Transfer Frames)
                     ▼
┌──────────────────────────────────────────────┐
│  VCMultiplexer (Multiplexer Thread)          │ ◄── Handles idle fill frames (VCID 63)
└────────────────────┬─────────────────────────┘
                     │ (Serialized Byte Stream)
                     ▼
┌──────────────────────────────────────────────┐
│  UDP Socket / GNU Radio DSP Pipeline         │
└──────────────────────────────────────────────┘
```

### Key Components

* **`VCPRequest` (Data Ingestion):** The entry point for packet data. Validates the Packet Version Number (PVN) and queues the packet bytes into the corresponding Virtual Channel (VC) accumulator.
* **`VCPacketThread` (Frame Packing):** Runs on a background thread. Periodically monitors each active Virtual Channel, calculates the First Header Pointer (FHP), packs packets into Transfer Frame Data Zones (TFDZ), and applies temporal standard flushes.
* **`VCMultiplexer` (Multiplexing & Heartbeats):** Prioritizes outbound frames from the Virtual Channels. If the transmission queue is empty, it automatically generates and inserts Only Idle Data (OID) frames on VCID 63 to maintain physical link synchronization.

---

## Technical Features

### 1. Deterministic Embedded Memory Management
To comply with safety-critical flight software guidelines (such as MISRA C++) and prevent heap fragmentation, the stack strictly avoids dynamic memory allocation (`malloc`, `std::vector` resizing) during execution:
* **Statically Sized Buffers:** Implements a custom, templated `BitBuffer` backed by `std::array` for data storage.
* **Low RAM Footprint:** By optimizing buffer thresholds (e.g., matching the queue size to realistic boundaries), the memory footprint of the `USLP` controller was reduced from over **5.6 MB** to approximately **60 KB**, making it highly suitable for low-power microcontrollers.

### 2. CCSDS Temporal Standards
The stack natively implements two critical USLP temporal constraints to govern frame release:
* **USLP-143 (TFDF Completion Timeout):** Measures the elapsed time from when the first octet of a packet is placed into an empty accumulator. If the timeout threshold (`TFDFCompletionTimeoutMs`) is reached before the buffer is full, the partial frame is immediately flushed.
* **USLP-144 (Inter-Frame Delay):** Monitors the duration since the last frame was released on each active VC. If the configured `interFrameDelayMs` is exceeded, the VC automatically releases a padded "heartbeat" frame (even if the buffer is empty) to ensure link continuity and state machine updates.

### 3. FHP and Circular Buffer Indexing
The stack implements robust 16-bit First Header Pointer (FHP) calculations over a custom circular queue (`PacketPtrBuffer`):
* Correctly tracks the offset of the first fresh packet starting within the current frame window.
* Sets the FHP to the standard USLP 16-bit sentinel value of `65535` (`0xFFFF`) when no new packet starts in the frame.
* Dynamically shifts queued packet indices across frame boundaries when packets span multiple frames.

### 4. Standard 1-Octet Encapsulation Idle Packets (EIP)
Instead of using raw `0x00` padding—which causes receiver-side de-framing crashes because `0x00` is parsed as a Space Packet header—the stack utilizes CCSDS-compliant **1-octet Encapsulation Idle Packets** (`0xE0` / binary `11100000` as defined in CCSDS 133.1-B) to backfill incomplete frames.

---

## Directory Structure

* `/src`
  * `uslp.cpp` — Core USLP protocol processing, packet thread, and multiplexer implementations
  * `networking.cpp` — Networking code to connect to GNU Radio Module over UDP
  * `uslpstructs.h` — Definition of transfer frames, primary headers, inserts, and security structures
  * `packing.cpp` — Frame serialization and parsing logic
  * `tests.cpp` — Tests for protocol
  * `utils.cpp` — Helper functions
* `/tests`
  * `main.cpp` — Entry point running the multi-VC multiplexing tests and temporal compliance tests.

---

## GNU Radio Integration

To verify the physical layer, the simulated ground station or satellite USLP stack sends serialized frames out over a UDP socket.