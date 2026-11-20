#include "entity.hpp"
#include "entity_detail.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace cfdp {

void Entity::advance(std::chrono::milliseconds elapsed) {
  if (elapsed.count() < 0)
    throw std::invalid_argument("negative CFDP elapsed time");
  poll_storage();
  elapsed_remainder_ += elapsed;
  while (elapsed_remainder_ >= std::chrono::seconds(1)) {
    elapsed_remainder_ -= std::chrono::seconds(1);
    step();
  }
}

void Entity::tick() { advance(std::chrono::seconds(1)); }

void Entity::step() {
  for (auto &[sequence, sender] : senders_) {
    (void)sequence;
    step_sender(sender);
  }
  for (auto &[key, r] : receivers_) {
    (void)key;
    if (r.suspended)
      continue;
    if (auto opportunity = link_opportunities_.find(r.header.sourceEntityID);
        opportunity != link_opportunities_.end() &&
        (!opportunity->second.first ||
         (acknowledged(r.header) && !opportunity->second.second)))
      continue;
    const PeerConfig &settings = peer(r.header.sourceEntityID);
    if (r.storage_pending && !r.terminated) {
      if (acknowledged(r.header) && settings.keep_alive_interval_ticks &&
          ++r.keep_alive_ticks >= settings.keep_alive_interval_ticks) {
        emit(reverse(r.header), KeepAlive_PDU{r.covered});
        r.keep_alive_ticks = 0;
      }
      continue;
    }
    if (r.terminated) {
      if (!r.finished_acked && r.finished_issued && acknowledged(r.header) &&
          ++r.ack_ticks >= settings.ack_interval_ticks) {
        r.ack_ticks = 0;
        if (r.ack_retries++ >= settings.ack_retry_limit) {
          event(Event_Type::Fault, r.header, Condition_Code::AckLimitReached,
                r.covered);
          event(Event_Type::Abandoned, r.header,
                Condition_Code::AckLimitReached, r.covered);
          r.finished_acked = true;
        } else
          emit(reverse(r.header), r.finished);
      }
      continue;
    }
    if (acknowledged(r.header) && !r.eof &&
        settings.keep_alive_interval_ticks != 0 &&
        ++r.keep_alive_ticks >= settings.keep_alive_interval_ticks) {
      emit(reverse(r.header), KeepAlive_PDU{r.covered});
      r.keep_alive_ticks = 0;
    }
    if (!acknowledged(r.header) && r.eof &&
        ++r.check_ticks >= settings.check_interval_ticks) {
      r.check_ticks = 0;
      maybe_complete(r);
      if (!r.terminated && ++r.check_retries >= settings.check_limit)
        fail(r, Condition_Code::CheckLimitReached);
    }
    if (r.terminated || r.suspended)
      continue;
    if (++r.idle_ticks >= settings.inactivity_limit_ticks) {
      fail(r, Condition_Code::InactivityDetected);
    } else if (r.eof && acknowledged(r.header) &&
               ++r.nak_ticks >= settings.nak_interval_ticks) {
      r.nak_ticks = 0;
      if (r.nak_retries++ >= settings.nak_retry_limit)
        fail(r, Condition_Code::NakLimitReached);
      else
        request_missing(r);
    }
  }
}

void Entity::cancel_receive(uint64_t source_id, uint64_t sequence) {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &r = it->second;
  if (r.terminated)
    return;
  Finished_PDU finished{Condition_Code::CancelRequested,
                        Delivery_Code::Incomplete,
                        File_Status::DiscardedDeliberately,
                        {},
                        config_.entity_id};
  validate_finished_capacity(r, finished);
  r.finished = std::move(finished);
  r.terminated = true;
  r.finished_pending =
      acknowledged(r.header) || (r.metadata && r.metadata->closureRequested);
  r.finished_acked = !r.finished_pending;
  event(Event_Type::TransactionFinished, r.header,
        Condition_Code::CancelRequested, r.covered);
  events_.back().delivery = Delivery_Code::Incomplete;
  events_.back().file_status = File_Status::DiscardedDeliberately;
}

void Entity::suspend_receive(uint64_t source_id, uint64_t sequence) {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &r = it->second;
  if (r.terminated)
    return;
  r.local_suspended = true;
  if (r.suspended)
    return;
  r.suspended = true;
  event(Event_Type::Suspended, r.header, Condition_Code::SuspendRequested,
        r.covered);
}

void Entity::resume_receive(uint64_t source_id, uint64_t sequence) {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &r = it->second;
  r.local_suspended = false;
  if (!r.suspended || remote_suspensions_.contains(Key{source_id, sequence}))
    return;
  r.suspended = false;
  r.idle_ticks = 0;
  event(Event_Type::Resumed, r.header, Condition_Code::NoError, r.covered);
  request_missing(r);
  maybe_complete(r);
}

