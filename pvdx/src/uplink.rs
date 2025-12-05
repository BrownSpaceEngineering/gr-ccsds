use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::{
    io::{Cursor, Read, Write},
    process::Command,
};

pub struct UplinkPacket {
    pub uplink_header: UplinkPacketHeader,
    pub uplink_cmd_header: UplinkCommandHeader,
    pub data: Vec<u8>,
}

impl UplinkPacket {
    pub fn from_bytes(bytes: &[u8]) -> std::io::Result<Self> {
        todo!()
    }

    pub fn to_bytes(&self) -> std::io::Result<Vec<u8>> {
        todo!()



        let mut writer = Cursor::new(Vec::new());
        self.uplink_header.to_writer(&mut writer)?;
    }
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum CommandType {
    DeviceEnable,  // u8: Device in question
    DeviceDisable, // u8: Device in question
    DeviceBreak,   // u8: Device in question
    DeviceUnbreak, // u8: Device in question
    DisplayUpdate, // [u8: 8192]: Image bitmap
    Sleep,         // u32: Duration to sleep
    Reboot,
    PictureCapture, // u32: Timestamp to take picture
    PictureSend,    // u16: Number of pictures to send
    SetTime,        // u32: UNIX Timestamp
    SetPowerMode,
    ADCSSetOpMode,
    ADCSSetKeplers, // ADCSUpdate: Set Kepler Coefficients for ADCS
}

impl TryFrom<u8> for CommandType {
    type Error = std::io::Error;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(CommandType::DeviceEnable),
            1 => Ok(CommandType::DeviceDisable),
            2 => Ok(CommandType::DeviceBreak),
            3 => Ok(CommandType::DeviceUnbreak),
            4 => Ok(CommandType::DisplayUpdate),
            5 => Ok(CommandType::Sleep),
            6 => Ok(CommandType::Reboot),
            7 => Ok(CommandType::PictureCapture),
            8 => Ok(CommandType::PictureSend),
            9 => Ok(CommandType::SetTime),
            10 => Ok(CommandType::SetPowerMode),
            11 => Ok(CommandType::ADCSSetOpMode),
            12 => Ok(CommandType::ADCSSetKeplers),
            _ => Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "Invalid command type",
            )),
        }
    }
}

impl TryFrom<CommandType> for u8 {
    type Error = ();

    fn try_from(ct: CommandType) -> Result<Self, Self::Error> {
        Ok(match ct {
            CommandType::DeviceEnable => 0,
            CommandType::DeviceDisable => 1,
            CommandType::DeviceBreak => 2,
            CommandType::DeviceUnbreak => 3,
            CommandType::DisplayUpdate => 4,
            CommandType::Sleep => 5,
            CommandType::Reboot => 6,
            CommandType::PictureCapture => 7,
            CommandType::PictureSend => 8,
            CommandType::SetTime => 9,
            CommandType::SetPowerMode => 10,
            CommandType::ADCSSetOpMode => 11,
            CommandType::ADCSSetKeplers => 12,
        })
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct UplinkPacketHeader {
    pub callsign: [u8; 6],
    pub size: u32,
    pub n_cmds: u16,
}

impl UplinkPacketHeader {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let mut callsign = [0u8; 6];
        reader.read_exact(&mut callsign)?;
        let size = reader.read_u32::<BigEndian>()?;
        let n_cmds = reader.read_u16::<BigEndian>()?;

        Ok(UplinkPacketHeader {
            callsign,
            size,
            n_cmds,
        })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        writer.write_all(&self.callsign)?;
        writer.write_u32::<BigEndian>(self.size)?;
        writer.write_u16::<BigEndian>(self.n_cmds)?;

        Ok(())
    }
}

pub struct UplinkCommandHeader {
    pub cmd_type: CommandType,
    pub cmd_sz: u32,
}

impl UplinkCommandHeader {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let cmd_type_u8 = reader.read_u8()?;
        let cmd_type = CommandType::try_from(cmd_type_u8)?;
        let cmd_sz = reader.read_u32::<BigEndian>()?;

        Ok(UplinkCommandHeader { cmd_type, cmd_sz })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        let cmd_type_u8: u8 = self.cmd_type.try_into()?;
        writer.write_u8(cmd_type_u8)?;
        writer.write_u32::<BigEndian>(self.cmd_sz)?;

        Ok(())
    }
}

pub struct ADCSUpdate {
    pub timestamp: u32,
    pub inclination: f32,
    pub raan: f32,
    pub eccentricity: f32,
    pub perigee_arg: f32,
    pub mean_anomaly: f32,
    pub mean_motion: f32,
}

impl ADCSUpdate {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let timestamp = reader.read_u32::<BigEndian>()?;
        let inclination = reader.read_f32::<BigEndian>()?;
        let raan = reader.read_f32::<BigEndian>()?;
        let eccentricity = reader.read_f32::<BigEndian>()?;
        let perigee_arg = reader.read_f32::<BigEndian>()?;
        let mean_anomaly = reader.read_f32::<BigEndian>()?;
        let mean_motion = reader.read_f32::<BigEndian>()?;

        Ok(ADCSUpdate {
            timestamp,
            inclination,
            raan,
            eccentricity,
            perigee_arg,
            mean_anomaly,
            mean_motion,
        })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        writer.write_u32::<BigEndian>(self.timestamp)?;
        writer.write_f32::<BigEndian>(self.inclination)?;
        writer.write_f32::<BigEndian>(self.raan)?;
        writer.write_f32::<BigEndian>(self.eccentricity)?;
        writer.write_f32::<BigEndian>(self.perigee_arg)?;
        writer.write_f32::<BigEndian>(self.mean_anomaly)?;
        writer.write_f32::<BigEndian>(self.mean_motion)?;

        Ok(())
    }
}
