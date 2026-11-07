#include "receive_storage.hpp"
#include "entity_detail.hpp"

#include <stdexcept>

namespace cfdp {
StoredTransfer store_received(
    Filestore filestore, Transfer transfer, PDU_Header header,
    bool closure_requested, size_t outgoing_limit,
    const std::function<void(const Transfer &, const Filestore &)> &commit) {
  StoredTransfer result{filestore, std::move(transfer), {}, {}, false};
  try {
    if (!result.transfer.destination_name.empty())
      result.filestore.write(result.transfer.destination_name,
                             result.transfer.data,
                             result.transfer.record_lengths);
    result.transfer.filestore_responses =
        result.filestore.execute(result.transfer.metadata_options);
  } catch (...) {
    result.error = std::current_exception();
    result.filestore.swap(filestore);
    return result;
  }
  try {
    result.finished = {Condition_Code::NoError,
                       Delivery_Code::Complete,
                       File_Status::Retained,
                       result.transfer.filestore_responses,
                       {}};
    if ((acknowledged(header) || closure_requested) &&
        CFDP_PDU{reverse(header), result.finished}.to_bytes().size() >
            outgoing_limit)
      throw std::length_error("CFDP Finished exceeds outgoing queue limit");
  } catch (...) {
    result.error = std::current_exception();
    result.capacity_error = true;
    result.filestore.swap(filestore);
    return result;
  }
  try {
    if (commit)
      commit(result.transfer, result.filestore);
    result.filestore.commit();
  } catch (...) {
    result.error = std::current_exception();
    result.filestore.swap(filestore);
  }
  return result;
}

ReceiveStorage::ReceiveStorage() : thread_([this] { run(); }) {}

ReceiveStorage::~ReceiveStorage() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_one();
  thread_.join();
}

void ReceiveStorage::submit(std::packaged_task<StoredTransfer()> task) {
  std::lock_guard lock(mutex_);
  if (result_.valid() || task_)
    throw std::logic_error("receive storage worker is busy");
  result_ = task.get_future();
  task_.emplace(std::move(task));
  changed_.notify_one();
}

bool ReceiveStorage::ready() const {
  return result_.valid() &&
         result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

StoredTransfer ReceiveStorage::take() { return result_.get(); }

void ReceiveStorage::run() {
  while (true) {
    std::packaged_task<StoredTransfer()> task;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || task_.has_value(); });
      if (!task_)
        return;
      task = std::move(*task_);
      task_.reset();
    }
    task();
  }
}
} // namespace cfdp
