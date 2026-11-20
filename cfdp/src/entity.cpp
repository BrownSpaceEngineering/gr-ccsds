#include "entity.hpp"
#include "entity_detail.hpp"
#include "receive_storage.hpp"
#include "wire_constants.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cfdp {
namespace {
// Leave room for CFDP headers and metadata within the 16-bit PDU data field.
constexpr size_t max_configured_segment_size = 60000;

static_assert(std::is_nothrow_move_constructible_v<Transfer>);
static_assert(std::is_nothrow_move_constructible_v<Event>);
static_assert(std::is_nothrow_move_assignable_v<Finished_PDU>);

void validate_peer(const PeerConfig &peer) {
  if (peer.protocol_version != 1 ||
      static_cast<uint8_t>(peer.default_mode) > 1 ||
      !checksum_type_supported(peer.default_checksum_type) ||
      !peer.segment_size || peer.segment_size > max_configured_segment_size ||
      !peer.ack_interval_ticks || !peer.ack_retry_limit ||
      !peer.nak_interval_ticks || !peer.nak_retry_limit ||
      !peer.check_interval_ticks || !peer.check_limit ||
      !peer.inactivity_limit_ticks)
    throw std::invalid_argument("invalid CFDP peer configuration");
}
void validate_metadata_options(const Metadata_PDU &metadata) {
  for (const auto &option : metadata.options) {
    switch (option.type) {
    case TLV_Type::FilestoreRequest:
      Filestore::validate_request(option);
      break;
    case TLV_Type::FaultHandlerOverrides:
      if (option.value.size() != 1 ||
          (option.value[0] >> wire::condition_shift) == 0 ||
          (option.value[0] >> wire::condition_shift) >=
              wire::first_reserved_condition ||
          (option.value[0] & wire::low_nibble_mask) <
              static_cast<uint8_t>(Fault_Handler::Cancel) ||
          (option.value[0] & wire::low_nibble_mask) >
              static_cast<uint8_t>(Fault_Handler::Abandon))
        throw std::invalid_argument("invalid fault handler override");
      break;
    case TLV_Type::MessageToUser:
    case TLV_Type::FlowLabel:
      break;
    default:
      throw std::invalid_argument("invalid Metadata option");
    }
  }
}
} // namespace

Entity::Entity(Config config)
    : config_(config), next_sequence_(config.initial_sequence) {
  if (!config_.id_width || config_.id_width > wire::max_id_width ||
      !config_.sequence_width || config_.sequence_width > wire::max_id_width ||
      !config_.initial_sequence ||
      (config_.sequence_width < wire::max_id_width &&
       config_.initial_sequence >=
           (uint64_t{1} << (8 * config_.sequence_width))) ||
      !config_.max_file_size || !config_.max_active_transactions ||
      !config_.max_active_data_bytes || !config_.max_queued_pdu_bytes ||
      (config_.id_width < wire::max_id_width &&
       config_.entity_id >= (uint64_t{1} << (8 * config_.id_width))))
    throw std::invalid_argument("invalid CFDP entity configuration");
  validate_peer(config_.default_peer);
  for (const auto &[id, settings] : config_.peers) {
    if (config_.id_width < wire::max_id_width &&
        id >= (uint64_t{1} << (8 * config_.id_width)))
      throw std::invalid_argument("CFDP peer ID does not fit");
    validate_peer(settings);
  }
  filestore_ = Filestore(config_.filestore, config_.max_file_size);
  if (config_.asynchronous_storage)
    storage_ = std::make_unique<ReceiveStorage>();
}

const PeerConfig &Entity::peer(uint64_t id) const {
  const auto it = config_.peers.find(id);
  return it == config_.peers.end() ? config_.default_peer : it->second;
}

size_t Entity::active_data_bytes() const {
  size_t total = storage_data_bytes_;
  for (const auto &[sequence, sender] : senders_) {
    (void)sequence;
    if (!sender.done)
      total += sender.file_size;
  }
  for (const auto &[key, receiver] : receivers_) {
    (void)key;
    if (!receiver.terminated)
      total += receiver.data.size() + receiver.present.size();
  }
  return total;
}

