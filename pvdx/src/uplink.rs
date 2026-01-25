use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::io::{Cursor, Read, Write};

#[repr(C, align(4))]
pub struct UplinkPacket {
    pub header: UplinkPacketHeader, // Callsign, total size, number of commands
    pub cmd_headers: Vec<UplinkCommandHeader>, // Command type, size per command
    pub commands: Vec<Command>,     // Command data
}

impl UplinkPacket {
    pub fn new(callsign: [u8; 6], commands: Vec<Command>) -> Self {
        let n_cmds = commands.len() as u16;
        let header_size = 16;
        let cmd_headers_size = n_cmds as u32 * 8;
        let data_size: u32 = commands.iter().map(|cmd| cmd.data_size()).sum();
        let total_size = header_size + cmd_headers_size + data_size;

        let header = UplinkPacketHeader {
            callsign,
            _pad1: [0; 2],
            size: total_size,
            n_cmds,
            _pad2: [0; 2],
        };

        let cmd_headers = commands
            .iter()
            .map(|cmd| UplinkCommandHeader {
                cmd_type: cmd.cmd_type_to(),
                _pad: [0; 2],
                cmd_sz: cmd.data_size(),
            })
            .collect();

        UplinkPacket {
            header,
            cmd_headers,
            commands,
        }
    }

    pub fn from_bytes(bytes: &[u8]) -> std::io::Result<Self> {
        let mut reader = Cursor::new(bytes);

        let header = UplinkPacketHeader::from_reader(&mut reader)?;

        let mut cmd_headers = Vec::with_capacity(header.n_cmds as usize);
        for _ in 0..header.n_cmds {
            let cmd_header = UplinkCommandHeader::from_reader(&mut reader)?;
            cmd_headers.push(cmd_header);
        }

        let mut commands = Vec::with_capacity(header.n_cmds as usize);
        for cmd_header in &cmd_headers {
            let mut cmd_data = vec![0u8; cmd_header.cmd_sz as usize];
            reader.read_exact(&mut cmd_data)?;
            let command = Command::from_bytes(cmd_header.cmd_type, &cmd_data)?;
            commands.push(command);
        }

        Ok(UplinkPacket {
            header,
            cmd_headers,
            commands,
        })
    }

    pub fn to_bytes(&self) -> std::io::Result<Vec<u8>> {
        let mut writer = Cursor::new(Vec::new());

        self.header.to_writer(&mut writer)?;

        for cmd_header in &self.cmd_headers {
            cmd_header.to_writer(&mut writer)?;
        }

        for cmd in &self.commands {
            let cmd_bytes = cmd.to_bytes()?;
            writer.write_all(&cmd_bytes)?;
        }

        Ok(writer.into_inner())
    }
}

#[derive(Debug, PartialEq, Clone)]
pub enum Command {
    DeviceEnable { device: u8 },
    DeviceDisable { device: u8 },
    DeviceBreak { device: u8 },
    DeviceUnbreak { device: u8 },
    DisplayUpdate { bitmap: [u8; 8192] },
    Sleep { duration: u32 },
    Reboot,
    PictureSend { count: u16 },
    PictureCapture { timestamp: u32 },
    SetTime { timestamp: u32 },
    SetPowerMode { mode: u8 },
    ADCSSetOpMode { mode: u8 },
    ADCSSetKeplers { update: ADCSUpdate },
}

impl Command {
    pub fn cmd_type_to(&self) -> u16 {
        match self {
            Command::DeviceEnable { .. } => 0,
            Command::DeviceDisable { .. } => 1,
            Command::DeviceBreak { .. } => 2,
            Command::DeviceUnbreak { .. } => 3,
            Command::DisplayUpdate { .. } => 4,
            Command::Sleep { .. } => 5,
            Command::Reboot => 6,
            Command::PictureCapture { .. } => 7,
            Command::PictureSend { .. } => 8,
            Command::SetTime { .. } => 9,
            Command::SetPowerMode { .. } => 10,
            Command::ADCSSetOpMode { .. } => 11,
            Command::ADCSSetKeplers { .. } => 12,
        }
    }

    pub fn data_size(&self) -> u32 {
        match self {
            Command::DeviceEnable { .. } => 1,
            Command::DeviceDisable { .. } => 1,
            Command::DeviceBreak { .. } => 1,
            Command::DeviceUnbreak { .. } => 1,
            Command::DisplayUpdate { .. } => 8192,
            Command::Sleep { .. } => 4,
            Command::Reboot => 0,
            Command::PictureSend { .. } => 2,
            Command::PictureCapture { .. } => 4,
            Command::SetTime { .. } => 4,
            Command::SetPowerMode { .. } => 1,
            Command::ADCSSetOpMode { .. } => 1,
            Command::ADCSSetKeplers { .. } => 28,
        }
    }

