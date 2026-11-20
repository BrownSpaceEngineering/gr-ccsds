#include "cfdp.hpp"
#include "wire_constants.hpp"

#include <stdexcept>

namespace {
using Bytes = std::vector<uint8_t>;
namespace wire = cfdp::wire;

uint16_t crc16(const Bytes &bytes, size_t end) {
  uint16_t crc = wire::crc16_initial;
  for (size_t i = 0; i < end; ++i) {
    crc ^= static_cast<uint16_t>(bytes[i]) << 8;
    for (int bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>(
          (crc << 1) ^
          ((crc & wire::crc16_high_bit) ? wire::crc16_polynomial : 0));
  }
  return crc;
}

class Reader {
public:
  Reader(const Bytes &bytes, size_t end) : bytes_(bytes), end_(end) {}
  size_t pos() const { return pos_; }
  size_t remaining() const { return end_ - pos_; }
  void seek(size_t pos) {
    if (pos > end_)
      throw std::invalid_argument("CFDP field exceeds PDU");
    pos_ = pos;
  }
  uint8_t byte() {
    if (!remaining())
      throw std::invalid_argument("truncated CFDP PDU");
    return bytes_[pos_++];
  }
  uint64_t integer(unsigned width) {
    if (!width || width > wire::max_id_width || remaining() < width)
      throw std::invalid_argument("truncated CFDP integer");
    uint64_t value = 0;
    while (width--)
      value = (value << 8) | byte();
    return value;
  }
  Bytes take(size_t count) {
    if (remaining() < count)
      throw std::invalid_argument("truncated CFDP value");
    Bytes value(bytes_.begin() + pos_, bytes_.begin() + pos_ + count);
    pos_ += count;
    return value;
  }
  std::string lv() {
    auto value = take(byte());
    return {value.begin(), value.end()};
  }
  TLV tlv() {
    auto type = static_cast<TLV_Type>(byte());
    return {type, take(byte())};
  }

private:
  const Bytes &bytes_;
  size_t end_;
  size_t pos_ = 0;
};

Condition_Code condition(uint8_t n) {
  if (n == wire::first_reserved_condition || n == wire::last_reserved_condition)
    throw std::invalid_argument("reserved CFDP condition");
  return static_cast<Condition_Code>(n);
}

uint64_t fault_location(const TLV &tlv, unsigned width) {
  if (tlv.type != TLV_Type::EntityID || tlv.value.size() != width)
    throw std::invalid_argument("invalid CFDP fault location");
  uint64_t id = 0;
  for (auto byte : tlv.value)
    id = (id << 8) | byte;
  return id;
}
} // namespace

CFDP_PDU CFDP_PDU::from_bytes(const Bytes &bytes) {
  if (bytes.size() < wire::fixed_header_size + 3)
    throw std::invalid_argument("truncated CFDP header");
  const uint8_t flags = bytes[0], lengths = bytes[3];
  if ((flags >> wire::version_shift) != wire::protocol_version ||
      (lengths & wire::header_reserved_bit))
    throw std::invalid_argument("invalid CFDP header");
  const size_t entity_width =
      ((lengths >> wire::entity_width_shift) & wire::width_mask) + 1;
  const size_t sequence_width = (lengths & wire::width_mask) + 1;
  const size_t header_size =
      wire::fixed_header_size + 2 * entity_width + sequence_width;
  const size_t field_size = (static_cast<size_t>(bytes[1]) << 8) | bytes[2];
  if (bytes.size() != header_size + field_size)
    throw std::invalid_argument("CFDP PDU length mismatch");
  const bool crc = flags & wire::crc_flag;
  if (field_size < (crc ? wire::crc_width : 0U))
    throw std::invalid_argument("truncated CFDP CRC");
  const size_t end = bytes.size() - (crc ? wire::crc_width : 0);
  if (crc && crc16(bytes, end) !=
                 (static_cast<uint16_t>(bytes[end]) << 8 | bytes[end + 1]))
    throw PDU_CRC_Error();

  Reader in(bytes, end);
  CFDP_PDU pdu;
  auto &h = pdu.header;
  h.version = flags >> wire::version_shift;
  h.type = flags & wire::file_data_flag ? PDU_Type::FileData
                                        : PDU_Type::FileDirective;
  h.direction =
      static_cast<PDU_Direction>((flags >> wire::direction_shift) & 1);
  h.transmissionMode = static_cast<PDU_Transmission_Mode>(
      (flags >> wire::transmission_mode_shift) & 1);
  h.crcFlag = crc ? PDU_CRC_Flag::Present : PDU_CRC_Flag::NotPresent;
  h.largeFileFlag = flags & wire::large_file_flag
                        ? PDU_Large_File_Flag::LargeFile
                        : PDU_Large_File_Flag::SmallFile;
  h.fieldLength = field_size;
  h.segmentationControl =
      lengths & wire::segmentation_control_flag
          ? PDU_Segmentation_Control::BoundariesPreserved
          : PDU_Segmentation_Control::BoundariesNotPreserved;
  h.entityLength = entity_width;
  h.segmentMetadataFlag = lengths & wire::segment_metadata_flag
                              ? PDU_Segment_Metadata_Flag::Present
                              : PDU_Segment_Metadata_Flag::NotPresent;
  h.transactionSequenceNumberLength = sequence_width;
  in.seek(wire::fixed_header_size);
  h.sourceEntityID = in.integer(entity_width);
  h.transactionSequenceNumber = in.integer(sequence_width);
  h.destinationEntityID = in.integer(entity_width);
  const unsigned fss = flags & wire::large_file_flag
                           ? wire::large_file_size_width
                           : wire::small_file_size_width;
  if (h.type == PDU_Type::FileData) {
    FileData_PDU data;
    if (h.segmentMetadataFlag == PDU_Segment_Metadata_Flag::Present) {
      const auto field = in.byte();
      data.recordContinuationState = static_cast<Record_Continuation_State>(
          field >> wire::record_continuation_shift);
      data.segmentMetadata = in.take(field & wire::max_segment_metadata_length);
    }
    data.offset = in.integer(fss);
    data.data = in.take(in.remaining());
    pdu.body = std::move(data);
    return pdu;
  }
  if (lengths & wire::reserved_width_bits)
    throw std::invalid_argument("invalid directive header flags");
  switch (in.byte()) {
  case static_cast<uint8_t>(wire::Directive::Metadata): {
    Metadata_PDU m;
    const auto flags = in.byte();
    if (flags & wire::metadata_reserved_bits)
      throw std::invalid_argument("invalid Metadata flags");
    m.closureRequested = flags & wire::metadata_closure_flag;
    m.checksumType = flags & wire::low_nibble_mask;
    m.fileSize = in.integer(fss);
    m.sourceFileName = in.lv();
    m.destinationFileName = in.lv();
    while (in.remaining())
      m.options.push_back(in.tlv());
    pdu.body = std::move(m);
    break;
  }
  case static_cast<uint8_t>(wire::Directive::Eof): {
    EOF_PDU eof;
    const auto flags = in.byte();
    if (flags & wire::low_nibble_mask)
      throw std::invalid_argument("invalid EOF flags");
    eof.condition = condition(flags >> wire::condition_shift);
    eof.fileChecksum = in.integer(4);
    eof.fileSize = in.integer(fss);
    if (eof.condition != Condition_Code::NoError)
      eof.faultLocation = fault_location(in.tlv(), entity_width);
    pdu.body = eof;
    break;
  }
  case static_cast<uint8_t>(wire::Directive::Finished): {
    Finished_PDU f;
    const auto field = in.byte();
    if (field & wire::finished_spare_bit)
      throw std::invalid_argument("invalid Finished flags");
    f.condition = condition(field >> wire::condition_shift);
    f.delivery =
        static_cast<Delivery_Code>((field >> wire::delivery_shift) & 1);
    f.fileStatus = static_cast<File_Status>(field & 3);
    while (in.remaining()) {
      auto tlv = in.tlv();
      if (tlv.type == TLV_Type::EntityID &&
          f.condition != Condition_Code::NoError &&
          f.condition != Condition_Code::UnsupportedChecksumType &&
          !in.remaining())
        f.faultLocation = fault_location(tlv, entity_width);
      else if (tlv.type == TLV_Type::FilestoreResponse)
        f.filestoreResponses.push_back(std::move(tlv));
      else
        throw std::invalid_argument("invalid Finished TLV");
    }
    pdu.body = std::move(f);
    break;
  }
  case static_cast<uint8_t>(wire::Directive::Ack): {
    const auto directive = in.byte(), status = in.byte();
    if ((directive != wire::ack_eof_field &&
         directive != wire::ack_finished_field) ||
        (status & wire::ack_reserved_status_bits))
      throw std::invalid_argument("invalid ACK PDU");
    pdu.body = ACK_PDU{
        directive == wire::ack_eof_field ? Acknowledged_Directive::EndOfFile
                                         : Acknowledged_Directive::Finished,
        condition(status >> 4), static_cast<Transaction_Status>(status & 3)};
    break;
  }
  case static_cast<uint8_t>(wire::Directive::Nak): {
    NAK_PDU nak;
    nak.startOfScope = in.integer(fss);
    nak.endOfScope = in.integer(fss);
    if (nak.startOfScope > nak.endOfScope || in.remaining() % (2 * fss))
      throw std::invalid_argument("invalid NAK PDU");
    while (in.remaining()) {
      Segment_Request request{in.integer(fss), in.integer(fss)};
      if (request.startOffset > request.endOffset)
        throw std::invalid_argument("invalid NAK segment");
      nak.segmentRequests.push_back(request);
    }
    pdu.body = std::move(nak);
    break;
  }
  case static_cast<uint8_t>(wire::Directive::Prompt): {
    const auto value = in.byte();
    if (value != 0 && value != wire::prompt_keep_alive_flag)
      throw std::invalid_argument("invalid Prompt PDU");
    pdu.body = Prompt_PDU{value ? Prompt_Type::KeepAlive : Prompt_Type::NAK};
    break;
  }
  case static_cast<uint8_t>(wire::Directive::KeepAlive):
    pdu.body = KeepAlive_PDU{in.integer(fss)};
    break;
  default:
    throw std::invalid_argument("unsupported CFDP directive");
  }
  if (in.remaining())
    throw std::invalid_argument("trailing CFDP directive data");
  return pdu;
}

uint32_t modular_checksum(const Bytes &bytes) {
  uint32_t sum = 0;
  for (size_t i = 0; i < bytes.size(); i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4 && i + j < bytes.size(); ++j)
      word |= static_cast<uint32_t>(bytes[i + j]) << (24 - 8 * j);
    sum += word;
  }
  return sum;
}

bool checksum_type_supported(uint8_t type) {
  return type == wire::modular_checksum_type ||
         type == wire::crc32c_checksum_type ||
         type == wire::crc32_checksum_type || type == wire::null_checksum_type;
}

uint32_t file_checksum(const Bytes &bytes, uint8_t type) {
  if (type == wire::modular_checksum_type)
    return modular_checksum(bytes);
  if (type == wire::null_checksum_type)
    return 0;
  if (type != wire::crc32c_checksum_type && type != wire::crc32_checksum_type)
    throw std::invalid_argument("unsupported CFDP checksum type");
  // Reflected CRC-32C and CRC-32 polynomials (checksum IDs 2 and 3).
  constexpr uint32_t crc32c_polynomial = 0x82f63b78u;
  constexpr uint32_t crc32_polynomial = 0xedb88320u;
  const uint32_t polynomial =
      type == wire::crc32c_checksum_type ? crc32c_polynomial : crc32_polynomial;
  constexpr uint32_t crc32_initial = 0xffffffffu;
  uint32_t crc = crc32_initial;
  for (const uint8_t octet : bytes) {
    crc ^= octet;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? polynomial : 0u);
  }
  return crc ^ crc32_initial;
}
