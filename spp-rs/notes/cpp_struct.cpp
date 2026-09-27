#include <cstdint>

// https://ccsds.org/Pubs/133x0b2e2.pdf#%5B%7B%22num%22%3A89%2C%22gen%22%3A0%7D%2C%7B%22name%22%3A%22XYZ%22%7D%2C78%2C728%2C0%5D

struct SpacePacketPrimaryHeader {
    /*
    ** 3 bits - Packet Version Number
    **
    ** Packet Identification Type (13 bits)
    ** 1 bit - Packet Type (0 = telemetry, 1 = telecommand)
    ** 1 bit - Secondary Header Flag (bool if present)
    ** 11 bits - Application Process Identifier
    */
    uint16_t packet_metadata;

    /*
    ** Packet Sequence Control
    ** 2 bits - Sequence Flags (optional, can be used to indicate that packet is part of larger set of data)
    ** - 00 if continuation segment
    ** - 01 if first segment
    ** - 10 if last segment
    ** - 11 if unsegmented
    ** 14 bits - Packet Sequence Count or Packet Name
    ** - Continuous (mod 16384)
    ** - Has to be count for telemetry (can be name for telecommand)
    */
    uint16_t packet_sequence_control;

    uint16_t packet_data_length;
};

struct SpacePacketSecondaryHeader {
    // Can contain either time code, ancillary data field, or neither
};

struct SpacePacket {
    SpacePacketPrimaryHeader primary_header;

    // Optional
    SpacePacketSecondaryHeader secondary_header;

    void* user_data;
};
