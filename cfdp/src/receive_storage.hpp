#pragma once

#include "entity.hpp"

#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

namespace cfdp {

struct StoredTransfer {
  Filestore filestore;
  Transfer transfer;
  Finished_PDU finished;
  std::exception_ptr error;
  bool capacity_error = false;
};

StoredTransfer store_received(
    Filestore filestore, Transfer transfer, PDU_Header header,
    bool closure_requested, size_t outgoing_limit,
    const std::function<void(const Transfer &, const Filestore &)> &commit);

// One worker and one outstanding job. Waiting receives stay in Entity's
// bounded transaction table rather than accumulating another payload queue.
class ReceiveStorage {
public:
  ReceiveStorage();
  ~ReceiveStorage();
  ReceiveStorage(const ReceiveStorage &) = delete;
  ReceiveStorage &operator=(const ReceiveStorage &) = delete;
  void submit(std::packaged_task<StoredTransfer()> task);
  bool ready() const;
  StoredTransfer take();

private:
  void run();
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::optional<std::packaged_task<StoredTransfer()>> task_;
  std::future<StoredTransfer> result_;
  std::thread thread_;
};
} // namespace cfdp