void Entity::event(Event_Type type, const PDU_Header &h,
                   Condition_Code condition, uint64_t progress) {
  Event next{};
  next.type = type;
  next.source_id = h.sourceEntityID;
  next.sequence = h.transactionSequenceNumber;
  next.condition = condition;
  next.progress = progress;
  events_.push_back(std::move(next));
}

uint64_t Entity::put(uint64_t destination_id, std::string destination_name,
                     Bytes data, std::optional<PDU_Transmission_Mode> mode_override,
                     PutOptions options) {
  const auto &settings = peer(destination_id);
  const auto mode = mode_override.value_or(settings.default_mode);
  if (!options.closure_requested)
    options.closure_requested = settings.default_closure_requested;
  if (!options.checksum_type)
    options.checksum_type = settings.default_checksum_type;
  if (std::count_if(senders_.begin(), senders_.end(),
                    [](const auto &entry) { return !entry.second.done; }) +
          std::count_if(
              receivers_.begin(), receivers_.end(),
              [](const auto &entry) { return !entry.second.terminated; }) >=
      static_cast<std::ptrdiff_t>(config_.max_active_transactions))
    throw std::length_error("CFDP active transaction limit exceeded");
  if (data.size() > config_.max_file_size ||
      destination_name.size() > wire::max_lv_length)
    throw std::length_error("CFDP transfer exceeds configured limit");
  if (data.size() > config_.max_active_data_bytes ||
      active_data_bytes() > config_.max_active_data_bytes - data.size())
    throw std::length_error("CFDP active data limit exceeded");
  if (config_.id_width < wire::max_id_width &&
      destination_id >= (uint64_t{1} << (8 * config_.id_width)))
    throw std::invalid_argument("CFDP destination ID does not fit");
  const uint64_t max_sequence =
      config_.sequence_width == wire::max_id_width
          ? std::numeric_limits<uint64_t>::max()
          : (uint64_t{1} << (8 * config_.sequence_width)) - 1;
  if (options.record_delimiter && !options.record_lengths.empty())
    throw std::invalid_argument(
        "record delimiter and lengths are mutually exclusive");
  if (options.segment_metadata.size() > wire::max_segment_metadata_length)
    throw std::invalid_argument("CFDP segment metadata exceeds 63 octets");
  if (options.record_delimiter) {
    size_t begin = 0;
    for (size_t i = 0; i < data.size(); ++i) {
      if (data[i] == *options.record_delimiter) {
        options.record_lengths.push_back(i + 1 - begin);
        begin = i + 1;
      }
    }
    if (begin < data.size())
      options.record_lengths.push_back(data.size() - begin);
  }
  if (!options.record_lengths.empty()) {
    size_t record_total = 0;
    for (const size_t length : options.record_lengths) {
      if (!length || length > data.size() - record_total)
        throw std::invalid_argument("invalid CFDP record lengths");
      record_total += length;
    }
    if (record_total != data.size())
      throw std::invalid_argument("CFDP record lengths do not cover the file");
  }
  if (!checksum_type_supported(options.checksum_type.value_or(0)))
    throw std::invalid_argument("unsupported CFDP checksum type");
  uint64_t candidate = next_sequence_;
  if (!candidate || candidate > max_sequence)
    candidate = 1;
  const uint64_t first_candidate = candidate;
  while (senders_.contains(candidate)) {
    candidate = candidate == max_sequence ? 1 : candidate + 1;
    if (candidate == first_candidate)
      throw std::overflow_error("CFDP sequence space exhausted");
  }
  const uint64_t sequence = candidate;
  PDU_Header h;
  h.sourceEntityID = config_.entity_id;
  h.destinationEntityID = destination_id;
  h.transactionSequenceNumber = sequence;
  h.entityLength = config_.id_width;
  h.transactionSequenceNumberLength = config_.sequence_width;
  h.transmissionMode = mode;
  h.crcFlag = settings.crc ? PDU_CRC_Flag::Present : PDU_CRC_Flag::NotPresent;
  if (!options.record_lengths.empty()) {
    h.segmentationControl = PDU_Segmentation_Control::BoundariesPreserved;
  }
  if (!options.record_lengths.empty() || !options.segment_metadata.empty()) {
    h.segmentMetadataFlag = PDU_Segment_Metadata_Flag::Present;
  }
  if (options.unbounded || data.size() > std::numeric_limits<uint32_t>::max())
    h.largeFileFlag = PDU_Large_File_Flag::LargeFile;
  Metadata_PDU metadata;
  metadata.closureRequested = mode == PDU_Transmission_Mode::Unacknowledged &&
                              options.closure_requested.value_or(false);
  metadata.checksumType = options.checksum_type.value_or(0);
  metadata.fileSize = options.unbounded ? 0 : data.size();
  metadata.sourceFileName =
      options.source_name.empty() ? destination_name : options.source_name;
  metadata.destinationFileName = std::move(destination_name);
  metadata.options = std::move(options.metadata_options);
  validate_metadata_options(metadata);
  EOF_PDU eof{Condition_Code::NoError,
              file_checksum(data, metadata.checksumType),
              data.size(),
              {}};
  Sender sender;
  sender.header = h;
  sender.metadata = metadata;
  sender.file_size = data.size();
  sender.eof = eof;
  auto append_segments = [&](size_t begin, size_t end, bool record) {
    for (size_t offset = begin; offset < end; offset += settings.segment_size) {
      const size_t stop = std::min(end, offset + settings.segment_size);
      Record_Continuation_State state = Record_Continuation_State::NoBoundary;
      if (record) {
        const bool first = offset == begin;
        const bool last = stop == end;
        state = first && last ? Record_Continuation_State::Complete
                : first       ? Record_Continuation_State::Start
                : last        ? Record_Continuation_State::End
                              : Record_Continuation_State::NoBoundary;
      }
      sender.segments.push_back(
          {offset, Bytes(data.begin() + offset, data.begin() + stop), state,
           options.segment_metadata});
    }
  };
  if (options.record_lengths.empty())
    append_segments(0, data.size(), false);
  else {
    size_t begin = 0;
    for (const size_t length : options.record_lengths) {
      append_segments(begin, begin + length, true);
      begin += length;
    }
  }
  auto initial = prepare_sender_pdus(sender);
  reserve_outgoing(initial);
  senders_.emplace(sequence, std::move(sender));
  next_sequence_ = sequence == max_sequence ? 1 : sequence + 1;
  append_outgoing(std::move(initial));
  event(Event_Type::TransactionStarted, h);
  events_.back().source_name = metadata.sourceFileName;
  events_.back().destination_name = metadata.destinationFileName;
  events_.back().file_size = data.size();
  events_.back().metadata_options = metadata.options;
  return sequence;
}

