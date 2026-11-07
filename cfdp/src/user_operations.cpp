#include "user_operations.hpp"
#include "entity.hpp"
#include "wire_constants.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cfdp {
namespace {
enum class MessageType : uint8_t {
  ProxyPut = 0x00,
  ProxyUserMessage = 0x01,
  ProxyFilestoreRequest = 0x02,
  ProxyFaultOverride = 0x03,
  ProxyTransmissionMode = 0x04,
  ProxyFlowLabel = 0x05,
  ProxySegmentationControl = 0x06,
  ProxyPutResponse = 0x07,
  ProxyFilestoreResponse = 0x08,
  ProxyCancel = 0x09,
  OriginatingTransactionId = 0x0a,
  ProxyClosureRequest = 0x0b,
  DirectoryListingRequest = 0x10,
  DirectoryListingResponse = 0x11,
  RemoteStatusRequest = 0x20,
  RemoteStatusResponse = 0x21,
  RemoteSuspendRequest = 0x30,
  RemoteSuspendResponse = 0x31,
  RemoteResumeRequest = 0x38,
  RemoteResumeResponse = 0x39,
};

constexpr size_t reserved_header_size = 5;
constexpr uint8_t reserved_width_bits = wire::reserved_width_bits;
constexpr uint8_t suspended_flag = 0x80;
constexpr uint8_t listing_failed_flag = 0x80;
constexpr unsigned suspension_status_shift = 5;
constexpr unsigned report_status_shift = 6;

class MessageReader {
public:
  explicit MessageReader(const Bytes &bytes) : bytes_(bytes) {}

  uint8_t octet() {
    if (pos_ == bytes_.size())
      throw std::invalid_argument("truncated CFDP user operation");
    return bytes_[pos_++];
  }

  uint64_t integer(size_t width) {
    if (bytes_.size() - pos_ < width)
      throw std::invalid_argument("truncated CFDP user operation integer");
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i)
      result = (result << 8) | octet();
    return result;
  }

  std::string name() {
    const auto length = octet();
    if (bytes_.size() - pos_ < length)
      throw std::invalid_argument("truncated CFDP user operation name");
    std::string result(bytes_.begin() + pos_, bytes_.begin() + pos_ + length);
    pos_ += length;
    return result;
  }

  Bytes value() {
    const auto length = octet();
    if (bytes_.size() - pos_ < length)
      throw std::invalid_argument("truncated CFDP user operation value");
    Bytes result(bytes_.begin() + pos_, bytes_.begin() + pos_ + length);
    pos_ += length;
    return result;
  }

  struct TransactionId {
    uint64_t source_id;
    uint64_t sequence;
    Bytes encoded;
  };

  TransactionId transaction_id() {
    const size_t start = pos_;
    const auto widths = octet();
    if (widths & reserved_width_bits)
      throw std::invalid_argument("invalid CFDP transaction ID widths");
    const size_t entity_width =
        ((widths >> wire::entity_width_shift) & wire::width_mask) + 1;
    const size_t sequence_width = (widths & wire::width_mask) + 1;
    const auto source_id = integer(entity_width);
    const auto sequence = integer(sequence_width);
    return {source_id, sequence,
            Bytes(bytes_.begin() + start, bytes_.begin() + pos_)};
  }

  void require_end() const {
    if (pos_ != bytes_.size())
      throw std::invalid_argument("extra CFDP user operation data");
  }

private:
  const Bytes &bytes_;
  size_t pos_ = reserved_header_size;
};

bool is_reserved_message(const TLV &tlv) {
  return tlv.type == TLV_Type::MessageToUser &&
         tlv.value.size() >= reserved_header_size &&
         std::equal(tlv.value.begin(), tlv.value.begin() + 4, "cfdp");
}

