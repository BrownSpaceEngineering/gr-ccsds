#include "entity.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace cfdp;

void check(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}

void run(PDU_Transmission_Mode mode, bool loss) {
  Entity sender(
      Config{.entity_id = 1,
             .default_peer = PeerConfig{.segment_size = 3, .ut_address = {}}});
  Entity receiver(
      Config{.entity_id = 2,
             .default_peer = PeerConfig{.segment_size = 3, .ut_address = {}}});
  Bytes source{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  const auto sequence = sender.put(2, "result.bin", source, mode);
  auto initial = sender.take_outgoing();
  check(initial.size() == 5, "unexpected initial PDU count");
  for (size_t i = 0; i < initial.size(); ++i) {
    if (loss && (i == 0 || i == 2))
      continue; // Missing metadata and middle segment.
    auto decoded = CFDP_PDU::from_bytes(initial[i]);
    check(decoded.to_bytes() == initial[i], "PDU failed round trip");
    receiver.receive(initial[i]);
  }
  for (int turn = 0; turn < 5; ++turn) {
    auto to_sender = receiver.take_outgoing();
    for (auto &pdu : to_sender)
      sender.receive(pdu);
    auto to_receiver = sender.take_outgoing();
    for (auto &pdu : to_receiver)
      receiver.receive(pdu);
    if (mode == PDU_Transmission_Mode::Acknowledged &&
        !sender.sender_complete(sequence)) {
      sender.tick();
      receiver.tick();
    }
  }
  auto completed = receiver.take_completed();
  check(completed.size() == 1, "file not delivered exactly once");
  check(completed[0].data == source, "delivered file differs");
  check(completed[0].destination_name == "result.bin",
        "destination name differs");
  check(sender.sender_complete(sequence), "sender did not finish");
}

int main() {
  try {
    run(PDU_Transmission_Mode::Unacknowledged, false);
    run(PDU_Transmission_Mode::Acknowledged, false);
    run(PDU_Transmission_Mode::Acknowledged, true);
    {
      Entity sender(Config{
          .entity_id = 1,
          .max_file_size = 1024,
          .default_peer =
              PeerConfig{.segment_size = 3, .crc = true, .ut_address = {}}});
      auto sequence =
          sender.put(2, "empty", {}, PDU_Transmission_Mode::Acknowledged);
      auto packets = sender.take_outgoing();
      check(packets.size() == 2, "empty file should have Metadata and EOF");
      auto corrupted = packets.front();
      corrupted.back() ^= 1;
      bool rejected = false;
      try {
        CFDP_PDU::from_bytes(corrupted);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      check(rejected, "bad CRC was accepted");
      Entity receiver(Config{
          .entity_id = 2,
          .max_file_size = 1024,
          .default_peer =
              PeerConfig{.segment_size = 3, .crc = true, .ut_address = {}}});
      receiver.receive(corrupted);
      check(receiver.take_outgoing().empty(), "bad CRC caused a response");
      for (auto &packet : packets)
        receiver.receive(packet);
      check(receiver.take_completed().size() == 1,
            "empty file was not delivered");
      check(!sender.sender_complete(sequence),
            "acknowledged sender finished prematurely");
    }
    {
      Entity sender(Config{1}), receiver(Config{2});
      sender.put(2, "original.bin", Bytes{1, 2, 3});
      auto packets = sender.take_outgoing();
      receiver.receive(packets[0]);
      auto changed = CFDP_PDU::from_bytes(packets[0]);
      std::get<Metadata_PDU>(changed.body).destinationFileName = "changed.bin";
      receiver.receive(changed.to_bytes());
      for (size_t i = 1; i < packets.size(); ++i)
        receiver.receive(packets[i]);
      check(receiver.read_file("original.bin") == Bytes({1, 2, 3}),
            "duplicate Metadata replaced original");
      check(!receiver.read_file("changed.bin"),
            "duplicate Metadata changed destination");
    }
    check(modular_checksum({1, 2, 3, 4, 5}) == 0x06020304,
          "modular checksum mismatch");
    std::cout << "CFDP entity tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
