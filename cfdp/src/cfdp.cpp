#include "cfdp.hpp"
#include "wire_constants.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using Bytes = std::vector<uint8_t>;
namespace wire = cfdp::wire;

void append_uint(Bytes &out, uint64_t value, unsigned octets) {
  if (octets < 1 || octets > wire::max_id_width ||
      (octets < wire::max_id_width && value >= (uint64_t{1} << (8 * octets))))
    throw std::invalid_argument("CFDP integer does not fit its field");
  for (unsigned i = octets; i > 0; --i)
    out.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
}

void append_fss(Bytes &out, uint64_t value, bool large) {
  append_uint(out, value,
              large ? wire::large_file_size_width
                    : wire::small_file_size_width);
}

void append_lv(Bytes &out, const std::string &value) {
  if (value.size() > wire::max_lv_length)
    throw std::length_error("CFDP file name exceeds 255 octets");
  out.push_back(static_cast<uint8_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void append_tlv(Bytes &out, const TLV &tlv) {
  if (tlv.value.size() > wire::max_lv_length)
    throw std::length_error("CFDP TLV value exceeds 255 octets");
  out.push_back(static_cast<uint8_t>(tlv.type));
  out.push_back(static_cast<uint8_t>(tlv.value.size()));
  out.insert(out.end(), tlv.value.begin(), tlv.value.end());
}

uint8_t condition(Condition_Code code) {
  const auto value = static_cast<uint8_t>(code);
  if (value > wire::low_nibble_mask ||
      value == wire::first_reserved_condition ||
      value == wire::last_reserved_condition)
    throw std::invalid_argument("invalid CFDP condition code");
  return value;
}

uint16_t crc16(const Bytes &bytes) {
  uint16_t crc = wire::crc16_initial;
  for (uint8_t byte : bytes) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>(
          (crc << 1) ^
          ((crc & wire::crc16_high_bit) ? wire::crc16_polynomial : 0));
  }
  return crc;
}
} // namespace

TLV TLV::message_to_user(std::vector<uint8_t> message) {
  return {TLV_Type::MessageToUser, std::move(message)};
}

TLV TLV::flow_label(std::vector<uint8_t> label) {
  return {TLV_Type::FlowLabel, std::move(label)};
}

TLV TLV::entity_id(uint64_t id, uint8_t length) {
  Bytes value;
  append_uint(value, id, length);
  return {TLV_Type::EntityID, std::move(value)};
}

namespace {
bool needs_second_name(Filestore_Action action) {
  return action == Filestore_Action::RenameFile ||
         action == Filestore_Action::AppendFile ||
         action == Filestore_Action::ReplaceFile;
}

void validate_filestore_names(Filestore_Action action,
                              const std::optional<std::string> &secondName) {
  if (static_cast<uint8_t>(action) >
          static_cast<uint8_t>(Filestore_Action::DenyDirectory) ||
      needs_second_name(action) != secondName.has_value())
    throw std::invalid_argument(
        "incorrect filestore action or second file name");
}
} // namespace

TLV filestore_request(Filestore_Action action, std::string firstName,
                      std::optional<std::string> secondName) {
  validate_filestore_names(action, secondName);
  Bytes value{static_cast<uint8_t>(static_cast<uint8_t>(action)
                                   << wire::high_nibble_shift)};
  append_lv(value, firstName);
  if (secondName)
    append_lv(value, *secondName);
  return {TLV_Type::FilestoreRequest, std::move(value)};
}

TLV filestore_response(Filestore_Action action, uint8_t status,
                       std::string firstName,
                       std::optional<std::string> secondName,
                       std::string message) {
  validate_filestore_names(action, secondName);
  if (status > wire::low_nibble_mask)
    throw std::invalid_argument("filestore status exceeds four bits");
  Bytes value{static_cast<uint8_t>(
      (static_cast<uint8_t>(action) << wire::high_nibble_shift) | status)};
  append_lv(value, firstName);
  if (secondName)
    append_lv(value, *secondName);
  append_lv(value, message);
  return {TLV_Type::FilestoreResponse, std::move(value)};
}

TLV fault_handler_override(Condition_Code fault, Fault_Handler handler) {
  if (fault == Condition_Code::NoError ||
      fault == Condition_Code::SuspendRequested ||
      fault == Condition_Code::CancelRequested ||
      static_cast<uint8_t>(handler) < 1 || static_cast<uint8_t>(handler) > 4)
    throw std::invalid_argument("invalid fault handler override");
  return {TLV_Type::FaultHandlerOverrides,
          {static_cast<uint8_t>((condition(fault) << wire::condition_shift) |
                                static_cast<uint8_t>(handler))}};
}