Bytes reserved_message(uint8_t type, Bytes content) {
  Bytes message{'c', 'f', 'd', 'p', type};
  message.insert(message.end(), content.begin(), content.end());
  return message;
}
Bytes reserved_message(MessageType type, Bytes content) {
  return reserved_message(static_cast<uint8_t>(type), std::move(content));
}
void append_integer(Bytes &bytes, uint64_t value, uint8_t width) {
  for (unsigned i = width; i > 0; --i)
    bytes.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
}
bool fits_width(uint64_t value, uint8_t width) {
  return width == wire::max_id_width || value < (uint64_t{1} << (8 * width));
}
void append_lv(Bytes &bytes, const std::string &value) {
  if (value.size() > wire::max_lv_length)
    throw std::length_error("CFDP user operation name too long");
  bytes.push_back(static_cast<uint8_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}
void append_lv(Bytes &bytes, const Bytes &value) {
  if (value.size() > wire::max_lv_length)
    throw std::length_error("CFDP user operation value too long");
  bytes.push_back(static_cast<uint8_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}
Bytes transaction_id_content(uint64_t entity, uint64_t sequence,
                             uint8_t entity_width, uint8_t sequence_width) {
  Bytes content{static_cast<uint8_t>(
      ((entity_width - 1) << wire::entity_width_shift) | (sequence_width - 1))};
  append_integer(content, entity, entity_width);
  append_integer(content, sequence, sequence_width);
  return content;
}
Bytes originating_id(uint64_t entity, uint64_t sequence, uint8_t entity_width,
                     uint8_t sequence_width) {
  return reserved_message(
      MessageType::OriginatingTransactionId,
      transaction_id_content(entity, sequence, entity_width, sequence_width));
}
} // namespace

uint64_t UserOperations::proxy_put(Entity &entity, uint64_t respondent_id,
                                   uint64_t beneficiary_id,
                                   std::string source_name,
                                   std::string destination_name,
                                   ProxyOptions proxy_options) {
  Bytes content;
  content.push_back(entity.config_.id_width);
  append_integer(content, beneficiary_id, entity.config_.id_width);
  append_lv(content, source_name);
  append_lv(content, destination_name);
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(
      reserved_message(MessageType::ProxyPut, std::move(content))));
  if (proxy_options.mode) {
    const auto mode = static_cast<uint8_t>(*proxy_options.mode);
    if (mode > 1)
      throw std::invalid_argument("invalid proxy transmission mode");
    options.metadata_options.push_back(TLV::message_to_user(
        reserved_message(MessageType::ProxyTransmissionMode, {mode})));
  }
  if (proxy_options.closure_requested)
    options.metadata_options.push_back(TLV::message_to_user(reserved_message(
        MessageType::ProxyClosureRequest,
        {static_cast<uint8_t>(*proxy_options.closure_requested)})));
  for (const auto &message : proxy_options.user_messages) {
    Bytes payload;
    append_lv(payload, message);
    options.metadata_options.push_back(TLV::message_to_user(
        reserved_message(MessageType::ProxyUserMessage, std::move(payload))));
  }
  for (const auto &request : proxy_options.filestore_requests) {
    if (request.type != TLV_Type::FilestoreRequest)
      throw std::invalid_argument("proxy option is not a filestore request");
    Filestore::validate_request(request);
    Bytes payload;
    append_lv(payload, request.value);
    options.metadata_options.push_back(TLV::message_to_user(reserved_message(
        MessageType::ProxyFilestoreRequest, std::move(payload))));
  }
  for (const auto &override : proxy_options.fault_handler_overrides) {
    if (override.type != TLV_Type::FaultHandlerOverrides ||
        override.value.size() != 1)
      throw std::invalid_argument("invalid proxy fault handler override");
    options.metadata_options.push_back(TLV::message_to_user(
        reserved_message(MessageType::ProxyFaultOverride, override.value)));
  }
  if (!proxy_options.flow_label.empty()) {
    Bytes payload;
    append_lv(payload, proxy_options.flow_label);
    options.metadata_options.push_back(TLV::message_to_user(
        reserved_message(MessageType::ProxyFlowLabel, std::move(payload))));
  }
  if (proxy_options.preserve_record_boundaries)
    options.metadata_options.push_back(TLV::message_to_user(reserved_message(
        MessageType::ProxySegmentationControl,
        {static_cast<uint8_t>(
            *proxy_options.preserve_record_boundaries ? 0 : 1)})));
  return entity.put(respondent_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

uint64_t UserOperations::cancel_proxy(Entity &entity, uint64_t respondent_id,
                                      uint64_t originating_sequence) {
  if (!fits_width(originating_sequence, entity.config_.sequence_width))
    throw std::invalid_argument("proxy origin transaction ID does not fit");
  PutOptions options;
  options.metadata_options.push_back(
      TLV::message_to_user(reserved_message(MessageType::ProxyCancel, {})));
  options.metadata_options.push_back(TLV::message_to_user(
      originating_id(entity.config_.entity_id, originating_sequence,
                     entity.config_.id_width, entity.config_.sequence_width)));
  return entity.put(respondent_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

uint64_t UserOperations::directory_listing(Entity &entity, uint64_t remote_id,
                                           std::string directory_name,
                                           std::string output_name) {
  Bytes content;
  append_lv(content, directory_name);
  append_lv(content, output_name);
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(reserved_message(
      MessageType::DirectoryListingRequest, std::move(content))));
  return entity.put(remote_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

uint64_t UserOperations::remote_status(Entity &entity, uint64_t remote_id,
                                       uint64_t source_id, uint64_t sequence,
                                       std::string report_file_name) {
  if (!fits_width(source_id, entity.config_.id_width) ||
      !fits_width(sequence, entity.config_.sequence_width))
    throw std::invalid_argument("remote status transaction ID does not fit");
  Bytes content =
      transaction_id_content(source_id, sequence, entity.config_.id_width,
                             entity.config_.sequence_width);
  append_lv(content, report_file_name);
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(
      reserved_message(MessageType::RemoteStatusRequest, std::move(content))));
  return entity.put(remote_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

uint64_t UserOperations::remote_suspend(Entity &entity, uint64_t remote_id,
                                        uint64_t source_id, uint64_t sequence) {
  if (!fits_width(source_id, entity.config_.id_width) ||
      !fits_width(sequence, entity.config_.sequence_width))
    throw std::invalid_argument("remote suspend transaction ID does not fit");
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(reserved_message(
      MessageType::RemoteSuspendRequest,
      transaction_id_content(source_id, sequence, entity.config_.id_width,
                             entity.config_.sequence_width))));
  return entity.put(remote_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

uint64_t UserOperations::remote_resume(Entity &entity, uint64_t remote_id,
                                       uint64_t source_id, uint64_t sequence) {
  if (!fits_width(source_id, entity.config_.id_width) ||
      !fits_width(sequence, entity.config_.sequence_width))
    throw std::invalid_argument("remote resume transaction ID does not fit");
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(reserved_message(
      MessageType::RemoteResumeRequest,
      transaction_id_content(source_id, sequence, entity.config_.id_width,
                             entity.config_.sequence_width))));
  return entity.put(remote_id, "", {}, PDU_Transmission_Mode::Acknowledged,
                    std::move(options));
}

void UserOperations::report_proxy(Entity &entity, const ProxyContext &context,
                                  Condition_Code condition,
                                  const std::vector<TLV> &filestore_responses) {
  PutOptions options;
  const uint8_t delivery = condition == Condition_Code::NoError ? 0 : 1;
  const uint8_t file_status = condition == Condition_Code::NoError ? 2 : 0;
  const uint8_t result = static_cast<uint8_t>(
      (static_cast<uint8_t>(condition) << wire::condition_shift) |
      (delivery << wire::delivery_shift) | file_status);
  options.metadata_options.push_back(TLV::message_to_user(
      reserved_message(MessageType::ProxyPutResponse, {result})));
  options.metadata_options.push_back(TLV::message_to_user(
      originating_id(context.originator_id, context.originating_sequence,
                     entity.config_.id_width, entity.config_.sequence_width)));
  for (const auto &response : filestore_responses) {
    Bytes content{static_cast<uint8_t>(response.value.size())};
    content.insert(content.end(), response.value.begin(), response.value.end());
    options.metadata_options.push_back(TLV::message_to_user(reserved_message(
        MessageType::ProxyFilestoreResponse, std::move(content))));
  }
  entity.put(context.originator_id, "", {}, PDU_Transmission_Mode::Acknowledged,
             std::move(options));
}

void UserOperations::on_sender_finished(
    Entity &entity, uint64_t sequence, Condition_Code result,
    const std::vector<TLV> &filestore_responses) {
  const auto pending = pending_proxies_.find(sequence);
  if (pending == pending_proxies_.end())
    return;
  const auto context = pending->second;
  pending_proxies_.erase(pending);
  report_proxy(entity, context, result, filestore_responses);
}

void UserOperations::forget_sender(uint64_t sequence) {
  pending_proxies_.erase(sequence);
}

void UserOperations::handle_proxy_cancel(
    Entity &entity, const PDU_Header &header, const Bytes &value,
    const std::vector<ReservedMessage> &messages) {
  MessageReader(value).require_end();
  std::optional<uint64_t> originating_sequence;
  for (const auto &message : messages) {
    if (message.type !=
        static_cast<uint8_t>(MessageType::OriginatingTransactionId))
      continue;
    MessageReader reader(*message.value);
    const auto id = reader.transaction_id();
    reader.require_end();
    if (id.source_id != header.sourceEntityID)
      throw std::invalid_argument("proxy cancel originator mismatch");
    originating_sequence = id.sequence;
    break;
  }
  if (!originating_sequence)
    throw std::invalid_argument("missing proxy cancel transaction ID");
  for (const auto &[sequence, context] : pending_proxies_)
    if (context.originator_id == header.sourceEntityID &&
        context.originating_sequence == *originating_sequence &&
        !entity.sender_complete(sequence)) {
      entity.cancel_send(sequence);
      break;
    }
}

void UserOperations::handle_remote_request(Entity &entity,
                                           const PDU_Header &header,
                                           uint8_t type, const Bytes &value) {
  MessageReader reader(value);
  const auto id = reader.transaction_id();
  const auto originating = originating_id(
      header.sourceEntityID, header.transactionSequenceNumber,
      header.entityLength, header.transactionSequenceNumberLength);
  const bool suspension =
      type == static_cast<uint8_t>(MessageType::RemoteSuspendRequest) ||
      type == static_cast<uint8_t>(MessageType::RemoteResumeRequest);
  if (suspension) {
    reader.require_end();
    const bool suspend =
        type == static_cast<uint8_t>(MessageType::RemoteSuspendRequest);
    const auto order = entity.apply_remote_order(
        header.sourceEntityID, id.source_id, id.sequence, suspend);
    Bytes response{static_cast<uint8_t>(
        (order.suspended ? suspended_flag : 0) |
        (static_cast<uint8_t>(order.status) << suspension_status_shift))};
    response.insert(response.end(), id.encoded.begin(), id.encoded.end());
    PutOptions options;
    options.metadata_options.push_back(TLV::message_to_user(
        reserved_message(suspend ? MessageType::RemoteSuspendResponse
                                 : MessageType::RemoteResumeResponse,
                         std::move(response))));
    options.metadata_options.push_back(TLV::message_to_user(originating));
    entity.put(header.sourceEntityID, "", {},
               PDU_Transmission_Mode::Acknowledged, std::move(options));
    return;
  }

  const auto report_name = reader.name();
  reader.require_end();
  const auto report = entity.find_report(id.source_id, id.sequence);
  const auto status = !report              ? Transaction_Status::Unrecognized
                      : report->terminated ? Transaction_Status::Terminated
                                           : Transaction_Status::Active;
  const bool success = report.has_value() && !report_name.empty();
  Bytes response{static_cast<uint8_t>(
      (static_cast<uint8_t>(status) << report_status_shift) |
      (success ? 1 : 0))};
  response.insert(response.end(), id.encoded.begin(), id.encoded.end());
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(reserved_message(
      MessageType::RemoteStatusResponse, std::move(response))));
  options.metadata_options.push_back(TLV::message_to_user(originating));
  std::string report_text;
  if (success) {
    report_text =
        "source=" + std::to_string(report->source_id) +
        " sequence=" + std::to_string(report->sequence) +
        " progress=" + std::to_string(report->progress) +
        " terminated=" + std::to_string(report->terminated) +
        " suspended=" + std::to_string(report->suspended) + " condition=" +
        std::to_string(static_cast<unsigned>(report->condition)) + "\n";
  }
  entity.put(header.sourceEntityID, success ? report_name : "",
             Bytes(report_text.begin(), report_text.end()),
             PDU_Transmission_Mode::Acknowledged, std::move(options));
}

void UserOperations::handle_directory_listing(Entity &entity,
                                              const PDU_Header &header,
                                              const Bytes &value) {
  MessageReader reader(value);
  const auto directory_name = reader.name();
  const auto output_name = reader.name();
  reader.require_end();
  const auto [exists, listing] = entity.filestore_.listing(directory_name);
  Bytes response{static_cast<uint8_t>(exists ? 0 : listing_failed_flag)};
  append_lv(response, directory_name);
  append_lv(response, output_name);
  PutOptions options;
  options.metadata_options.push_back(TLV::message_to_user(reserved_message(
      MessageType::DirectoryListingResponse, std::move(response))));
  options.metadata_options.push_back(TLV::message_to_user(originating_id(
      header.sourceEntityID, header.transactionSequenceNumber,
      header.entityLength, header.transactionSequenceNumberLength)));
  entity.put(header.sourceEntityID, exists ? output_name : "",
             exists ? Bytes(listing.begin(), listing.end()) : Bytes{},
             PDU_Transmission_Mode::Acknowledged, std::move(options));
}

void UserOperations::handle_proxy_put(
    Entity &entity, const PDU_Header &header, const Bytes &value,
    const std::vector<ReservedMessage> &messages) {
  MessageReader reader(value);
  const auto id_width = reader.octet();
  if (!id_width || id_width > wire::max_id_width)
    throw std::invalid_argument("invalid proxy beneficiary ID width");
  const auto beneficiary = reader.integer(id_width);
  const auto source_name = reader.name();
  const auto destination_name = reader.name();
  reader.require_end();
  auto source = entity.read_file(source_name);
  if (!source) {
    report_proxy(entity,
                 {header.sourceEntityID, header.transactionSequenceNumber},
                 Condition_Code::FilestoreRejection, {});
    return;
  }

  PutOptions options;
  options.source_name = source_name;
  PDU_Transmission_Mode proxy_mode = PDU_Transmission_Mode::Acknowledged;
  bool preserve_records = false;
  for (const auto &message : messages) {
    const auto kind = static_cast<MessageType>(message.type);
    MessageReader option(*message.value);
    switch (kind) {
    case MessageType::ProxyFaultOverride:
      options.metadata_options.push_back(
          {TLV_Type::FaultHandlerOverrides, {option.octet()}});
      option.require_end();
      break;
    case MessageType::ProxyTransmissionMode:
    case MessageType::ProxyClosureRequest:
    case MessageType::ProxySegmentationControl: {
      const auto control = option.octet();
      option.require_end();
      if (control > 1)
        throw std::invalid_argument("invalid proxy control option");
      if (kind == MessageType::ProxyTransmissionMode)
        proxy_mode = static_cast<PDU_Transmission_Mode>(control);
      else if (kind == MessageType::ProxyClosureRequest)
        options.closure_requested = control != 0;
      else
        preserve_records = control == 0;
      break;
    }
    case MessageType::ProxyUserMessage:
    case MessageType::ProxyFilestoreRequest:
    case MessageType::ProxyFlowLabel: {
      auto payload = option.value();
      option.require_end();
      if (kind == MessageType::ProxyUserMessage)
        options.metadata_options.push_back(
            TLV::message_to_user(std::move(payload)));
      else if (kind == MessageType::ProxyFlowLabel)
        options.metadata_options.push_back(TLV::flow_label(std::move(payload)));
      else {
        TLV request{TLV_Type::FilestoreRequest, std::move(payload)};
        Filestore::validate_request(request);
        options.metadata_options.push_back(std::move(request));
      }
      break;
    }
    default:
      break;
    }
  }
  if (preserve_records) {
    const auto layout = entity.filestore_.records(source_name);
    if (!layout) {
      report_proxy(entity,
                   {header.sourceEntityID, header.transactionSequenceNumber},
                   Condition_Code::InvalidFileStructure, {});
      return;
    }
    options.record_lengths = *layout;
  }
  options.metadata_options.push_back(TLV::message_to_user(originating_id(
      header.sourceEntityID, header.transactionSequenceNumber,
      header.entityLength, header.transactionSequenceNumberLength)));
  const auto sequence =
      entity.put(beneficiary, destination_name, std::move(*source), proxy_mode,
                 std::move(options));
  pending_proxies_.emplace(
      sequence,
      ProxyContext{header.sourceEntityID, header.transactionSequenceNumber});
}

void UserOperations::process(Entity &entity, const PDU_Header &header,
                             const Metadata_PDU &metadata, size_t covered) {
  std::vector<ReservedMessage> messages;
  for (const auto &tlv : metadata.options)
    if (is_reserved_message(tlv))
      messages.push_back({tlv.value[4], &tlv.value});

  for (const auto &message : messages) {
    try {
      switch (static_cast<MessageType>(message.type)) {
      case MessageType::ProxyCancel:
        handle_proxy_cancel(entity, header, *message.value, messages);
        break;
      case MessageType::RemoteStatusRequest:
      case MessageType::RemoteSuspendRequest:
      case MessageType::RemoteResumeRequest:
        handle_remote_request(entity, header, message.type, *message.value);
        break;
      case MessageType::DirectoryListingRequest:
        handle_directory_listing(entity, header, *message.value);
        break;
      case MessageType::ProxyPut:
        handle_proxy_put(entity, header, *message.value, messages);
        break;
      default:
        break;
      }
    } catch (const std::exception &) {
      entity.event(Event_Type::Fault, header,
                   Condition_Code::FilestoreRejection, covered);
    }
  }
}

} // namespace cfdp
