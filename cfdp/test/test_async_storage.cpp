#include "entity.hpp"
#include "udp_receiver.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace cfdp;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

struct Gate {
  std::promise<std::thread::id> entered;
  std::promise<void> release;
  std::shared_future<void> allowed = release.get_future().share();
};

// Declared after Entity so failures release the worker before Entity joins it.
struct ReleaseGate {
  Gate &gate;
  bool released = false;
  void release() {
    if (!released) {
      gate.release.set_value();
      released = true;
    }
  }
  ~ReleaseGate() { release(); }
};

void send(Entity &source, Entity &destination) {
  for (const auto &wire : source.take_outgoing())
    destination.receive(wire);
}

template <class Predicate> void until(Predicate done) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!done()) {
    check(std::chrono::steady_clock::now() < deadline,
          "storage did not finish");
    std::this_thread::sleep_for(1ms);
  }
}

void concurrent_receives(const std::filesystem::path &directory) {
  Config config{2};
  config.asynchronous_storage = true;
  config.max_active_data_bytes = 8;
  config.filestore = {Filestore_Mode::Disk, directory};
  config.default_peer.keep_alive_interval_ticks = 1;
  Gate gate;
  auto entered = gate.entered.get_future();
  config.commit_received = [&](const Transfer &transfer, const Filestore &) {
    if (transfer.destination_name == "first") {
      gate.entered.set_value(std::this_thread::get_id());
      if (gate.allowed.wait_for(3s) != std::future_status::ready)
        throw std::runtime_error("test storage gate timed out");
    }
    if (transfer.destination_name == "failure")
      throw std::runtime_error("simulated storage failure");
  };
  {
    Entity receiver(config), first(Config{1}), second(Config{3});
    ReleaseGate release{gate};
    const auto first_id = first.put(2, "first", {1, 2});
    auto first_packets = first.take_outgoing();
    for (const auto &wire : first_packets)
      receiver.receive(wire);
    check(entered.wait_for(1s) == std::future_status::ready,
          "receive did not start storage worker");
    check(entered.get() != std::this_thread::get_id(),
          "storage callback ran on protocol thread");
    // The callback is blocked, but another complete transaction can be
    // received.
    PutOptions options;
    options.metadata_options = {
        filestore_request(Filestore_Action::CreateFile, "created")};
    const auto second_id =
        second.put(2, "second", {3, 4}, std::nullopt, options);
    send(second, receiver);
    Entity excess(Config{4});
    excess.put(2, "over-budget", {5});
    const auto excess_packets = excess.take_outgoing();
    bool limited = false;
    try {
      receiver.receive(excess_packets[1]);
    } catch (const std::length_error &) {
      limited = true;
    }
    check(limited, "payload on the storage worker escaped the active-data limit");
    receiver.receive(first_packets.back()); // Duplicate EOF while storing.
    receiver.receive(first_packets[1]);     // Duplicate data must not grow RAM.
    receiver.advance(6s); // Local storage is not missing data.
    check(receiver.take_completed().empty() &&
              !receiver.receiver_result(1, first_id) &&
              !receiver.receiver_result(3, second_id),
          "receive completed before storage");
    size_t acknowledgments = 0;
    for (const auto &wire : receiver.take_outgoing()) {
      const auto pdu = CFDP_PDU::from_bytes(wire);
      check(!std::holds_alternative<Finished_PDU>(pdu.body),
            "Finished was emitted before storage completed");
      if (std::holds_alternative<ACK_PDU>(pdu.body))
        ++acknowledgments;
      (pdu.header.sourceEntityID == 1 ? first : second).receive(wire);
    }
    check(acknowledgments >= 3, "protocol stopped acknowledging while storing");
    release.release();
    until([&] {
      for (const auto &wire : receiver.take_outgoing()) {
        const auto pdu = CFDP_PDU::from_bytes(wire);
        (pdu.header.sourceEntityID == 1 ? first : second).receive(wire);
      }
      send(first, receiver);
      send(second, receiver);
      return first.sender_complete(first_id) &&
             second.sender_complete(second_id);
    });
    check(first.sender_result(first_id) == Condition_Code::NoError &&
              second.sender_result(second_id) == Condition_Code::NoError &&
              receiver.read_file("first") == Bytes({1, 2}) &&
              receiver.read_file("second") == Bytes({3, 4}) &&
              receiver.read_file("created") == Bytes{},
          "serialized storage lost a concurrent filestore update");
    receiver.take_completed();
    const auto failure = first.put(2, "failure", {9});
    send(first, receiver);
    until([&] {
      send(receiver, first);
      send(first, receiver);
      return first.sender_complete(failure);
    });
    check(first.sender_result(failure) == Condition_Code::FilestoreRejection &&
              receiver.take_completed().empty() &&
              !receiver.read_file("failure"),
          "asynchronous storage failure was reported as success");
  }
  // Reopen without the callback to verify what was actually committed.
  config.commit_received = {};
  Entity restored(config);
  check(restored.read_file("first") == Bytes({1, 2}) &&
            restored.read_file("second") == Bytes({3, 4}) &&
            restored.read_file("created") == Bytes{} &&
            !restored.read_file("failure"),
        "asynchronous storage did not survive restart");
}

void cancellation_during_storage() {
  Gate gate;
  auto entered = gate.entered.get_future();
  Config config{2};
  config.asynchronous_storage = true;
  config.commit_received = [&](const Transfer &, const Filestore &) {
    gate.entered.set_value(std::this_thread::get_id());
    if (gate.allowed.wait_for(3s) != std::future_status::ready)
      throw std::runtime_error("test cancellation gate timed out");
  };
  Entity receiver(config), sender(Config{1});
  ReleaseGate release{gate};
  const auto sequence = sender.put(2, "canceled", {7});
  send(sender, receiver);
  check(entered.wait_for(1s) == std::future_status::ready,
        "cancellation test did not start storage");
  receiver.cancel_receive(1, sequence);
  send(receiver, sender);
  send(sender, receiver);
  release.release();
  until([&] {
    receiver.poll_storage();
    return receiver.receiver_complete(1, sequence);
  });
  check(sender.sender_result(sequence) == Condition_Code::CancelRequested &&
            receiver.take_completed().empty(),
        "late storage completion reversed cancellation");
}

void bounded_queue() {
  DatagramQueue queue(2, 3);
  check(queue.push({{1, 2}, {}}) && !queue.push({{3, 4}, {}}) &&
            queue.push({{3}, {}}) && !queue.push({{}, {}}),
        "receive queue did not enforce byte and packet limits");
  check(queue.dropped() == 2 && queue.pop(0ms)->bytes == Bytes({1, 2}) &&
            queue.pop(0ms)->bytes == Bytes({3}) && !queue.pop(0ms),
        "receive queue did not retain existing packets in order");
  check(queue.push({{4, 5, 6}, {}}),
        "queue capacity did not recover after draining");
}
} // namespace

int main() {
  std::string pattern = "/tmp/cfdp-async-test-XXXXXX";
  if (!mkdtemp(pattern.data()))
    return 1;
  const std::filesystem::path directory(pattern);
  try {
    bounded_queue();
    concurrent_receives(directory / "store");
    cancellation_during_storage();
    std::filesystem::remove_all(directory);
    std::cout << "CFDP asynchronous storage tests passed\n";
  } catch (const std::exception &error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
