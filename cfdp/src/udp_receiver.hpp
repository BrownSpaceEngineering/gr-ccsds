#pragma once

#include "filestore.hpp"

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace cfdp {
struct Datagram {
  Bytes bytes;
  sockaddr_in peer{};
};

class DatagramQueue {
public:
  DatagramQueue(size_t max_packets, size_t max_bytes);
  bool push(Datagram packet);
  std::optional<Datagram> pop(std::chrono::milliseconds timeout);
  void close(std::exception_ptr error = {});
  size_t dropped() const;

private:
  const size_t max_packets_, max_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Datagram> packets_;
  size_t bytes_ = 0, dropped_ = 0;
  bool closed_ = false;
  std::exception_ptr error_;
};

// Owns the socket. Only this thread receives; the protocol thread can send on
// the same socket. Overflow drops new packets and increments dropped_packets().
class UdpReceiver {
public:
  static constexpr size_t default_max_packets = 1024;
  static constexpr size_t default_max_bytes = 8 * 1024 * 1024;
  explicit UdpReceiver(int fd, size_t max_packets = default_max_packets,
                       size_t max_bytes = default_max_bytes);
  ~UdpReceiver();
  UdpReceiver(const UdpReceiver &) = delete;
  UdpReceiver &operator=(const UdpReceiver &) = delete;
  int socket() const { return fd_; }
  std::optional<Datagram> receive(std::chrono::milliseconds timeout);
  size_t dropped_packets() const { return queue_.dropped(); }
  // Consumer-thread telemetry: drops since the previous call.
  size_t take_drop_count();

private:
  void run();
  int fd_;
  DatagramQueue queue_;
  std::atomic<bool> stopping_{false};
  size_t reported_drops_ = 0;
  std::thread thread_;
};
} // namespace cfdp
