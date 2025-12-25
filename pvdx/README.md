# PVDX Communications Data Format

## Purpose

This directory contains specifications for and implementations
of formats and serialization/deserialization of the
Pervoskite Visual Degradation eXperiment (PVDX) CubeSat's
internal uplink/downlink communications format.

Formats are only specified for UHF half-duplex communications
marked as **non-image transfer frames.** Image transfer frames
are handled by CFDP and are packetized as pure data below SPP,
to be processed after file transfer completion.

All messages sent between PVDX and the PVDX ground station
are expected to follow specified formats or should be considered malformed.

## Uplink Format


| Section                 | Field              | Description                                          | Value Type        | Size (Bytes) |
|-------------------------|--------------------|------------------------------------------------------|-------------------|--------------|
| Preamble / <br>Metadata | Callsign           | Required by international law                        | char[6]           | 6            |
|                         | Message Size       | Message size in bytes                                | uint32            | 4            |
|                         | n = Number of Cmds | If zero, assume command bits are 0                   | uint16            | 2            |
| Command                 | Command to execute | See Uplink Commands                                  | enum              | 2            |
|                         | Command data size  |                                                      | uint32            | 4            |
|                         | Command data       | Upper bound of a 256 x 64 bitmap with 16-level value | dependent on enum | <= 8192      |


## Downlink Format