void Entity::request_nak(uint64_t source_id, uint64_t sequence) {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  request_missing(it->second, true, it->second.present.size());
}

void Entity::prompt_nak(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  if (acknowledged(it->second.header) && !it->second.done)
    emit(it->second.header, Prompt_PDU{Prompt_Type::NAK});
}

void Entity::prompt_keep_alive(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  if (acknowledged(it->second.header) && !it->second.done)
    emit(it->second.header, Prompt_PDU{Prompt_Type::KeepAlive});
}

Entity::RemoteOrderResult Entity::apply_remote_order(uint64_t requester,
                                                     uint64_t source_id,
                                                     uint64_t sequence,
                                                     bool suspend) {
  const Key key{source_id, sequence};
  const bool is_sender = source_id == config_.entity_id;
  auto sender = is_sender ? senders_.find(sequence) : senders_.end();
  auto receiver = is_sender ? receivers_.end() : receivers_.find(key);
  const bool active =
      (sender != senders_.end() && !sender->second.done) ||
      (receiver != receivers_.end() && !receiver->second.terminated);
  if (active && suspend) {
    auto &orders = remote_suspensions_[key];
    if (orders.insert(requester).second) {
      if (sender != senders_.end() && !sender->second.suspended) {
        suspend_sender(sender->second, Condition_Code::SuspendRequested);
      } else if (receiver != receivers_.end() && !receiver->second.suspended) {
        suspend_receive(source_id, sequence);
        receiver->second.local_suspended = false;
      }
    }
  } else if (active) {
    auto orders = remote_suspensions_.find(key);
    if (orders != remote_suspensions_.end()) {
      orders->second.erase(requester);
      if (orders->second.empty())
        remote_suspensions_.erase(orders);
    }
    if (sender != senders_.end() && sender->second.suspended &&
        !sender->second.local_suspended && !remote_suspensions_.contains(key))
      resume_send(sequence);
    if (receiver != receivers_.end() && receiver->second.suspended &&
        !receiver->second.local_suspended && !remote_suspensions_.contains(key))
      resume_receive(source_id, sequence);
  }
  const bool known = sender != senders_.end() || receiver != receivers_.end();
  const auto status = !known   ? Transaction_Status::Unrecognized
                      : active ? Transaction_Status::Active
                               : Transaction_Status::Terminated;
  const bool suspended =
      active && (sender != senders_.end() ? sender->second.suspended
                                          : receiver->second.suspended);
  return {status, suspended};
}

std::optional<Transaction_Report> Entity::find_report(uint64_t source_id,
                                                      uint64_t sequence) {
  if (source_id == config_.entity_id) {
    if (senders_.contains(sequence))
      return report_send(sequence);
  } else if (receivers_.contains(Key{source_id, sequence})) {
    return report_receive(source_id, sequence);
  }
  return std::nullopt;
}

Transaction_Report Entity::report_send(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &s = it->second;
  event(Event_Type::Report, s.header, s.result, s.file_size);
  return {config_.entity_id, sequence, s.file_size, s.done,
          s.suspended,       s.result};
}

Transaction_Report Entity::report_receive(uint64_t source_id,
                                          uint64_t sequence) {
  auto it = receivers_.find(Key{source_id, sequence});
  if (it == receivers_.end())
    throw std::invalid_argument("unknown CFDP transaction");
  auto &r = it->second;
  event(Event_Type::Report, r.header, r.finished.condition, r.covered);
  return {source_id,    sequence,    r.covered,
          r.terminated, r.suspended, r.finished.condition};
}

void Entity::forget_send(uint64_t sequence) {
  auto it = senders_.find(sequence);
  if (it == senders_.end() || !it->second.done || it->second.cancelling)
    throw std::invalid_argument("CFDP sender transaction is not complete");
  if (user_operations_.has_pending_proxy(sequence))
    throw std::logic_error("CFDP proxy response is still pending");
  if (queued_bytes(Key{config_.entity_id, sequence}))
    throw std::logic_error("CFDP transaction still has queued PDUs");
  remote_suspensions_.erase(Key{config_.entity_id, sequence});
  senders_.erase(it);
}

void Entity::forget_receive(uint64_t source_id, uint64_t sequence) {
  const Key key{source_id, sequence};
  auto it = receivers_.find(key);
  if (it == receivers_.end() || !receiver_complete(source_id, sequence))
    throw std::invalid_argument("CFDP receiver transaction is not complete");
  if (it->second.finished_pending)
    throw std::logic_error("CFDP transaction still has a pending Finished PDU");
  if (queued_bytes(key))
    throw std::logic_error("CFDP transaction still has queued PDUs");
  remote_suspensions_.erase(key);
  receivers_.erase(it);
}

} // namespace cfdp
