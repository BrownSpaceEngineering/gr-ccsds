# gr-ccsds

This repository consists of formats, documentation, and modules for
working with the Pervoskite Visual Degradation eXperiment's (PVDX)
CCSDS-based communications stack and tools.

## Design Documents

We will be releasing our communications design documentation open-source
as progress is made on PVDX, but we are currently exploring the best means
to do so, given our internal documentation is on a different platform.

For now, if you'd like access, please email either
bse@brown.edu or alexander_khosrowshahi@brown.edu and put
'Request for documentation access' in the subject.

## Navigating the Repository

Every directory contains files relating to a different communications protocol.
Within each, there is a README.md file detailing protocol specification
and file uses within the directory.

### Directory Structure

- `USLP/` - Unified Space Link Protocol, used as the data-layer protocol for PVDX with COP-1
- `cfdp/` - CCSDS File Delivery Protocol, transport protocol primarily for images sent by PVDX.
- `spp/` - Space Packet Protocol, packetization protocol used by PVDX
- `pvdx/` - Telemetry and data formats used by PVDX internally.
