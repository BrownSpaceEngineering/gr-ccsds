#include "udp_receiver.hpp"
#include "wire_constants.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace cfdp {
DatagramQueue::DatagramQueue(size_t max_packets, size_t max_bytes)
    : max_packets_(max_packets), max_bytes_(max_bytes) {
  if (!max_packets || !max_bytes)
    throw std::invalid_argument("UDP receive queue limits must be positive");
}

bool DatagramQueue::push(Datagram packet) {
  std::lock_guard lock(mutex_);
  if (closed_)
    return false;
  if (packets_.size() >= max_packets_ ||
      packet.bytes.size() > max_bytes_ - bytes_) {
    ++dropped_;
    return false;
  }
  bytes_ += packet.bytes.size();
  packets_.push_back(std::move(packet));
  changed_.notify_one();
  return true;
}

std::optional<Datagram> DatagramQueue::pop(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout,
                    [&] { return closed_ || !packets_.empty(); });
  if (packets_.empty()) {
    if (error_)
      std::rethrow_exception(error_);
    return std::nullopt;
  }
  Datagram packet = std::move(packets_.front());
  packets_.pop_front();
  bytes_ -= packet.bytes.size();
  return packet;
}

void DatagramQueue::close(std::exception_ptr error) {
  std::lock_guard lock(mutex_);
  error_ = error;
  closed_ = true;
  changed_.notify_all();
}

size_t DatagramQueue::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

UdpReceiver::UdpReceiver(int fd, size_t max_packets, size_t max_bytes) try
    : fd_(fd), queue_(max_packets, max_bytes) {
  if (fd < 0)
    throw std::invalid_argument("invalid UDP receive socket");
  thread_ = std::thread([this] { run(); });
} catch (...) {
  ::close(fd);
  throw;
}

UdpReceiver::~UdpReceiver() {
  stopping_ = true;
  queue_.close();
  thread_.join();
  ::close(fd_);
}

std::optional<Datagram>
UdpReceiver::receive(std::chrono::milliseconds timeout) {
  return queue_.pop(timeout);
}

size_t UdpReceiver::take_drop_count() {
  const auto total = dropped_packets();
  const auto delta = total - reported_drops_;
  reported_drops_ = total;
  return delta;
}

void UdpReceiver::run() {
  try {
    Bytes buffer(wire::max_pdu_size);
    while (!stopping_) {
      pollfd item{fd_, POLLIN, 0};
      const int ready = ::poll(&item, 1, 20);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL)))
        throw std::runtime_error("UDP receive poll failed");
      if (!ready)
        continue;
      Datagram packet;
      socklen_t length = sizeof(packet.peer);
      const auto size =
          ::recvfrom(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT,
                     reinterpret_cast<sockaddr *>(&packet.peer), &length);
      if (size < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (size < 0)
        throw std::runtime_error("UDP receive failed");
      // Keep queued allocations proportional to the datagram size, rather than
      // retaining a maximum-sized buffer for every small control packet.
      packet.bytes.assign(buffer.begin(), buffer.begin() + size);
      queue_.push(std::move(packet));
    }
  } catch (...) {
    queue_.close(std::current_exception());
  }
}
} // namespace cfdp
