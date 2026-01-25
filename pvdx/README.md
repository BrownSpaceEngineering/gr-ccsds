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

### Overall Format


| Section                 | Field              | Description                                          | Value Type        | Size (Bytes) |
|-------------------------|--------------------|------------------------------------------------------|-------------------|--------------|
| Preamble / Metadata | Callsign           | Required by international law                        | char[6]           | 6            |
|                         | Message Size       | Message size in bytes                                | uint32            | 4            |
|                         | n = Number of Cmds | If zero, assume command bits are 0                   | uint16            | 2            |
| Command                 | Command to execute | See Uplink Commands                                  | enum              | 2            |
|                         | Command data size  |                                                      | uint32            | 4            |
|                         | Command data       | Upper bound of a 256 x 64 bitmap with 16-level value | dependent on enum | <= 8192      |

### PVDX Uplink Commands


| PVDX Uplink Commands          |                              |            |              |
|-------------------------------|------------------------------|------------|--------------|
| Command (enum)                | Data                         | Type       | Size (bytes) |
| Enable/Disable Device         | Device in question           | Enum       | 2            |
| Set device as broken/unbroken | Device in question           | Enum       | 2            |
| Update display                | Display bitmap               | char[8192] | 8192         |
| Sleep                         | Time to sleep for            | uint32     | 4            |
| Reboot                        | Flag to reboot               | uint8      | 1            |
| Take picture                  | Timestamp to take picture at | uint32     | 4            |
| Send pictures                 | Number of pictures to send   | int32      | 4            |
| Set Kepler coefficients       | Kepler coefficients          | float[6]   | 24           |
| Set Unix Time                 | Unix Time in seconds         | unit32     | 4            |
| Set ADCS Opmode               | ADCS Opmode                  | Enum       | 2            |
| ADCS Update Parameters        | See ADCS Command Format      | Struct     |              |
| Set power mode                | Power mode                   | Enum       | 2            |

### ADCS Command Format


| ADCS Command Format |                                                                                                     |         |              |
|---------------------|-----------------------------------------------------------------------------------------------------|---------|--------------|
| Field               | Description                                                                                         | Type    | Size (Bytes) |
| Timestamp           |                                                                                                     | uint32  | 4            |
| Inclination         | degrees, Angle from equatorial reference plane                                                      | float32 | 4            |
| RAAN                | degrees, measured in ECI. Angle from orbit ascending node to reference frame's reference direction. | float32 | 4            |
| Eccentricity        | unitless, 0-1. Describes deviation from orbit circularity.                                          | float32 | 4            |
| Argument of Perigee | degrees, angle from body's ascending node to periapsis                                              | float32 | 4            |
| Mean anomaly        |                                                                                                     | float32 | 4            |
| Mean Motion         | Revolutions per day                                                                                 | float32 | 4            |

## Downlink Format

> Under Construction :(
