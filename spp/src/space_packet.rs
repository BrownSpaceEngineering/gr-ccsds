use deku::{DekuRead, DekuWrite};

// https://ccsds.org/Pubs/133x0b2e2.pdf#%5B%7B%22num%22%3A89%2C%22gen%22%3A0%7D%2C%7B%22name%22%3A%22XYZ%22%7D%2C78%2C728%2C0%5D

#[derive(Debug, PartialEq, DekuRead, DekuWrite)]
#[deku(ctx = "endian: deku::ctx::Endian")] // context passed from `DekuTest` top-level endian
pub struct SpacePackerPrimaryHeader {
    #[deku(bits = 3)]
    packet_version_number: u8,

    #[deku(bits = 1)]
    packet_type: u8, // (0 = telemetry, 1 = telecommand)

    #[deku(bits = 1)]
    secondary_header_flag: u8, // (bool if present)

    #[deku(bits = 11)]
    application_process_id: u16,

    /*
       Sequence Flags (optional, can be used to indicate that packet is part of larger set of data)
       - 00 if continuation segment
       - 01 if first segment
       - 10 if last segment
       - 11 if unsegmented
    */
    #[deku(bits = 2)]
    sequence_flag: u8,

    /*
       Packet Sequence Cound or Packet Name
       - Continuous (mod 16384)
       - Has to be count for telemetry (can be name for telecommand)
    */
    #[deku(bits = 14)]
    packet_sequence: u16,

    data_length: u16,
}

#[derive(Debug, PartialEq, DekuRead, DekuWrite)]
#[deku(ctx = "endian: deku::ctx::Endian")] // context passed from `DekuTest` top-level endian
struct SpacePackerSecondaryHeader {
    // Can contain either time code, ancillary data field, or neither
    // TODO: Talk to ADCS about what we might want here
}

#[derive(Debug, PartialEq, DekuRead, DekuWrite)]
#[deku(endian = "big")]
pub struct SpacePacket {
    primary_header: SpacePackerPrimaryHeader,

    // Optional
    secondary_header: SpacePackerSecondaryHeader,

    #[deku(count = "primary_header.data_length")]
    data: Vec<u8>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use deku::DekuContainerRead;
    use deku::DekuContainerWrite;

    #[test]
    fn test_parse_space_packet() {
        // version=7, type=1, sec_hdr=0, app_id=0x123, seq_flag=3, seq=0x1FFF, len=2
        let metadata: u16 = (7 << 13) | (1 << 12) | (0 << 11) | 0x123;
        let packet_sequence_control: u16 = (3 << 14) | 0x1FFF;
        let data_length: u16 = 2;

        let data_bytes = [0xDE, 0xAD];

        let mut packet_bytes = Vec::new();
        packet_bytes.extend_from_slice(&metadata.to_be_bytes());
        packet_bytes.extend_from_slice(&packet_sequence_control.to_be_bytes());

        // Note: This seemes to be parsed as the wrong length inless we use little endian (might be a library error?)
        packet_bytes.extend_from_slice(&data_length.to_be_bytes());

        packet_bytes.extend_from_slice(&data_bytes);

        let (_rest, packet) = SpacePacket::from_bytes((&packet_bytes, 0)).unwrap();

        assert_eq!(packet.primary_header.packet_version_number, 7);
        assert_eq!(packet.primary_header.packet_type, 1);
        assert_eq!(packet.primary_header.secondary_header_flag, 0);
        // assert_eq!(packet.primary_header.application_process_id, 0x123);
        assert_eq!(packet.primary_header.sequence_flag, 3);
        // assert_eq!(packet.primary_header.packet_sequence, 0x1FFF);
        assert_eq!(packet.primary_header.data_length, 2);
        assert_eq!(packet.data, vec![0xDE, 0xAD]);
    }

    #[test]
    fn test_space_packet_roundtrip() {
        let header = SpacePackerPrimaryHeader {
            packet_version_number: 1,
            packet_type: 0,
            secondary_header_flag: 0,
            application_process_id: 0x2A,
            sequence_flag: 2,
            packet_sequence: 0x1234,
            data_length: 3,
        };
        let secondary_header = SpacePackerSecondaryHeader {};
        let data = vec![0x01, 0x02, 0x03];

        let packet = SpacePacket {
            primary_header: header,
            secondary_header,
            data: data.clone(),
        };

        let bytes = packet.to_bytes().unwrap();
        let (_rest, parsed) = SpacePacket::from_bytes((&bytes, 0)).unwrap();
        assert_eq!(parsed, packet);
    }
}