uint64_t Entity::proxy_put(uint64_t respondent_id, uint64_t beneficiary_id,
                           std::string source_name,
                           std::string destination_name, ProxyOptions options) {
  return user_operations_.proxy_put(
      *this, respondent_id, beneficiary_id, std::move(source_name),
      std::move(destination_name), std::move(options));
}
uint64_t Entity::cancel_proxy(uint64_t respondent_id,
                              uint64_t originating_sequence) {
  return user_operations_.cancel_proxy(*this, respondent_id,
                                       originating_sequence);
}
uint64_t Entity::directory_listing(uint64_t remote_id,
                                   std::string directory_name,
                                   std::string output_name) {
  return user_operations_.directory_listing(
      *this, remote_id, std::move(directory_name), std::move(output_name));
}
uint64_t Entity::remote_status(uint64_t remote_id, uint64_t source_id,
                               uint64_t sequence,
                               std::string report_file_name) {
  return user_operations_.remote_status(*this, remote_id, source_id, sequence,
                                        std::move(report_file_name));
}
uint64_t Entity::remote_suspend(uint64_t remote_id, uint64_t source_id,
                                uint64_t sequence) {
  return user_operations_.remote_suspend(*this, remote_id, source_id, sequence);
}
uint64_t Entity::remote_resume(uint64_t remote_id, uint64_t source_id,
                               uint64_t sequence) {
  return user_operations_.remote_resume(*this, remote_id, source_id, sequence);
}

