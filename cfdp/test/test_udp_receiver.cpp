#include "udp_receiver.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace cfdp;
using namespace std::chrono_literals;

int main() {
  int sender = -1;
  try {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
      throw std::runtime_error("cannot create test receive socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
        0) {
      ::close(fd);
      throw std::runtime_error("cannot bind test receive socket");
    }
    UdpReceiver receiver(fd, 2, 4);
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) < 0)
      throw std::runtime_error("cannot obtain test receive address");
    sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0)
      throw std::runtime_error("cannot create test send socket");
    auto transmit = [&](uint8_t byte) {
      const Bytes packet{byte, byte};
      if (::sendto(sender, packet.data(), packet.size(), 0,
                   reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) != 2)
        throw std::runtime_error("cannot send test packet");
    };
    // Do no consuming: the dedicated receiver must still drain the socket and
    // account for overflow, even while the protocol consumer is busy elsewhere.
    transmit(1);
    transmit(2);
    transmit(3);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!receiver.dropped_packets()) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(
            "receive thread did not drain socket independently");
      std::this_thread::sleep_for(1ms);
    }
    const auto first = receiver.receive(100ms);
    const auto second = receiver.receive(100ms);
    if (!first || !second || first->bytes != Bytes({1, 1}) ||
        second->bytes != Bytes({2, 2}) || receiver.dropped_packets() != 1)
      throw std::runtime_error(
          "bounded UDP queue lost order or overflow count");
    transmit(4);
    const auto next = receiver.receive(1s);
    if (!next || next->bytes != Bytes({4, 4}))
      throw std::runtime_error("UDP receiver did not recover after overflow");
    ::close(sender);
    sender = -1;
    std::cout << "Bounded UDP receiver tests passed\n";
  } catch (const std::exception &error) {
    if (sender >= 0)
      ::close(sender);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
