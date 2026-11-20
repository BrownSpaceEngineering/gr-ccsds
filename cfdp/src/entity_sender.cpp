#include "entity.hpp"
#include "entity_detail.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cfdp {

void Entity::step_sender(Sender &s) {
  if ((s.done && !s.cancelling) || s.suspended || !s.eof_issued)
    return;
  if (auto opportunity =
          link_opportunities_.find(s.header.destinationEntityID);
      opportunity != link_opportunities_.end() &&
      (!opportunity->second.first ||
       (acknowledged(s.header) && !opportunity->second.second)))
    return;
  const PeerConfig &settings = peer(s.header.destinationEntityID);
  if (!acknowledged(s.header) && s.metadata.closureRequested &&
      ++s.check_ticks >= settings.check_interval_ticks) {
    s.check_ticks = 0;
    if (++s.check_retries >= settings.check_limit)
      fail(s, Condition_Code::CheckLimitReached);
  }
  if ((s.done && !s.cancelling) || s.suspended || !s.eof_issued)
    return;
  if (++s.idle_ticks >= settings.inactivity_limit_ticks) {
    fail(s, Condition_Code::InactivityDetected);
  } else if (acknowledged(s.header) && !s.eof_acked &&
             ++s.ack_ticks >= settings.ack_interval_ticks) {
    s.ack_ticks = 0;
    if (s.ack_retries++ >= settings.ack_retry_limit)
      fail(s, Condition_Code::AckLimitReached);
    else
      emit(s.header, s.eof);
  }
}

void Entity::receive_sender(const CFDP_PDU &pdu) {
  const auto &h = pdu.header;
  if (h.sourceEntityID != config_.entity_id)
    throw std::invalid_argument("invalid CFDP reverse direction");
  auto it = senders_.find(h.transactionSequenceNumber);
  if (it == senders_.end() ||
      it->second.header.destinationEntityID != h.destinationEntityID)
    throw std::invalid_argument("unknown CFDP sender transaction");
  auto &s = it->second;
  if (h.transmissionMode != s.header.transmissionMode ||
      h.largeFileFlag != s.header.largeFileFlag ||
      h.entityLength != s.header.entityLength ||
      h.transactionSequenceNumberLength !=
          s.header.transactionSequenceNumberLength)
    throw std::invalid_argument("inconsistent CFDP reverse header");
  if (auto ack = std::get_if<ACK_PDU>(&pdu.body)) {
    if (ack->directive == Acknowledged_Directive::EndOfFile) {
      if (!acknowledged(s.header) || !s.eof_issued ||
          ack->condition != s.eof.condition)
        throw std::invalid_argument("unexpected CFDP EOF ACK");
      s.idle_ticks = 0;
      s.eof_acked = true;
      s.ack_retries = 0;
      s.ack_ticks = 0;
      if (s.cancelling && s.done)
        s.cancelling = false;
    } else
      throw std::invalid_argument("unexpected CFDP sender ACK");
  } else if (auto nak = std::get_if<NAK_PDU>(&pdu.body)) {
    if (!acknowledged(s.header) || s.done)
      throw std::invalid_argument("unexpected CFDP NAK");
    for (const auto &request : nak->segmentRequests) {
      if (request.startOffset == 0 && request.endOffset == 0)
        continue;
      if (request.endOffset > s.file_size ||
          request.startOffset >= request.endOffset)
        throw std::invalid_argument("invalid CFDP NAK range");
    }
    if (s.suspended || s.cancelling)
      return;
    s.idle_ticks = 0;
    for (const auto &request : nak->segmentRequests) {
      if (request.startOffset == 0 && request.endOffset == 0) {
        emit(s.header, s.metadata);
        continue;
      }
      for (const auto &segment : s.segments)
        if (segment.offset < request.endOffset &&
            segment.offset + segment.data.size() > request.startOffset)
          emit(s.header, segment);
    }
  } else if (auto finished = std::get_if<Finished_PDU>(&pdu.body)) {
    if (!s.eof_issued ||
        (!acknowledged(s.header) && !s.metadata.closureRequested))
      throw std::invalid_argument("unexpected CFDP Finished");
    finish_sender(s, *finished);
  } else if (auto keepalive = std::get_if<KeepAlive_PDU>(&pdu.body)) {
    if (!acknowledged(s.header) || s.done ||
        keepalive->progress > s.file_size)
      throw std::invalid_argument("invalid CFDP keep alive progress");
    s.idle_ticks = 0;
    if (keepalive->progress > s.keep_alive_progress) {
      s.keep_alive_progress = keepalive->progress;
      s.keep_alive_stalls = 0;
    } else if (peer(s.header.destinationEntityID).keep_alive_limit &&
               ++s.keep_alive_stalls >=
                   peer(s.header.destinationEntityID).keep_alive_limit) {
      fail(s, Condition_Code::KeepAliveLimitReached);
    }
  } else
    throw std::invalid_argument("unexpected CFDP sender PDU");
}