    pub fn to_bytes(&self) -> std::io::Result<Vec<u8>> {
        let mut writer = Cursor::new(Vec::new());

        match self {
            Command::DeviceEnable { device } => writer.write_u8(*device)?,
            Command::DeviceDisable { device } => writer.write_u8(*device)?,
            Command::DeviceBreak { device } => writer.write_u8(*device)?,
            Command::DeviceUnbreak { device } => writer.write_u8(*device)?,
            Command::DisplayUpdate { bitmap } => writer.write_all(bitmap)?,
            Command::Sleep { duration } => writer.write_u32::<BigEndian>(*duration)?,
            Command::Reboot => {}
            Command::PictureSend { count } => writer.write_u16::<BigEndian>(*count)?,
            Command::PictureCapture { timestamp } => writer.write_u32::<BigEndian>(*timestamp)?,
            Command::SetTime { timestamp } => writer.write_u32::<BigEndian>(*timestamp)?,
            Command::SetPowerMode { mode } => writer.write_u8(*mode)?,
            Command::ADCSSetOpMode { mode } => writer.write_u8(*mode)?,
            Command::ADCSSetKeplers { update } => update.to_writer(&mut writer)?,
        }

        Ok(writer.into_inner())
    }

    pub fn from_bytes(cmd_type: u16, bytes: &[u8]) -> std::io::Result<Self> {
        let mut reader = Cursor::new(bytes);

        Ok(match cmd_type {
            0 => {
                let device = reader.read_u8()?;
                Command::DeviceEnable { device }
            }
            1 => {
                let device = reader.read_u8()?;
                Command::DeviceDisable { device }
            }
            2 => {
                let device = reader.read_u8()?;
                Command::DeviceBreak { device }
            }
            3 => {
                let device = reader.read_u8()?;
                Command::DeviceUnbreak { device }
            }
            4 => {
                let mut bitmap = [0u8; 8192];
                reader.read_exact(&mut bitmap)?;
                Command::DisplayUpdate { bitmap }
            }
            5 => {
                let duration = reader.read_u32::<BigEndian>()?;
                Command::Sleep { duration }
            }
            6 => Command::Reboot,
            7 => {
                let timestamp = reader.read_u32::<BigEndian>()?;
                Command::PictureCapture { timestamp }
            }
            8 => {
                let count = reader.read_u16::<BigEndian>()?;
                Command::PictureSend { count }
            }
            9 => {
                let timestamp = reader.read_u32::<BigEndian>()?;
                Command::SetTime { timestamp }
            }
            10 => {
                let mode = reader.read_u8()?;
                Command::SetPowerMode { mode }
            }
            11 => {
                let mode = reader.read_u8()?;
                Command::ADCSSetOpMode { mode }
            }
            12 => {
                let update = ADCSUpdate::from_reader(&mut reader)?;
                Command::ADCSSetKeplers { update }
            }
            _ => {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::InvalidData,
                    "Invalid command type",
                ));
            }
        })
    }
}

#[repr(C, align(4))]
#[derive(Debug, PartialEq, Clone, Copy)]
pub struct UplinkPacketHeader {
    pub callsign: [u8; 6],
    pub _pad1: [u8; 2],
    pub size: u32,
    pub n_cmds: u16,
    pub _pad2: [u8; 2],
}

impl UplinkPacketHeader {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let mut callsign = [0u8; 6];
        reader.read_exact(&mut callsign)?;
        let mut _pad1 = [0u8; 2];
        reader.read_exact(&mut _pad1)?;
        let size = reader.read_u32::<BigEndian>()?;
        let n_cmds = reader.read_u16::<BigEndian>()?;
        let mut _pad2 = [0u8; 2];
        reader.read_exact(&mut _pad2)?;

        Ok(UplinkPacketHeader {
            callsign,
            _pad1,
            size,
            n_cmds,
            _pad2,
        })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        writer.write_all(&self.callsign)?;
        writer.write_all(&self._pad1)?;
        writer.write_u32::<BigEndian>(self.size)?;
        writer.write_u16::<BigEndian>(self.n_cmds)?;
        writer.write_all(&self._pad2)?;

        Ok(())
    }
}

#[repr(C, align(4))]
#[derive(Debug, PartialEq, Clone, Copy)]
pub struct UplinkCommandHeader {
    pub cmd_type: u16,
    pub _pad: [u8; 2],
    pub cmd_sz: u32,
}

impl UplinkCommandHeader {
    pub fn from_reader<R: Read>(reader: &mut R) -> std::io::Result<Self> {
        let cmd_type = reader.read_u16::<BigEndian>()?;
        let mut _pad = [0u8; 2];
        reader.read_exact(&mut _pad)?;
        let cmd_sz = reader.read_u32::<BigEndian>()?;

        Ok(UplinkCommandHeader {
            cmd_type,
            _pad,
            cmd_sz,
        })
    }

    pub fn to_writer<W: Write>(&self, writer: &mut W) -> std::io::Result<()> {
        writer.write_u16::<BigEndian>(self.cmd_type)?;
        writer.write_all(&self._pad)?;
        writer.write_u32::<BigEndian>(self.cmd_sz)?;

        Ok(())
    }
}

#[repr(C, align(4))]
#[derive(Debug, PartialEq, Clone, Copy)]
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