void Entity::request_missing(Receiver &r, bool force, uint64_t scope_end) {
  if (!acknowledged(r.header) || r.terminated || r.suspended || r.storage_pending ||
      (!r.eof && !force))
    return;
  NAK_PDU nak;
  nak.startOfScope = 0;
  nak.endOfScope = r.eof ? r.eof->fileSize : scope_end;
  const size_t fss = r.header.largeFileFlag == PDU_Large_File_Flag::LargeFile
                         ? wire::large_file_size_width
                         : wire::small_file_size_width;
  const size_t crc =
      r.header.crcFlag == PDU_CRC_Flag::Present ? wire::crc_width : 0;
  const size_t max_requests = (wire::max_data_field_length -
                               wire::directive_code_width - 2 * fss - crc) /
                              (2 * fss);
  auto append_request = [&](Segment_Request request) {
    if (nak.segmentRequests.size() == max_requests) {
      emit(reverse(r.header), nak);
      nak.segmentRequests.clear();
    }
    nak.segmentRequests.push_back(request);
  };
  if (!r.metadata)
    append_request({0, 0});
  const size_t size = static_cast<size_t>(nak.endOfScope);
  for (size_t i = 0; i < size;) {
    if (i < r.present.size() && r.present[i]) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < size && (i >= r.present.size() || !r.present[i]))
      ++i;
    append_request({start, i});
  }
  if (!nak.segmentRequests.empty() || force)
    emit(reverse(r.header), nak);
}

Fault_Handler Entity::fault_handler(const std::vector<TLV> &options,
                                   Condition_Code condition) const {
  Fault_Handler handler = Fault_Handler::Cancel;
  if (auto configured = config_.default_fault_handlers.find(condition);
      configured != config_.default_fault_handlers.end())
    handler = configured->second;
  for (const auto &option : options)
    if (option.type == TLV_Type::FaultHandlerOverrides &&
        option.value.size() == 1 &&
        (option.value[0] >> wire::condition_shift) ==
            static_cast<uint8_t>(condition)) {
      const auto code = option.value[0] & wire::low_nibble_mask;
      if (code >= static_cast<uint8_t>(Fault_Handler::Cancel) &&
          code <= static_cast<uint8_t>(Fault_Handler::Abandon))
        handler = static_cast<Fault_Handler>(code);
    }
  return handler;
}

void Entity::fail(Receiver &r, Condition_Code condition) {
  if (r.terminated)
    return;
  event(Event_Type::Fault, r.header, condition, r.covered);
  const auto handler = fault_handler(
      r.metadata ? r.metadata->options : std::vector<TLV>{},
      condition);
  if (handler == Fault_Handler::Ignore) {
    r.idle_ticks = 0;
    return;
  }
  if (handler == Fault_Handler::Suspend && acknowledged(r.header)) {
    r.suspended = true;
    r.local_suspended = true;
    event(Event_Type::Suspended, r.header, condition, r.covered);
    return;
  }
  if (handler == Fault_Handler::Abandon) {
    r.terminated = true;
    r.finished_acked = true;
    event(Event_Type::Abandoned, r.header, condition, r.covered);
    return;
  }
  const auto delivery = condition == Condition_Code::FilestoreRejection
                            ? Delivery_Code::Complete
                            : Delivery_Code::Incomplete;
  const auto file_status = condition == Condition_Code::FilestoreRejection
                               ? File_Status::DiscardedByFilestore
                               : File_Status::DiscardedDeliberately;
  Finished_PDU finished{condition,
                        delivery,
                        file_status,
                        {},
                        condition == Condition_Code::UnsupportedChecksumType
                            ? std::nullopt
                            : std::optional<uint64_t>(config_.entity_id)};
  validate_finished_capacity(r, finished);
  r.finished = std::move(finished);
  r.terminated = true;
  r.finished_pending =
      acknowledged(r.header) || (r.metadata && r.metadata->closureRequested);
  r.finished_acked = !r.finished_pending;
  event(Event_Type::TransactionFinished, r.header, condition, r.covered);
  events_.back().delivery = delivery;
  events_.back().file_status = file_status;
}