void Entity::finish_sender(Sender &s, const Finished_PDU &finished) {
  const Key key{s.header.sourceEntityID, s.header.transactionSequenceNumber};
  PDU_Batch batch;
  if (acknowledged(s.header))
    batch.add(encode_pdu(
        s.header, ACK_PDU{Acknowledged_Directive::Finished, finished.condition,
                          Transaction_Status::Terminated}));
  reserve_outgoing(batch, queued_bytes(key));
  auto responses = finished.filestoreResponses;
  Event completed{};
  completed.type = Event_Type::TransactionFinished;
  completed.source_id = key.source;
  completed.sequence = key.sequence;
  completed.condition = finished.condition;
  completed.progress = s.file_size;
  completed.delivery = finished.delivery;
  completed.file_status = finished.fileStatus;
  completed.filestore_responses = responses;
  events_.reserve(events_.size() + 1);
  discard_outgoing(key);
  append_outgoing(std::move(batch));
  if (!s.done)
    events_.push_back(std::move(completed));
  s.result = finished.condition;
  s.filestore_responses = std::move(responses);
  s.idle_ticks = 0;
  s.done = true;
  s.cancelling = false;
  s.suspended = false;
  s.local_suspended = false;
}

void Entity::mark_sender_issued(Sender &s, const Queued_PDU &queued) {
  if (queued.issue == Queued_PDU::Issue::FileData) {
    s.sent_progress = std::max(s.sent_progress, queued.progress);
  } else if (!s.eof_issued) {
    s.eof_issued = true;
    event(Event_Type::EOFSent, s.header, queued.condition, s.sent_progress);
    if (!s.done && !acknowledged(s.header) && !s.metadata.closureRequested &&
        queued.condition == Condition_Code::NoError) {
      s.done = true;
      event(Event_Type::TransactionFinished, s.header,
            Condition_Code::NoError, s.sent_progress);
    }
  }
}

void Entity::suspend_sender(Sender &s, Condition_Code condition) {
  if (s.done || s.suspended)
    return;
  events_.reserve(events_.size() + 1);
  discard_outgoing(Key{s.header.sourceEntityID,
                       s.header.transactionSequenceNumber});
  s.suspended = true;
  event(Event_Type::Suspended, s.header, condition, s.file_size);
}

void Entity::cancel_sender(Sender &s, Condition_Code condition) {
  const Key key{s.header.sourceEntityID, s.header.transactionSequenceNumber};
  EOF_PDU eof{condition, 0, s.file_size, config_.entity_id};
  PDU_Batch batch;
  batch.add(encode_pdu(s.header, eof));
  reserve_outgoing(batch, queued_bytes(key));
  events_.reserve(events_.size() + 1);
  // Replace pending traffic only after the cancellation can be published.
  discard_outgoing(key);
  append_outgoing(std::move(batch));
  s.eof = std::move(eof);
  s.result = condition;
  s.cancelling = acknowledged(s.header);
  s.suspended = false;
  s.local_suspended = false;
  s.eof_issued = false;
  s.eof_acked = false;
  s.ack_retries = 0;
  s.ack_ticks = 0;
  // A local acknowledged cancellation waits for Finished; a fault already
  // determines the transaction result while its cancellation is acknowledged.
  s.done = condition != Condition_Code::CancelRequested ||
           !acknowledged(s.header);
  if (s.done)
    event(Event_Type::TransactionFinished, s.header, condition, s.file_size);
}

void Entity::abandon_sender(Sender &s, Condition_Code condition) {
  events_.reserve(events_.size() + 1);
  discard_outgoing(Key{s.header.sourceEntityID,
                       s.header.transactionSequenceNumber});
  if (!s.cancelling)
    s.result = condition;
  s.cancelling = false;
  s.suspended = false;
  s.local_suspended = false;
  s.done = true;
  event(Event_Type::Abandoned, s.header, condition, s.file_size);
}

void Entity::fail(Sender &s, Condition_Code condition) {
  if (s.done && !s.cancelling)
    return;
  if (s.cancelling) {
    abandon_sender(s, condition);
    return;
  }
  event(Event_Type::Fault, s.header, condition, s.file_size);
  switch (fault_handler(s.metadata.options, condition)) {
  case Fault_Handler::Ignore:
    s.idle_ticks = 0;
    s.ack_retries = 0;
    break;
  case Fault_Handler::Suspend:
    suspend_sender(s, condition);
    s.local_suspended = true;
    break;
  case Fault_Handler::Abandon:
    abandon_sender(s, condition);
    break;
  case Fault_Handler::Cancel:
    cancel_sender(s, condition);
    break;
  }
}

void Entity::cancel_send(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &s = it->second;
  if (s.done && acknowledged(s.header))
    return;
  cancel_sender(s, Condition_Code::CancelRequested);
}

void Entity::suspend_send(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &s = it->second;
  if (s.done)
    return;
  suspend_sender(s, Condition_Code::SuspendRequested);
  s.local_suspended = true;
}

void Entity::resume_send(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &s = it->second;
  if (!s.suspended ||
      remote_suspensions_.contains(Key{config_.entity_id, sequence})) {
    s.local_suspended = false;
    return;
  }
  auto batch = prepare_sender_pdus(s);
  reserve_outgoing(batch);
  events_.reserve(events_.size() + 1);
  // All serialization and allocation succeeds before publishing the resume.
  s.local_suspended = false;
  s.suspended = false;
  s.idle_ticks = 0;
  s.eof_issued = false;
  append_outgoing(std::move(batch));
  event(Event_Type::Resumed, s.header, Condition_Code::NoError, s.file_size);
}

} // namespace cfdp
