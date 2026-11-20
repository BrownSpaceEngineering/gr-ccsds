#include "entity.hpp"
#include "entity_detail.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace cfdp {

Entity::Queued_PDU Entity::encode_pdu(PDU_Header h, PDU_Body body) const {
  const uint64_t remote = h.direction == PDU_Direction::ToFileReceiver
                              ? h.destinationEntityID
                              : h.sourceEntityID;
  Queued_PDU queued{remote,
                    {h.sourceEntityID, h.transactionSequenceNumber},
                    {}};
  if (h.direction == PDU_Direction::ToFileReceiver &&
      h.sourceEntityID == config_.entity_id) {
    if (const auto *data = std::get_if<FileData_PDU>(&body)) {
      queued.issue = Queued_PDU::Issue::FileData;
      queued.progress = data->offset + data->data.size();
    } else if (const auto *eof = std::get_if<EOF_PDU>(&body)) {
      queued.issue = Queued_PDU::Issue::EndOfFile;
      queued.condition = eof->condition;
    }
  } else if (std::holds_alternative<Finished_PDU>(body)) {
    queued.issue = Queued_PDU::Issue::Finished;
  }
  queued.bytes = CFDP_PDU{h, std::move(body)}.to_bytes();
  return queued;
}

void Entity::reserve_outgoing(const PDU_Batch &batch, size_t released_bytes) {
  assert(released_bytes <= queued_bytes_);
  if (batch.bytes > config_.max_queued_pdu_bytes ||
      queued_bytes_ - released_bytes > config_.max_queued_pdu_bytes - batch.bytes)
    throw std::length_error("CFDP outgoing queue limit exceeded");
  outgoing_.reserve(outgoing_.size() + batch.pdus.size());
}

void Entity::append_outgoing(PDU_Batch batch) noexcept {
  assert(batch.pdus.size() <= outgoing_.capacity() - outgoing_.size());
  assert(batch.bytes <= config_.max_queued_pdu_bytes - queued_bytes_);
  for (auto &queued : batch.pdus)
    outgoing_.push_back(std::move(queued));
  queued_bytes_ += batch.bytes;
}

void Entity::emit(PDU_Header h, PDU_Body body) {
  auto queued = encode_pdu(h, std::move(body));
  PDU_Batch batch;
  batch.add(std::move(queued));
  reserve_outgoing(batch);
  append_outgoing(std::move(batch));
}

size_t Entity::queued_bytes(Key transaction) const {
  size_t total = 0;
  for (const auto &queued : outgoing_)
    if (queued.transaction == transaction)
      total += queued.bytes.size();
  return total;
}

void Entity::discard_outgoing(Key transaction) noexcept {
  std::erase_if(outgoing_, [&](const Queued_PDU &queued) {
    if (queued.transaction != transaction)
      return false;
    queued_bytes_ -= queued.bytes.size();
    return true;
  });
}

Entity::PDU_Batch Entity::prepare_sender_pdus(const Sender &sender) const {
  PDU_Batch batch;
  const size_t available = config_.max_queued_pdu_bytes - queued_bytes_;
  auto append = [&](PDU_Body body) {
    auto queued = encode_pdu(sender.header, std::move(body));
    if (queued.bytes.size() > available - batch.bytes)
      throw std::length_error("CFDP outgoing queue limit exceeded");
    batch.add(std::move(queued));
  };
  append(sender.metadata);
  for (const auto &segment : sender.segments)
    append(segment);
  append(sender.eof);
  return batch;
}

bool Entity::queue_pending_finished() {
  bool queued_any = false;
  for (auto &[key, receiver] : receivers_) {
    (void)key;
    if (!receiver.finished_pending)
      continue;
    auto queued = encode_pdu(reverse(receiver.header), receiver.finished);
    const size_t size = queued.bytes.size();
    if (size > config_.max_queued_pdu_bytes - queued_bytes_)
      continue;
    PDU_Batch batch;
    batch.add(std::move(queued));
    reserve_outgoing(batch);
    append_outgoing(std::move(batch));
    receiver.finished_pending = false;
    queued_any = true;
  }
  return queued_any;
}

size_t Entity::send_outgoing(
    const std::function<bool(uint64_t, const Bytes &)> &send) {
  poll_storage();
  if (!send)
    throw std::invalid_argument("CFDP transport callback is empty");
  std::set<uint64_t> blocked_peers;
  size_t issued = 0;
  auto drain = [&] {
    std::vector<Queued_PDU> held;
    held.reserve(outgoing_.size());
    for (auto &queued : outgoing_) {
      const auto opportunity = link_opportunities_.find(queued.peer);
      if (blocked_peers.contains(queued.peer) ||
          (opportunity != link_opportunities_.end() &&
           !opportunity->second.first)) {
        blocked_peers.insert(queued.peer);
        held.push_back(std::move(queued));
      } else if (!send(queued.peer, queued.bytes)) {
        blocked_peers.insert(queued.peer);
        held.push_back(std::move(queued));
      } else {
        mark_issued(queued);
        queued_bytes_ -= queued.bytes.size();
        ++issued;
      }
    }
    outgoing_.swap(held);
  };
  user_operations_.poll_completed(*this);
  queue_pending_finished();
  drain();
  user_operations_.poll_completed(*this);
  if (queue_pending_finished() || !outgoing_.empty())
    drain();
  return issued;
}

std::vector<Bytes> Entity::take_outgoing() {
  std::vector<Bytes> result;
  send_outgoing([&](uint64_t, const Bytes &bytes) {
    result.push_back(bytes);
    return true;
  });
  return result;
}

void Entity::mark_issued(const Queued_PDU &queued) {
  switch (queued.issue) {
  case Queued_PDU::Issue::FileData:
  case Queued_PDU::Issue::EndOfFile:
    if (auto sender = senders_.find(queued.transaction.sequence);
        sender != senders_.end())
      mark_sender_issued(sender->second, queued);
    break;
  case Queued_PDU::Issue::Finished:
    if (auto receiver = receivers_.find(queued.transaction);
        receiver != receivers_.end())
      receiver->second.finished_issued = true;
    break;
  case Queued_PDU::Issue::None:
    break;
  }
}

} // namespace cfdp
