#include "entity.hpp"
#include "entity_detail.hpp"
#include "receive_storage.hpp"

#include <stdexcept>

namespace cfdp {
Entity::~Entity() = default;

void Entity::start_receive_storage(Receiver &r) {
  Transfer transfer{r.header.sourceEntityID,
                    r.header.transactionSequenceNumber,
                    r.metadata->destinationFileName,
                    std::move(r.data),
                    r.metadata->sourceFileName,
                    r.metadata->options,
                    {},
                    std::move(r.storage_records)};
  if (!storage_) {
    auto result = store_received(
        filestore_, std::move(transfer), r.header, r.metadata->closureRequested,
        config_.max_queued_pdu_bytes, config_.commit_received);
    apply_receive_storage(r, std::move(result));
    return;
  }
  const auto bytes = transfer.data.size();
  std::packaged_task<StoredTransfer()> task(
      [filestore = std::move(filestore_), transfer = std::move(transfer),
       header = r.header, closure = r.metadata->closureRequested,
       limit = config_.max_queued_pdu_bytes,
       commit = config_.commit_received]() mutable {
        return store_received(std::move(filestore), std::move(transfer), header,
                              closure, limit, commit);
      });
  storage_->submit(std::move(task));
  storage_key_ =
      Key{r.header.sourceEntityID, r.header.transactionSequenceNumber};
  storage_data_bytes_ = bytes;
}

void Entity::apply_receive_storage(Receiver &r, StoredTransfer result) {
  r.storage_pending = false;
  filestore_.swap(result.filestore);
  if (result.error) {
    r.data = std::move(result.transfer.data);
    if (!r.terminated) {
      if (result.capacity_error && !storage_)
        std::rethrow_exception(result.error);
      fail(r, Condition_Code::FilestoreRejection);
    }
    return;
  }
  // An in-flight write cannot be rolled back by cancellation. Publish the
  // committed filestore, but never report that canceled transfer as successful.
  if (r.terminated)
    return;
  Event finished{};
  finished.type = Event_Type::TransactionFinished;
  finished.source_id = r.header.sourceEntityID;
  finished.sequence = r.header.transactionSequenceNumber;
  finished.progress = r.covered;
  finished.delivery = Delivery_Code::Complete;
  finished.file_status = File_Status::Retained;
  finished.filestore_responses = result.transfer.filestore_responses;
  completed_.reserve(completed_.size() + 1);
  events_.reserve(events_.size() + 1);
  completed_.push_back(std::move(result.transfer));
  events_.push_back(std::move(finished));
  Bytes{}.swap(r.present);
  r.delivered = true;
  r.terminated = true;
  if (acknowledged(r.header) || r.metadata->closureRequested) {
    r.finished = std::move(result.finished);
    r.finished_pending = true;
  }
  user_operations_.process(*this, r.header, *r.metadata, r.covered);
}

void Entity::poll_storage() {
  if (!storage_)
    return;
  if (storage_key_) {
    auto &receiver = receivers_.at(*storage_key_);
    if (!storage_->ready() || (receiver.suspended && !receiver.terminated))
      return;
    auto result = storage_->take();
    storage_key_.reset();
    storage_data_bytes_ = 0;
    apply_receive_storage(receiver, std::move(result));
  }
  for (auto &[key, receiver] : receivers_) {
    (void)key;
    if (!receiver.storage_pending)
      continue;
    if (receiver.terminated) {
      receiver.storage_pending = false;
      continue;
    }
    if (!receiver.suspended) {
      start_receive_storage(receiver);
      break;
    }
  }
}
} // namespace cfdp