void Entity::validate_finished_capacity(const Receiver &r,
                                        const Finished_PDU &finished) const {
  if (!acknowledged(r.header) && !(r.metadata && r.metadata->closureRequested))
    return;
  const auto wire = CFDP_PDU{reverse(r.header), finished}.to_bytes();
  if (wire.size() > config_.max_queued_pdu_bytes)
    throw std::length_error("CFDP Finished exceeds outgoing queue limit");
}

void Entity::maybe_complete(Receiver &r) {
  if (r.terminated || r.suspended || r.storage_pending || !r.eof || !r.metadata)
    return;
  const auto size = r.eof->fileSize;
  const bool unbounded =
      r.header.largeFileFlag == PDU_Large_File_Flag::LargeFile &&
      r.metadata->fileSize == 0;
  if ((!unbounded && r.metadata->fileSize != size) || r.present.size() > size) {
    fail(r, Condition_Code::FileSizeError);
    return;
  }
  if (r.present.size() < size || r.covered < size)
    return;
  if (!checksum_type_supported(r.metadata->checksumType)) {
    fail(r, Condition_Code::UnsupportedChecksumType);
    return;
  }
  if (file_checksum(r.data, r.metadata->checksumType) != r.eof->fileChecksum) {
    fail(r, Condition_Code::FileChecksumFailure);
    return;
  }
  std::vector<size_t> record_lengths;
  if (r.records_preserved && size && r.record_ends.contains(size)) {
    size_t begin = 0;
    for (const size_t end : r.record_ends) {
      if (end > size)
        break;
      record_lengths.push_back(end - begin);
      begin = end;
    }
  }
  r.storage_records = std::move(record_lengths);
  r.storage_pending = true;
  if (!storage_key_)
    start_receive_storage(r);
}