std::vector<uint8_t> CFDP_PDU::to_bytes() const {
  if (header.version != wire::protocol_version)
    throw std::invalid_argument("unsupported CFDP version");
  if (header.entityLength < 1 || header.entityLength > wire::max_id_width ||
      header.transactionSequenceNumberLength < 1 ||
      header.transactionSequenceNumberLength > wire::max_id_width)
    throw std::invalid_argument("CFDP ID lengths must be 1 to 8 octets");
  if (static_cast<uint8_t>(header.direction) > 1 ||
      static_cast<uint8_t>(header.transmissionMode) > 1 ||
      static_cast<uint8_t>(header.crcFlag) > 1 ||
      static_cast<uint8_t>(header.largeFileFlag) > 1 ||
      static_cast<uint8_t>(header.segmentationControl) > 1 ||
      static_cast<uint8_t>(header.segmentMetadataFlag) > 1)
    throw std::invalid_argument("invalid CFDP header flag");

  const bool large = header.largeFileFlag == PDU_Large_File_Flag::LargeFile;
  const bool crc_present = header.crcFlag == PDU_CRC_Flag::Present;
  const bool file_data = std::holds_alternative<FileData_PDU>(body);
  Bytes data;
  bool segment_metadata = false;

  std::visit(
      [&](const auto &pdu) {
        using T = std::decay_t<decltype(pdu)>;
        if constexpr (std::is_same_v<T, Metadata_PDU>) {
          data.push_back(static_cast<uint8_t>(wire::Directive::Metadata));
          if (pdu.checksumType > wire::low_nibble_mask)
            throw std::invalid_argument("CFDP checksum type exceeds four bits");
          if (header.transmissionMode == PDU_Transmission_Mode::Acknowledged &&
              pdu.closureRequested)
            throw std::invalid_argument(
                "closure flag must be zero in acknowledged mode");
          data.push_back(static_cast<uint8_t>(
              (pdu.closureRequested ? wire::metadata_closure_flag : 0) |
              pdu.checksumType));
          append_fss(data, pdu.fileSize, large);
          append_lv(data, pdu.sourceFileName);
          append_lv(data, pdu.destinationFileName);
          for (const auto &tlv : pdu.options) {
            if (tlv.type != TLV_Type::FilestoreRequest &&
                tlv.type != TLV_Type::MessageToUser &&
                tlv.type != TLV_Type::FaultHandlerOverrides &&
                tlv.type != TLV_Type::FlowLabel)
              throw std::invalid_argument(
                  "TLV type is not a Metadata PDU option");
            append_tlv(data, tlv);
          }
        } else if constexpr (std::is_same_v<T, FileData_PDU>) {
          segment_metadata = header.segmentMetadataFlag ==
                                 PDU_Segment_Metadata_Flag::Present ||
                             !pdu.segmentMetadata.empty();
          if (segment_metadata) {
            if (pdu.segmentMetadata.size() > wire::max_segment_metadata_length)
              throw std::length_error(
                  "CFDP segment metadata exceeds 63 octets");
            const auto state =
                static_cast<uint8_t>(pdu.recordContinuationState);
            if (state > 3)
              throw std::invalid_argument(
                  "invalid CFDP record continuation state");
            data.push_back(static_cast<uint8_t>(
                (state << wire::record_continuation_shift) |
                pdu.segmentMetadata.size()));
            data.insert(data.end(), pdu.segmentMetadata.begin(),
                        pdu.segmentMetadata.end());
          } else if (pdu.recordContinuationState !=
                     Record_Continuation_State::NoBoundary) {
            throw std::invalid_argument(
                "record continuation state requires segment metadata flag");
          }
          append_fss(data, pdu.offset, large);
          data.insert(data.end(), pdu.data.begin(), pdu.data.end());
        } else if constexpr (std::is_same_v<T, EOF_PDU>) {
          data.push_back(static_cast<uint8_t>(wire::Directive::Eof));
          data.push_back(static_cast<uint8_t>(condition(pdu.condition)
                                              << wire::condition_shift));
          append_uint(data, pdu.fileChecksum, 4);
          append_fss(data, pdu.fileSize, large);
          if ((pdu.condition == Condition_Code::NoError) != !pdu.faultLocation)
            throw std::invalid_argument(
                "EOF fault location must match condition code");
          if (pdu.faultLocation)
            append_tlv(data,
                       TLV::entity_id(*pdu.faultLocation, header.entityLength));
        } else if constexpr (std::is_same_v<T, Finished_PDU>) {
          if (static_cast<uint8_t>(pdu.delivery) > 1 ||
              static_cast<uint8_t>(pdu.fileStatus) > 3)
            throw std::invalid_argument("invalid Finished PDU status");
          data.push_back(static_cast<uint8_t>(wire::Directive::Finished));
          data.push_back(static_cast<uint8_t>(
              (condition(pdu.condition) << wire::condition_shift) |
              (static_cast<uint8_t>(pdu.delivery) << wire::delivery_shift) |
              static_cast<uint8_t>(pdu.fileStatus)));
          for (const auto &tlv : pdu.filestoreResponses) {
            if (tlv.type != TLV_Type::FilestoreResponse)
              throw std::invalid_argument(
                  "Finished PDU accepts only filestore response TLVs");
            append_tlv(data, tlv);
          }
          const bool needs_location =
              pdu.condition != Condition_Code::NoError &&
              pdu.condition != Condition_Code::UnsupportedChecksumType;
          if (needs_location != pdu.faultLocation.has_value())
            throw std::invalid_argument(
                "Finished fault location must match condition code");
          if (pdu.faultLocation)
            append_tlv(data,
                       TLV::entity_id(*pdu.faultLocation, header.entityLength));
        } else if constexpr (std::is_same_v<T, ACK_PDU>) {
          if (pdu.directive != Acknowledged_Directive::EndOfFile &&
              pdu.directive != Acknowledged_Directive::Finished)
            throw std::invalid_argument(
                "only EOF and Finished PDUs may be acknowledged");
          if (static_cast<uint8_t>(pdu.transactionStatus) > 3)
            throw std::invalid_argument("invalid ACK transaction status");
          data.push_back(static_cast<uint8_t>(wire::Directive::Ack));
          data.push_back(static_cast<uint8_t>(
              (static_cast<uint8_t>(pdu.directive) << wire::condition_shift) |
              (pdu.directive == Acknowledged_Directive::Finished ? 1 : 0)));
          data.push_back(static_cast<uint8_t>(
              (condition(pdu.condition) << wire::condition_shift) |
              static_cast<uint8_t>(pdu.transactionStatus)));
        } else if constexpr (std::is_same_v<T, NAK_PDU>) {
          data.push_back(static_cast<uint8_t>(wire::Directive::Nak));
          append_fss(data, pdu.startOfScope, large);
          append_fss(data, pdu.endOfScope, large);
          if (pdu.startOfScope > pdu.endOfScope)
            throw std::invalid_argument("NAK scope start exceeds end");
          for (const auto &request : pdu.segmentRequests) {
            if (request.startOffset > request.endOffset)
              throw std::invalid_argument("NAK request start exceeds end");
            append_fss(data, request.startOffset, large);
            append_fss(data, request.endOffset, large);
          }
        } else if constexpr (std::is_same_v<T, Prompt_PDU>) {
          if (static_cast<uint8_t>(pdu.response) > 1)
            throw std::invalid_argument("invalid Prompt response");
          data.push_back(static_cast<uint8_t>(wire::Directive::Prompt));
          data.push_back(pdu.response == Prompt_Type::KeepAlive
                             ? wire::prompt_keep_alive_flag
                             : 0);
        } else if constexpr (std::is_same_v<T, KeepAlive_PDU>) {
          data.push_back(static_cast<uint8_t>(wire::Directive::KeepAlive));
          append_fss(data, pdu.progress, large);
        }
      },
      body);

  const size_t length = data.size() + (crc_present ? wire::crc_width : 0);
  if (length > wire::max_data_field_length)
    throw std::length_error("CFDP PDU data field exceeds 65535 octets");

  Bytes out;
  out.reserve(wire::fixed_header_size + 2 * header.entityLength +
              header.transactionSequenceNumberLength + length);
  out.push_back(static_cast<uint8_t>(
      (header.version << wire::version_shift) |
      (file_data ? wire::file_data_flag : 0) |
      (static_cast<uint8_t>(header.direction) << wire::direction_shift) |
      (static_cast<uint8_t>(header.transmissionMode)
       << wire::transmission_mode_shift) |
      (crc_present ? wire::crc_flag : 0) |
      (large ? wire::large_file_flag : 0)));
  append_uint(out, length, wire::data_field_length_width);
  out.push_back(static_cast<uint8_t>(
      (file_data && header.segmentationControl ==
                        PDU_Segmentation_Control::BoundariesPreserved
           ? wire::segmentation_control_flag
           : 0) |
      ((header.entityLength - 1) << wire::entity_width_shift) |
      (segment_metadata ? wire::segment_metadata_flag : 0) |
      (header.transactionSequenceNumberLength - 1)));
  append_uint(out, header.sourceEntityID, header.entityLength);
  append_uint(out, header.transactionSequenceNumber,
              header.transactionSequenceNumberLength);
  append_uint(out, header.destinationEntityID, header.entityLength);
  out.insert(out.end(), data.begin(), data.end());
  if (crc_present)
    append_uint(out, crc16(out), wire::crc_width);
  return out;
}
