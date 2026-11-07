#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// CCSDS 727.0-B-5, sections 5 and 6. Keep values shared by the PDU encoder,
// decoder, and user operations here so both directions use the same layout.
namespace cfdp::wire {
inline constexpr uint8_t protocol_version = 1;
inline constexpr size_t max_lv_length = std::numeric_limits<uint8_t>::max();
inline constexpr size_t max_data_field_length =
    std::numeric_limits<uint16_t>::max();
inline constexpr uint8_t max_id_width = 8;
inline constexpr size_t fixed_header_size = 4;
inline constexpr size_t directive_code_width = 1;
inline constexpr size_t data_field_length_width = sizeof(uint16_t);
inline constexpr size_t max_pdu_size =
    max_data_field_length + fixed_header_size + 3 * max_id_width;
inline constexpr unsigned small_file_size_width = 4;
inline constexpr unsigned large_file_size_width = 8;
inline constexpr size_t crc_width = 2;
inline constexpr size_t max_segment_metadata_length = 63;

enum class Directive : uint8_t {
  Eof = 0x04,
  Finished = 0x05,
  Ack = 0x06,
  Metadata = 0x07,
  Nak = 0x08,
  Prompt = 0x09,
  KeepAlive = 0x0c,
};

// The first header octet: version occupies bits 7..5.
inline constexpr unsigned version_shift = 5;
inline constexpr uint8_t file_data_flag = 0x10;
inline constexpr unsigned direction_shift = 3;
inline constexpr unsigned transmission_mode_shift = 2;
inline constexpr uint8_t crc_flag = 0x02;
inline constexpr uint8_t large_file_flag = 0x01;

// The fourth header octet: entity and sequence widths are stored minus one.
inline constexpr uint8_t segmentation_control_flag = 0x80;
inline constexpr unsigned entity_width_shift = 4;
inline constexpr uint8_t segment_metadata_flag = 0x08;
inline constexpr uint8_t width_mask = 0x07;
inline constexpr uint8_t reserved_width_bits = 0x88;

inline constexpr uint8_t metadata_closure_flag = 0x40;
inline constexpr uint8_t metadata_reserved_bits = 0xb0;
inline constexpr uint8_t prompt_keep_alive_flag = 0x80;
inline constexpr uint8_t header_reserved_bit = 0x04;
inline constexpr uint8_t finished_spare_bit = 0x08;
inline constexpr uint8_t ack_eof_field = 0x40;
inline constexpr uint8_t ack_finished_field = 0x51;
inline constexpr uint8_t ack_reserved_status_bits = 0x0c;
inline constexpr uint8_t low_nibble_mask = 0x0f;
inline constexpr unsigned high_nibble_shift = 4;
inline constexpr unsigned record_continuation_shift = 6;
inline constexpr unsigned condition_shift = high_nibble_shift;
inline constexpr unsigned delivery_shift = 2;
inline constexpr uint16_t crc16_initial = 0xffff;
inline constexpr uint16_t crc16_high_bit = 0x8000;
inline constexpr uint16_t crc16_polynomial = 0x1021;
inline constexpr uint8_t modular_checksum_type = 0;
inline constexpr uint8_t crc32c_checksum_type = 2;
inline constexpr uint8_t crc32_checksum_type = 3;
inline constexpr uint8_t null_checksum_type = 15;
inline constexpr uint8_t first_reserved_condition = 12;
inline constexpr uint8_t last_reserved_condition = 13;
} // namespace cfdp::wire