void Entity::receive(const Bytes &bytes) {
  CFDP_PDU pdu;
  try {
    pdu = CFDP_PDU::from_bytes(bytes);
  } catch (const PDU_CRC_Error &) {
    return;
  }
  auto &h = pdu.header;
  const uint64_t remote = h.direction == PDU_Direction::ToFileSender
                              ? h.destinationEntityID
                              : h.sourceEntityID;
  if (auto opportunity = link_opportunities_.find(remote);
      opportunity != link_opportunities_.end() && !opportunity->second.second)
    return;
  if (const auto *metadata = std::get_if<Metadata_PDU>(&pdu.body)) {
    if (h.transmissionMode == PDU_Transmission_Mode::Acknowledged &&
        metadata->closureRequested)
      throw std::invalid_argument("closure flag in acknowledged Metadata");
    validate_metadata_options(*metadata);
  }
  if (h.direction == PDU_Direction::ToFileSender) {
    receive_sender(pdu);
    return;
  }
  if (h.destinationEntityID != config_.entity_id)
    return;
  Key key{h.sourceEntityID, h.transactionSequenceNumber};
  if (!receivers_.contains(key) &&
      !std::holds_alternative<Metadata_PDU>(pdu.body) &&
      !std::holds_alternative<FileData_PDU>(pdu.body) &&
      !std::holds_alternative<EOF_PDU>(pdu.body))
    throw std::invalid_argument("unexpected first CFDP receiver PDU");
  if (auto metadata = std::get_if<Metadata_PDU>(&pdu.body);
      metadata && metadata->fileSize > config_.max_file_size)
    throw std::length_error("CFDP file exceeds configured limit");
  if (auto eof = std::get_if<EOF_PDU>(&pdu.body);
      eof && eof->fileSize > config_.max_file_size)
    throw std::length_error("CFDP EOF exceeds configured limit");
  if (auto data = std::get_if<FileData_PDU>(&pdu.body)) {
    if (data->offset > config_.max_file_size ||
        data->data.size() > config_.max_file_size - data->offset)
      throw std::length_error("CFDP file data exceeds file size");
    const auto old = receivers_.find(key);
    const size_t prior = old == receivers_.end() ? 0 : old->second.data.size();
    const size_t end = static_cast<size_t>(data->offset) + data->data.size();
    if ((old == receivers_.end() ||
         (!old->second.terminated && !old->second.storage_pending)) && end > prior) {
      const size_t growth = 2 * (end - prior);
      if (growth > config_.max_active_data_bytes ||
          active_data_bytes() > config_.max_active_data_bytes - growth)
        throw std::length_error("CFDP active data limit exceeded");
    }
  }
  if (!receivers_.contains(key) &&
      std::count_if(senders_.begin(), senders_.end(),
                    [](const auto &entry) { return !entry.second.done; }) +
              std::count_if(
                  receivers_.begin(), receivers_.end(),
                  [](const auto &entry) { return !entry.second.terminated; }) >=
          static_cast<std::ptrdiff_t>(config_.max_active_transactions))
    throw std::length_error("CFDP active transaction limit exceeded");
  auto [it, inserted] = receivers_.try_emplace(key);
  auto &r = it->second;
  if (inserted)
    r.header = h;
  else if (r.header.destinationEntityID != h.destinationEntityID ||
           r.header.transmissionMode != h.transmissionMode ||
           r.header.largeFileFlag != h.largeFileFlag ||
           r.header.entityLength != h.entityLength ||
           r.header.transactionSequenceNumberLength !=
               h.transactionSequenceNumberLength)
    throw std::invalid_argument("inconsistent CFDP transaction header");
  r.idle_ticks = 0;
  if (r.storage_pending && !r.terminated) {
    if (auto eof = std::get_if<EOF_PDU>(&pdu.body)) {
      if (acknowledged(h))
        emit(reverse(h), ACK_PDU{Acknowledged_Directive::EndOfFile,
                                 eof->condition, Transaction_Status::Active});
      if (eof->condition != Condition_Code::NoError)
        fail(r, eof->condition);
    } else if (auto prompt = std::get_if<Prompt_PDU>(&pdu.body);
               prompt && prompt->response == Prompt_Type::KeepAlive && acknowledged(h))
      emit(reverse(h), KeepAlive_PDU{r.covered});
    return;
  }
  if (r.terminated) {
    if (auto eof = std::get_if<EOF_PDU>(&pdu.body); eof && acknowledged(h))
      emit(reverse(h), ACK_PDU{Acknowledged_Directive::EndOfFile,
                               eof->condition, Transaction_Status::Terminated});
    else if (auto ack = std::get_if<ACK_PDU>(&pdu.body);
             ack && ack->directive == Acknowledged_Directive::Finished &&
             acknowledged(h) && r.finished_issued &&
             ack->condition == r.finished.condition)
      r.finished_acked = true;
    return;
  }
  if (auto metadata = std::get_if<Metadata_PDU>(&pdu.body)) {
    if (metadata->fileSize > config_.max_file_size)
      throw std::length_error("CFDP file exceeds configured limit");
    if (!r.metadata) {
      event(Event_Type::MetadataReceived, h);
      events_.back().file_size = metadata->fileSize;
      events_.back().source_name = metadata->sourceFileName;
      events_.back().destination_name = metadata->destinationFileName;
      events_.back().metadata_options = metadata->options;
      r.metadata = *metadata;
      r.nak_retries = 0;
      if (!checksum_type_supported(metadata->checksumType)) {
        fail(r, Condition_Code::UnsupportedChecksumType);
        return;
      }
    }
    if (r.eof)
      request_missing(r);
  } else if (auto data = std::get_if<FileData_PDU>(&pdu.body)) {
    if (data->offset > config_.max_file_size ||
        data->data.size() > config_.max_file_size - data->offset)
      throw std::length_error("CFDP file data exceeds file size");
    if (r.eof && data->offset + data->data.size() > r.eof->fileSize) {
      fail(r, Condition_Code::FileSizeError);
      return;
    }
    const size_t end = data->offset + data->data.size();
    if (h.segmentationControl == PDU_Segmentation_Control::BoundariesPreserved)
      r.records_preserved = true;
    if (!data->data.empty() &&
        h.segmentationControl ==
            PDU_Segmentation_Control::BoundariesPreserved &&
        (data->recordContinuationState == Record_Continuation_State::End ||
         data->recordContinuationState == Record_Continuation_State::Complete))
      r.record_ends.insert(end);
    if (r.data.size() < end) {
      r.data.resize(end);
      r.present.resize(end);
    }
    const size_t prior_coverage = r.covered;
    for (size_t i = 0; i < data->data.size(); ++i) {
      const size_t at = data->offset + i;
      if (!r.present[at]) {
        r.present[at] = 1;
        r.data[at] = data->data[i];
        ++r.covered;
      }
    }
    if (r.covered > prior_coverage) {
      r.nak_retries = 0;
      event(Event_Type::FileSegmentReceived, h, Condition_Code::NoError,
            r.covered);
      events_.back().segment_offset = data->offset;
      events_.back().segment_length = data->data.size();
      events_.back().record_state = data->recordContinuationState;
      events_.back().segment_metadata = data->segmentMetadata;
    }
    if (!r.eof && peer(h.sourceEntityID).nak_mode == Nak_Mode::Immediate) {
      const bool gap = std::find(r.present.begin(), r.present.begin() + end,
                                 0) != r.present.begin() + end;
      if (!r.metadata || gap)
        request_missing(r, true, end);
    }
  } else if (auto eof = std::get_if<EOF_PDU>(&pdu.body)) {
    if (eof->fileSize > config_.max_file_size)
      throw std::length_error("CFDP EOF exceeds configured limit");
    if (!r.eof) {
      event(Event_Type::EOFReceived, h, eof->condition, r.covered);
      r.eof = *eof;
    }
    if (acknowledged(h))
      emit(reverse(h), ACK_PDU{Acknowledged_Directive::EndOfFile,
                               eof->condition, Transaction_Status::Active});
    if (r.eof->condition != Condition_Code::NoError)
      fail(r, r.eof->condition);
    else
      request_missing(r);
  } else if (auto ack = std::get_if<ACK_PDU>(&pdu.body)) {
    if (ack->directive != Acknowledged_Directive::Finished ||
        !acknowledged(h) || !r.finished_issued ||
        ack->condition != r.finished.condition)
      throw std::invalid_argument("unexpected CFDP receiver ACK");
    r.finished_acked = true;
  } else if (auto prompt = std::get_if<Prompt_PDU>(&pdu.body)) {
    if (prompt->response == Prompt_Type::KeepAlive && !r.eof)
      emit(reverse(h), KeepAlive_PDU{r.covered});
    else if (prompt->response == Prompt_Type::NAK)
      request_missing(r, true, r.present.size());
  } else
    throw std::invalid_argument("unexpected CFDP receiver PDU");
  maybe_complete(r);
}

