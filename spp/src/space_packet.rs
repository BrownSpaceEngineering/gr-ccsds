use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::io::{Cursor, Read, Write};

// https://ccsds.org/Pubs/133x0b2e2.pdf#%5B%7B%22num%22%3A89%2C%22gen%22%3A0%7D%2C%7B%22name%22%3A%22XYZ%22%7D%2C78%2C728%2C0%5D

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum PacketType {
    Telemetry,
    Telecommand,
}

impl TryFrom<u8> for PacketType {
    type Error = std::io::Error;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(PacketType::Telemetry),
            1 => Ok(PacketType::Telecommand),
            _ => Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "Invalid packet type",
            )),
        }
    }
}

impl From<PacketType> for u8 {
    fn from(pt: PacketType) -> u8 {
        match pt {
            PacketType::Telemetry => 0,
            PacketType::Telecommand => 1,
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct SpacePacketPrimaryHeader {
    pub packet_version_number: u8,
    pub packet_type: PacketType,
    pub secondary_header_flag: bool, // (if secondary header is present)
    pub application_process_id: u16,

    /*
       Sequence Flags (optional, can be used to indicate that packet is part of larger set of data)
       - 00 if continuation segment
       - 01 if first segment
       - 10 if last segment
       - 11 if unsegmented
    */
    pub sequence_flag: u8,

    /*
       Packet Sequence Cound or Packet Name
       - Continuous (mod 16384)
       - Has to be count for telemetry (can be name for telecommand)
    */
    pub packet_sequence: u16,
    pub data_length: u16,
}

impl SpacePacketPrimaryHeader {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let metadata_bytes = reader.read_u16::<BigEndian>()?;
        let packet_version_number = (metadata_bytes >> 13) as u8;
        let packet_type_num = ((metadata_bytes >> 12) & 0x1) as u8;
        let packet_type = PacketType::try_from(packet_type_num)?;
        let secondary_header_flag = ((metadata_bytes >> 11) & 0x1) == 1;
        let application_process_id = metadata_bytes & 0x7FF;

        let sequence_bytes = reader.read_u16::<BigEndian>()?;
        let sequence_flag = (sequence_bytes >> 14) as u8;
        let packet_sequence = sequence_bytes & 0x3FFF;

        let data_length = reader.read_u16::<BigEndian>()?;

        Ok(SpacePacketPrimaryHeader {
            packet_version_number,
            packet_type,
            secondary_header_flag,
            application_process_id,
            sequence_flag,
            packet_sequence,
            data_length,
        })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        let packet_type_u8: u8 = self.packet_type.into();
        let metadata_bytes = ((self.packet_version_number as u16) << 13)
            | ((packet_type_u8 as u16) << 12)
            | ((self.secondary_header_flag as u16) << 11)
            | (self.application_process_id & 0x7FF);

        let sequence_bytes = ((self.sequence_flag as u16) << 14) | (self.packet_sequence & 0x3FFF);

        writer.write_u16::<BigEndian>(metadata_bytes)?;
        writer.write_u16::<BigEndian>(sequence_bytes)?;
        writer.write_u16::<BigEndian>(self.data_length)?;

        Ok(())
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct SpacePacketSecondaryHeader {
    // Can contain either time code, ancillary data field, or neither
    // TODO: Talk to ADCS about what we might want here
}

impl SpacePacketSecondaryHeader {
    pub fn from_reader<R: Read>(_reader: &mut R) -> std::io::Result<Self> {
        Ok(SpacePacketSecondaryHeader {})
    }

    pub fn to_writer<W: Write>(&self, _writer: &mut W) -> std::io::Result<()> {
        Ok(())
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct SpacePacket {
    pub primary_header: SpacePacketPrimaryHeader,
    pub secondary_header: Option<SpacePacketSecondaryHeader>,
    pub data: Vec<u8>,
}

impl SpacePacket {
    pub fn from_bytes(bytes: &[u8]) -> std::io::Result<Self> {
        let mut reader = Cursor::new(bytes);
        let primary_header = SpacePacketPrimaryHeader::from_reader(&mut reader)?;

        let secondary_header = if primary_header.secondary_header_flag {
            Some(SpacePacketSecondaryHeader::from_reader(&mut reader)?)
        } else {
            None
        };

        let mut data = Vec::with_capacity(primary_header.data_length as usize);
        reader
            .take(primary_header.data_length as u64)
            .read_to_end(&mut data)?;

        Ok(SpacePacket {
            primary_header,
            secondary_header,
            data,
        })
    }

    pub fn to_bytes(&self) -> std::io::Result<Vec<u8>> {
        let mut writer = Cursor::new(Vec::new());
        self.primary_header.to_writer(&mut writer)?;
        if let Some(secondary_header) = &self.secondary_header {
            secondary_header.to_writer(&mut writer)?;
        }
        writer.write_all(&self.data)?;
        Ok(writer.into_inner())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        packet_bytes.extend_from_slice(&data_length.to_be_bytes());
        packet_bytes.extend_from_slice(&data_bytes);

        let packet = SpacePacket::from_bytes(&packet_bytes).unwrap();

        assert_eq!(packet.primary_header.packet_version_number, 7);
        assert_eq!(packet.primary_header.packet_type, PacketType::Telecommand);
        assert_eq!(packet.primary_header.secondary_header_flag, false);
        assert_eq!(packet.primary_header.application_process_id, 0x123);
        assert_eq!(packet.primary_header.sequence_flag, 3);
        assert_eq!(packet.primary_header.packet_sequence, 0x1FFF);
        assert_eq!(packet.primary_header.data_length, 2);
        assert_eq!(packet.data, vec![0xDE, 0xAD]);
    }

    #[test]
    fn test_space_packet_roundtrip() {
        let header = SpacePacketPrimaryHeader {
            packet_version_number: 1,
            packet_type: PacketType::Telemetry,
            secondary_header_flag: true,
            application_process_id: 0x2A,
            sequence_flag: 2,
            packet_sequence: 0x1234,
            data_length: 3,
        };
        let secondary_header = SpacePacketSecondaryHeader {};
        let data = vec![0x01, 0x02, 0x03];

        let packet = SpacePacket {
            primary_header: header,
            secondary_header: Some(secondary_header),
            data: data.clone(),
        };

        let bytes = packet.to_bytes().unwrap();
        let parsed = SpacePacket::from_bytes(&bytes).unwrap();
        assert_eq!(parsed, packet);
    }
}