void Entity::configure_peer(uint64_t peer_id, PeerConfig settings) {
  if (config_.id_width < wire::max_id_width &&
      peer_id >= (uint64_t{1} << (8 * config_.id_width)))
    throw std::invalid_argument("CFDP peer ID does not fit");
  validate_peer(settings);
  for (const auto &[sequence, sender] : senders_) {
    (void)sequence;
    if (!sender.done && sender.header.destinationEntityID == peer_id)
      throw std::logic_error("cannot reconfigure active CFDP peer");
  }
  for (const auto &[key, receiver] : receivers_) {
    (void)key;
    if (!receiver.terminated && receiver.header.sourceEntityID == peer_id)
      throw std::logic_error("cannot reconfigure active CFDP peer");
  }
  config_.peers[peer_id] = std::move(settings);
}

PeerConfig Entity::peer_settings(uint64_t peer_id) const {
  return peer(peer_id);
}

Config Entity::local_settings() const { return config_; }

void Entity::set_link_opportunity(uint64_t peer_id, bool transmit,
                                  bool receive) {
  link_opportunities_[peer_id] = {transmit, receive};
}

std::vector<Transfer> Entity::take_completed() {
  poll_storage();
  std::vector<Transfer> result;
  result.swap(completed_);
  return result;
}
std::vector<Event> Entity::take_events() {
  std::vector<Event> result;
  result.swap(events_);
  const auto &policy = config_.indications;
  std::erase_if(result, [&](const Event &event) {
    const bool receiver = event.source_id != config_.entity_id;
    switch (event.type) {
    case Event_Type::EOFSent:
      return !policy.eof_sent;
    case Event_Type::EOFReceived:
      return !policy.eof_received;
    case Event_Type::FileSegmentReceived:
      return !policy.file_segment_received;
    case Event_Type::TransactionFinished:
      return receiver && !policy.receiver_finished;
    case Event_Type::Suspended:
      return receiver && !policy.receiver_suspended;
    case Event_Type::Resumed:
      return receiver && !policy.receiver_resumed;
    default:
      return false;
    }
  });
  return result;
}
bool Entity::sender_complete(uint64_t sequence) const {
  auto it = senders_.find(sequence);
  return it != senders_.end() && it->second.done;
}
std::optional<Condition_Code> Entity::sender_result(uint64_t sequence) const {
  auto it = senders_.find(sequence);
  if (it == senders_.end() || !it->second.done)
    return std::nullopt;
  return it->second.result;
}
uint64_t Entity::next_sequence() const { return next_sequence_; }
std::vector<TLV> Entity::sender_filestore_responses(uint64_t sequence) const {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  return it->second.filestore_responses;
}
bool Entity::receiver_complete(uint64_t source_id, uint64_t sequence) const {
  auto it = receivers_.find(Key{source_id, sequence});
  return it != receivers_.end() && it->second.terminated && !it->second.storage_pending &&
         (!acknowledged(it->second.header) || it->second.finished_acked);
}
std::optional<Condition_Code> Entity::receiver_result(uint64_t source_id,
                                                      uint64_t sequence) const {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end() || !it->second.terminated)
    return std::nullopt;
  return it->second.finished.condition;
}
void Entity::write_file(std::string name, Bytes data,
                        std::vector<size_t> record_lengths) {
  if (storage_key_)
    throw std::logic_error("receive storage is in progress");
  if (data.size() > config_.max_file_size)
    throw std::invalid_argument("invalid filestore file");
  Filestore prepared(filestore_);
  prepared.write(std::move(name), std::move(data), std::move(record_lengths));
  prepared.commit();
  filestore_.swap(prepared);
}
std::optional<Bytes> Entity::read_file(const std::string &name) const {
  if (storage_key_)
    throw std::logic_error("receive storage is in progress");
  return filestore_.read(name);
}
void Entity::save_filestore(const std::filesystem::path &path) const {
  if (storage_key_)
    throw std::logic_error("receive storage is in progress");
  filestore_.save(path);
}
void Entity::load_filestore(const std::filesystem::path &path) {
  if (!senders_.empty() || !receivers_.empty())
    throw std::logic_error("cannot load filestore during transactions");
  filestore_.load(path, config_.max_file_size);
}
} // namespace cfdp
