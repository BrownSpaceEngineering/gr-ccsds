#include "entity.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cfdp;

namespace {
enum class Kind { Metadata, Data, Eof, Finished, AckEof, AckFinished, Nak };

Kind kind(const Bytes &wire) {
  const auto pdu = CFDP_PDU::from_bytes(wire);
  if (std::holds_alternative<Metadata_PDU>(pdu.body))
    return Kind::Metadata;
  if (std::holds_alternative<FileData_PDU>(pdu.body))
    return Kind::Data;
  if (std::holds_alternative<EOF_PDU>(pdu.body))
    return Kind::Eof;
  if (std::holds_alternative<Finished_PDU>(pdu.body))
    return Kind::Finished;
  if (auto ack = std::get_if<ACK_PDU>(&pdu.body))
    return ack->directive == Acknowledged_Directive::EndOfFile
               ? Kind::AckEof
               : Kind::AckFinished;
  if (std::holds_alternative<NAK_PDU>(pdu.body))
    return Kind::Nak;
  throw std::runtime_error("unexpected PDU in notebook test");
}

struct Drop {
  Kind kind;
  int remaining;
};

void transfer(std::string name, PDU_Transmission_Mode mode,
              std::vector<Drop> drops = {}, bool duplicate = false,
              bool reorder = false, size_t file_size = 50000,
              Nak_Mode nak_mode = Nak_Mode::Deferred, bool reverse = false) {
  const uint64_t source_id = reverse ? 2 : 1;
  const uint64_t destination_id = reverse ? 1 : 2;
  Entity a(Config{.entity_id = source_id,
                  .default_peer =
                      PeerConfig{.segment_size = 256, .ut_address = {}}});
  Config receiver_config{.entity_id = destination_id,
                         .default_peer =
                             PeerConfig{.segment_size = 256, .ut_address = {}}};
  receiver_config.default_peer.nak_mode = nak_mode;
  Entity b(receiver_config);
  Bytes content(file_size);
  for (size_t i = 0; i < content.size(); ++i)
    content[i] = static_cast<uint8_t>((i * 17 + i / 31) & 255);
  const uint64_t sequence = a.put(destination_id, name, content, mode);
  auto first = a.take_outgoing();
  if (reorder)
    std::swap(first[1], first[2]);
  if (duplicate) {
    first.insert(first.end() - 1, first[1]);
    first.insert(first.end() - 1, first[2]);
  }
  auto forward = [&](std::vector<Bytes> packets, Entity &target) {
    for (const auto &packet : packets) {
      const auto type = kind(packet);
      auto it = std::find_if(drops.begin(), drops.end(), [=](const Drop &d) {
        return d.kind == type && d.remaining > 0;
      });
      if (it != drops.end()) {
        --it->remaining;
        continue;
      }
      target.receive(packet);
    }
  };
  forward(std::move(first), b);
  for (int i = 0; i < 60; ++i) {
    forward(b.take_outgoing(), a);
    forward(a.take_outgoing(), b);
    if (a.sender_complete(sequence) && b.receiver_complete(source_id, sequence))
      break;
    a.tick();
    b.tick();
  }
  auto completed = b.take_completed();
  if (completed.size() != 1 || completed[0].data != content ||
      !a.sender_complete(sequence) || !b.receiver_complete(source_id, sequence))
    throw std::runtime_error(name + " failed");
  std::cout << name << (reverse ? " reverse" : " forward") << " passed\n";
}

void transfer_both(std::string name, PDU_Transmission_Mode mode,
                   std::vector<Drop> drops = {}, bool duplicate = false,
                   bool reorder = false, size_t file_size = 50000,
                   Nak_Mode nak_mode = Nak_Mode::Deferred) {
  transfer(name, mode, drops, duplicate, reorder, file_size, nak_mode);
  transfer(name, mode, drops, duplicate, reorder, file_size, nak_mode, true);
}

void triggered_nak_with_loss(Nak_Mode mode, bool reverse) {
  const uint64_t source_id = reverse ? 2 : 1;
  const uint64_t destination_id = reverse ? 1 : 2;
  const std::string name = mode == Nak_Mode::Prompted ? "F4-S3" : "F4-S4";
  Entity source(Config{.entity_id = source_id,
                       .default_peer =
                           PeerConfig{.segment_size = 256, .ut_address = {}}});
  Config receiver_config{.entity_id = destination_id,
                         .default_peer =
                             PeerConfig{.segment_size = 256, .ut_address = {}}};
  receiver_config.default_peer.nak_mode = mode;
  Entity receiver(receiver_config);
  Bytes content(50000);
  for (size_t i = 0; i < content.size(); ++i)
    content[i] = static_cast<uint8_t>((i * 13) & 255);
  const auto sequence = source.put(destination_id, name, content);
  const auto initial = source.take_outgoing();
  size_t data_count = 0, dropped = 0, nak_count = 0;
  receiver.receive(initial.front());
  for (size_t index = 1; index + 1 < initial.size(); ++index) {
    const auto &wire = initial[index];
    if (kind(wire) != Kind::Data)
      throw std::runtime_error(name + " unexpected PDU");
    ++data_count;
    if (data_count % 20 == 1)
      ++dropped;
    else
      receiver.receive(wire);
    if (data_count != 65 && data_count != 130)
      continue;
    if (mode == Nak_Mode::Prompted) {
      source.prompt_nak(sequence);
      for (const auto &prompt : source.take_outgoing())
        receiver.receive(prompt);
    } else
      receiver.request_nak(source_id, sequence);
    for (const auto &nak : receiver.take_outgoing()) {
      if (kind(nak) != Kind::Nak)
        throw std::runtime_error(name + " expected NAK");
      ++nak_count;
      source.receive(nak);
    }
    for (const auto &retry : source.take_outgoing())
      receiver.receive(retry);
  }
  receiver.receive(initial.back());
  for (int step = 0; step < 30; ++step) {
    for (const auto &wire : receiver.take_outgoing())
      source.receive(wire);
    for (const auto &wire : source.take_outgoing())
      receiver.receive(wire);
    if (source.sender_complete(sequence) &&
        receiver.receiver_complete(source_id, sequence))
      break;
    source.tick();
    receiver.tick();
  }
  const auto completed = receiver.take_completed();
  if (dropped < 9 || nak_count < 2 || completed.size() != 1 ||
      completed[0].data != content ||
      source.sender_result(sequence) != Condition_Code::NoError ||
      receiver.receiver_result(source_id, sequence) != Condition_Code::NoError)
    throw std::runtime_error(name + " 50 KiB loss recovery failed");
  std::cout << name << (reverse ? " reverse" : " forward") << " passed\n";
}
} // namespace

int main() {
  try {
    // Adapted from CCSDS 720.4-Y-1 F1/F2. Both directions are exercised by
    // the same Entity implementation in test_entity.cpp.
    transfer_both("F1-S1 short unacknowledged",
                  PDU_Transmission_Mode::Unacknowledged, {}, false, false, 20);
    transfer_both("F1-S2 multi-PDU unacknowledged",
                  PDU_Transmission_Mode::Unacknowledged);
    transfer_both("F1-S3 acknowledged", PDU_Transmission_Mode::Acknowledged);
    transfer_both("F1-S4 dropped data", PDU_Transmission_Mode::Acknowledged,
                  {{Kind::Data, 2}});
    transfer_both("F1-S5 duplicate data", PDU_Transmission_Mode::Acknowledged,
                  {}, true);
    transfer_both("F1-S6 out of order data",
                  PDU_Transmission_Mode::Acknowledged, {}, false, true);
    {
      Entity a(Config{1}), b(Config{2});
      PutOptions options;
      options.metadata_options = {TLV::message_to_user({'o', 'n', 'e'}),
                                  TLV::message_to_user({'t', 'w', 'o'})};
      a.put(2, "", {}, PDU_Transmission_Mode::Acknowledged, options);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      auto completed = b.take_completed();
      if (completed.size() != 1 || completed[0].metadata_options.size() != 2 ||
          completed[0].metadata_options[0].value != Bytes({'o', 'n', 'e'}) ||
          completed[0].metadata_options[1].value != Bytes({'t', 'w', 'o'}))
        throw std::runtime_error("F1 user messages failed");
      std::cout << "F1-S7 user messages passed\n";
    }
    transfer_both("F2-S1 lost Metadata", PDU_Transmission_Mode::Acknowledged,
                  {{Kind::Metadata, 1}});
    transfer_both("F2-S2 lost EOF", PDU_Transmission_Mode::Acknowledged,
                  {{Kind::Eof, 1}});
    transfer_both("F2-S3 lost Finished", PDU_Transmission_Mode::Acknowledged,
                  {{Kind::Finished, 1}});
    transfer_both("F2-S4 lost ACK EOF", PDU_Transmission_Mode::Acknowledged,
                  {{Kind::AckEof, 1}});
    transfer_both("F2-S5 lost ACK Finished",
                  PDU_Transmission_Mode::Acknowledged,
                  {{Kind::AckFinished, 1}});
    transfer_both("F4-S1 deferred NAK with 5% data loss",
                  PDU_Transmission_Mode::Acknowledged, {{Kind::Data, 10}});
    transfer_both("F4-S2 immediate NAK with 5% data loss",
                  PDU_Transmission_Mode::Acknowledged, {{Kind::Data, 10}},
                  false, false, 50000, Nak_Mode::Immediate);
    for (bool reverse : {false, true}) {
      triggered_nak_with_loss(Nak_Mode::Prompted, reverse);
      triggered_nak_with_loss(Nak_Mode::Asynchronous, reverse);
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      Bytes source(50000, 0x5a);
      auto sequence = a.put(2, "noisy.bin", source);
      std::set<Bytes> seen_forward, seen_reverse;
      for (int i = 0; i < 100; ++i) {
        for (auto &wire : a.take_outgoing())
          if (kind(wire) == Kind::Eof || !seen_forward.insert(wire).second)
            b.receive(wire);
        for (auto &wire : b.take_outgoing())
          if (!seen_reverse.insert(wire).second)
            a.receive(wire);
        if (a.sender_complete(sequence) && b.receiver_complete(1, sequence))
          break;
        a.tick();
        b.tick();
      }
      auto completed = b.take_completed();
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence) ||
          completed.size() != 1 || completed[0].data != source)
        throw std::runtime_error("F2-S6 every PDU dropped once failed");
      std::cout << "F2-S6 every PDU except EOF dropped once passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      auto sequence = a.put(2, "cancel.bin", Bytes(50000, 42));
      auto initial = a.take_outgoing();
      for (size_t i = 0; i < 100; ++i)
        b.receive(initial[i]);
      a.cancel_send(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence) ||
          a.sender_result(sequence) != Condition_Code::CancelRequested ||
          !b.take_completed().empty())
        throw std::runtime_error("F1 sender cancel failed");
      std::cout << "F1-S8 sender cancel passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      auto sequence = a.put(2, "cancel.bin", Bytes(50000, 42));
      auto packets = a.take_outgoing();
      for (size_t i = 0; i < 100; ++i)
        b.receive(packets[i]);
      b.cancel_receive(1, sequence);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence) ||
          a.sender_result(sequence) != Condition_Code::CancelRequested ||
          !b.take_completed().empty())
        throw std::runtime_error("F1 receiver cancel failed");
      std::cout << "F1-S9 receiver cancel passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      auto sequence = a.put(2, "cancel-unack.bin", Bytes(50000, 42),
                            PDU_Transmission_Mode::Unacknowledged);
      auto packets = a.take_outgoing();
      for (size_t i = 0; i < 100; ++i)
        b.receive(packets[i]);
      a.cancel_send(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (a.sender_result(sequence) != Condition_Code::CancelRequested ||
          b.receiver_result(1, sequence) != Condition_Code::CancelRequested ||
          !b.take_completed().empty())
        throw std::runtime_error("F1-S10 unacknowledged cancel failed");
      std::cout << "F1-S10 unacknowledged cancel passed\n";
    }
    {
      Entity a(Config{1}), b(Config{2});
      auto sequence = a.put(2, "bad.bin", Bytes(100, 7));
      auto packets = a.take_outgoing();
      for (auto &wire : packets) {
        auto pdu = CFDP_PDU::from_bytes(wire);
        if (auto data = std::get_if<FileData_PDU>(&pdu.body)) {
          data->data[0] ^= 1;
          wire = pdu.to_bytes();
        }
        b.receive(wire);
      }
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (a.sender_result(sequence) != Condition_Code::FileChecksumFailure ||
          b.receiver_result(1, sequence) !=
              Condition_Code::FileChecksumFailure ||
          !b.take_completed().empty())
        throw std::runtime_error("F2 checksum fault failed");
      std::cout << "F2 checksum fault passed\n";
    }
    {
      Entity a(Config{1}), b(Config{2});
      auto sequence = a.put(2, "unsupported.bin", Bytes(100, 7));
      auto packets = a.take_outgoing();
      auto metadata = CFDP_PDU::from_bytes(packets[0]);
      std::get<Metadata_PDU>(metadata.body).checksumType = 4;
      b.receive(metadata.to_bytes());
      auto replies = b.take_outgoing();
      if (replies.size() != 1 ||
          std::get<Finished_PDU>(CFDP_PDU::from_bytes(replies[0]).body)
              .faultLocation)
        throw std::runtime_error(
            "unsupported checksum fault location was encoded");
      for (auto &wire : replies)
        a.receive(wire);
      if (a.sender_result(sequence) != Condition_Code::UnsupportedChecksumType)
        throw std::runtime_error("unsupported checksum fault failed");
      std::cout << "unsupported checksum fault passed\n";
    }
    {
      Config source_config{1};
      source_config.default_peer.ack_retry_limit = 2;
      source_config.default_peer.inactivity_limit_ticks = 100;
      Entity a(source_config), b(Config{2});
      auto sequence = a.put(2, "ack-limit.bin", Bytes(100, 4));
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      b.take_outgoing(); // Suppress every ACK and Finished response.
      for (int i = 0; i < 4; ++i) {
        a.tick();
        a.take_outgoing();
      }
      if (a.sender_result(sequence) != Condition_Code::AckLimitReached)
        throw std::runtime_error("F2 ACK limit fault failed");
      std::cout << "F2-S7 ACK limit fault passed\n";
    }
    {
      Config destination_config{2};
      destination_config.default_peer.nak_retry_limit = 2;
      destination_config.default_peer.inactivity_limit_ticks = 100;
      Entity a(Config{1}), b(destination_config);
      auto sequence = a.put(2, "nak-limit.bin", Bytes(3000, 4));
      for (auto &wire : a.take_outgoing())
        if (kind(wire) != Kind::Data)
          b.receive(wire);
      for (int i = 0; i < 4; ++i) {
        for (auto &wire : b.take_outgoing())
          if (kind(wire) != Kind::Nak)
            a.receive(wire);
        b.tick();
      }
      for (auto &wire : b.take_outgoing())
        if (kind(wire) != Kind::Nak)
          a.receive(wire);
      if (b.receiver_result(1, sequence) != Condition_Code::NakLimitReached ||
          a.sender_result(sequence) != Condition_Code::NakLimitReached)
        throw std::runtime_error("F2 NAK limit fault failed");
      std::cout << "F2-S8 NAK limit fault passed\n";
    }
    {
      Config source_config{
          .entity_id = 1,
          .default_peer = PeerConfig{.segment_size = 256, .ut_address = {}}};
      source_config.default_peer.inactivity_limit_ticks = 3;
      Entity a(source_config),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      auto sequence = a.put(2, "sender-inactive.bin", Bytes(50000, 4));
      for (const auto &wire : a.take_outgoing())
        b.receive(wire);
      for (const auto &wire : b.take_outgoing())
        if (kind(wire) != Kind::Finished)
          a.receive(wire);
      for (int i = 0; i < 4; ++i) {
        a.tick();
        a.take_outgoing();
      }
      if (a.sender_result(sequence) != Condition_Code::InactivityDetected)
        throw std::runtime_error("F2-S9 sender inactivity fault failed");
      std::cout << "F2-S9 sender inactivity passed\n";
    }
    {
      Config destination_config{
          .entity_id = 2,
          .default_peer = PeerConfig{.segment_size = 256, .ut_address = {}}};
      destination_config.default_peer.inactivity_limit_ticks = 2;
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(destination_config);
      auto sequence = a.put(2, "receiver-inactive.bin", Bytes(50000, 4));
      auto packets = a.take_outgoing();
      for (size_t i = 0; i < 100; ++i)
        b.receive(packets[i]);
      b.tick();
      b.tick();
      if (b.receiver_result(1, sequence) != Condition_Code::InactivityDetected)
        throw std::runtime_error("F2-S10 receiver inactivity fault failed");
      std::cout << "F2-S10 receiver inactivity passed\n";
    }
    {
      Config destination_config{2};
      destination_config.default_peer.inactivity_limit_ticks = 2;
      destination_config
          .default_fault_handlers[Condition_Code::InactivityDetected] =
          Fault_Handler::Ignore;
      Entity a(Config{1}), b(destination_config);
      const auto sequence = a.put(2, "ignored-inactivity.bin", Bytes(10, 9));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.tick();
      b.tick();
      if (b.receiver_complete(1, sequence) ||
          b.report_receive(1, sequence).suspended)
        throw std::runtime_error(
            "default Ignore fault handler terminated transfer");
      for (size_t i = 1; i < packets.size(); ++i)
        b.receive(packets[i]);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!b.receiver_complete(1, sequence))
        throw std::runtime_error(
            "default Ignore fault handler blocked completion");
      if (b.read_file("ignored-inactivity.bin") != Bytes(10, 9))
        throw std::runtime_error(
            "default Ignore fault handler produced wrong file");
      std::cout << "default Ignore fault handler passed\n";
    }
    {
      Config destination_config{2};
      destination_config.default_peer.inactivity_limit_ticks = 2;
      Entity a(Config{1}), b(destination_config);
      PutOptions options;
      options.metadata_options.push_back(fault_handler_override(
          Condition_Code::InactivityDetected, Fault_Handler::Suspend));
      const auto sequence = a.put(2, "suspend-on-fault.bin", Bytes(10, 9),
                                  PDU_Transmission_Mode::Acknowledged, options);
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.tick();
      b.tick();
      if (!b.report_receive(1, sequence).suspended ||
          b.receiver_complete(1, sequence))
        throw std::runtime_error("fault handler override did not suspend");
      b.resume_receive(1, sequence);
      for (size_t i = 1; i < packets.size(); ++i)
        b.receive(packets[i]);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!b.receiver_complete(1, sequence))
        throw std::runtime_error("suspended fault transfer did not resume");
      std::cout << "fault handler Suspend override passed\n";
    }
    {
      Config destination_config{2};
      destination_config
          .default_fault_handlers[Condition_Code::FileChecksumFailure] =
          Fault_Handler::Abandon;
      Entity a(Config{1}), b(destination_config);
      const auto sequence = a.put(2, "abandoned.bin", Bytes(10, 9));
      for (auto wire : a.take_outgoing()) {
        auto pdu = CFDP_PDU::from_bytes(wire);
        if (auto data = std::get_if<FileData_PDU>(&pdu.body)) {
          data->data[0] ^= 1;
          wire = pdu.to_bytes();
        }
        b.receive(wire);
      }
      bool abandoned = false;
      for (const auto &event : b.take_events())
        abandoned |= event.type == Event_Type::Abandoned;
      if (!abandoned || !b.receiver_complete(1, sequence) ||
          !b.take_completed().empty())
        throw std::runtime_error("default Abandon fault handler failed");
      for (auto &wire : b.take_outgoing())
        if (std::holds_alternative<Finished_PDU>(
                CFDP_PDU::from_bytes(wire).body))
          throw std::runtime_error("abandoned transaction emitted Finished");
      std::cout << "default Abandon fault handler passed\n";
    }
    {
      Config source_config{1};
      source_config.default_peer.ack_retry_limit = 1;
      source_config.default_peer.inactivity_limit_ticks = 100;
      source_config.default_fault_handlers[Condition_Code::AckLimitReached] =
          Fault_Handler::Suspend;
      Entity a(source_config), b(Config{2});
      const auto sequence = a.put(2, "sender-suspended.bin", Bytes(10, 2));
      a.take_outgoing(); // Keep the receiver silent until the ACK limit fault.
      a.tick();
      a.tick();
      a.take_outgoing();
      if (!a.report_send(sequence).suspended || a.sender_complete(sequence))
        throw std::runtime_error("sender default Suspend handler failed");
      a.resume_send(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence))
        throw std::runtime_error("sender fault suspension did not recover");
      std::cout << "sender Suspend fault handler passed\n";
    }
    {
      Entity a(Config{1}), b(Config{2});
      b.write_file("first", {1, 2});
      b.write_file("second", {3, 4});
      auto request = [&](std::vector<TLV> actions) {
        PutOptions options;
        options.metadata_options = std::move(actions);
        auto sequence = a.put(2, "", {}, PDU_Transmission_Mode::Acknowledged,
                              std::move(options));
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence))
          throw std::runtime_error("F3 filestore transaction failed");
        return a.sender_filestore_responses(sequence);
      };
      auto status = [&](Filestore_Action action, std::string first,
                        std::optional<std::string> second = std::nullopt) {
        auto responses = request({filestore_request(action, first, second)});
        if (responses.size() != 1)
          throw std::runtime_error("missing filestore response");
        return static_cast<unsigned>(responses[0].value[0] & 15);
      };
      if (status(Filestore_Action::CreateFile, "new") != 0 ||
          status(Filestore_Action::CreateFile, "new") != 1 ||
          status(Filestore_Action::DeleteFile, "new") != 0 ||
          status(Filestore_Action::DeleteFile, "new") != 1 ||
          status(Filestore_Action::RenameFile, "first", "renamed") != 0 ||
          status(Filestore_Action::AppendFile, "renamed", "second") != 0 ||
          b.read_file("renamed") != Bytes({1, 2, 3, 4}) ||
          status(Filestore_Action::ReplaceFile, "renamed", "second") != 0 ||
          b.read_file("renamed") != Bytes({3, 4}) ||
          status(Filestore_Action::CreateDirectory, "dir") != 0 ||
          status(Filestore_Action::RemoveDirectory, "dir") != 0 ||
          status(Filestore_Action::DenyFile, "second") != 0 ||
          b.read_file("second") ||
          status(Filestore_Action::DenyDirectory, "dir") != 0)
        throw std::runtime_error("F3 filestore action failed");
      auto skipped = request(
          {filestore_request(Filestore_Action::DeleteFile, "missing"),
           filestore_request(Filestore_Action::CreateFile, "must-not-exist")});
      if (skipped.size() != 2 || (skipped[0].value[0] & 15) != 1 ||
          (skipped[1].value[0] & 15) != 15 || b.read_file("must-not-exist"))
        throw std::runtime_error("F3 filestore skip after failure failed");
      std::cout << "F3 filestore actions passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      Bytes remote_data(50000);
      for (size_t i = 0; i < remote_data.size(); ++i)
        remote_data[i] = static_cast<uint8_t>(i & 255);
      b.write_file("remote.dat", remote_data);
      const auto request = a.proxy_put(2, 1, "remote.dat", "fetched.dat");
      for (int i = 0; i < 20; ++i) {
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
      }
      bool report_seen = false;
      for (const auto &transfer : a.take_completed())
        for (const auto &tlv : transfer.metadata_options)
          if (tlv.type == TLV_Type::MessageToUser && tlv.value.size() == 6 &&
              tlv.value[4] == 0x07 && tlv.value[5] == 2)
            report_seen = true;
      if (a.read_file("fetched.dat") != remote_data ||
          a.sender_result(request) != Condition_Code::NoError || !report_seen)
        throw std::runtime_error("F3 two-party proxy get failed");
      std::cout << "F3 two-party proxy get passed\n";
    }
    {
      Entity a(Config{1}), b(Config{2});
      b.write_file("alpha", {1});
      b.write_file("beta", {2});
      const auto request = a.directory_listing(2, "", "listing.txt");
      for (int i = 0; i < 20; ++i) {
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
      }
      auto listing = a.read_file("listing.txt");
      if (!listing ||
          std::string(listing->begin(), listing->end()) != "alpha\nbeta\n" ||
          a.sender_result(request) != Condition_Code::NoError)
        throw std::runtime_error("F3 directory listing failed");
      std::cout << "F3 directory listing passed\n";
    }
    {
      Entity a(Config{1}), b(Config{2});
      Bytes data(1024, 0x37);
      auto sequence = a.put(2, "resumed.bin", data);
      a.suspend_send(sequence);
      if (!a.take_outgoing().empty() || !a.report_send(sequence).suspended)
        throw std::runtime_error("sender suspension did not stop PDUs");
      a.resume_send(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      auto events = a.take_events();
      bool suspended = false, resumed = false, reported = false;
      for (const auto &event : events) {
        suspended |= event.type == Event_Type::Suspended;
        resumed |= event.type == Event_Type::Resumed;
        reported |= event.type == Event_Type::Report;
      }
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence) ||
          !suspended || !resumed || !reported ||
          b.read_file("resumed.bin") != data)
        throw std::runtime_error("suspend/resume/report failed");
      std::cout << "suspend, resume and report passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 4, .ut_address = {}}});
      const auto sequence = a.put(2, "receiver-resume.bin", Bytes(8, 0x23));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.receive(packets[1]);
      b.suspend_receive(1, sequence);
      b.receive(packets[2]);
      b.receive(packets[3]);
      if (b.receiver_complete(1, sequence) ||
          !b.report_receive(1, sequence).suspended)
        throw std::runtime_error("F4 receiver suspension failed");
      b.resume_receive(1, sequence);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!b.receiver_complete(1, sequence) || !a.sender_complete(sequence))
        throw std::runtime_error("F4 receiver resume failed");
      std::cout << "F4 receiver suspend and resume passed\n";
    }
    {
      Config receiver_config{
          .entity_id = 2,
          .default_peer = PeerConfig{.segment_size = 4, .ut_address = {}}};
      receiver_config.default_peer.nak_mode = Nak_Mode::Immediate;
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(receiver_config);
      const auto sequence = a.put(2, "gaps.bin", Bytes(12, 7));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.receive(packets[1]);
      if (!b.take_outgoing().empty())
        throw std::runtime_error("F4 immediate NAK sent for contiguous data");
      b.receive(packets[3]);
      auto responses = b.take_outgoing();
      if (responses.size() != 1 || kind(responses[0]) != Kind::Nak)
        throw std::runtime_error("F4 immediate NAK missing");
      const auto nak =
          std::get<NAK_PDU>(CFDP_PDU::from_bytes(responses[0]).body);
      if (nak.startOfScope != 0 || nak.endOfScope != 12 ||
          nak.segmentRequests.size() != 1 ||
          nak.segmentRequests[0].startOffset != 4 ||
          nak.segmentRequests[0].endOffset != 8)
        throw std::runtime_error("F4 immediate NAK range wrong");
      a.receive(responses[0]);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      b.receive(packets[4]);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      if (!a.sender_complete(sequence) || !b.receiver_complete(1, sequence))
        throw std::runtime_error("F4 immediate NAK recovery failed");
      std::cout << "F4 immediate NAK passed\n";
    }
    {
      Config receiver_config{
          .entity_id = 2,
          .default_peer = PeerConfig{.segment_size = 4, .ut_address = {}}};
      receiver_config.default_peer.nak_mode = Nak_Mode::Prompted;
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(receiver_config);
      const auto sequence = a.put(2, "prompt.bin", Bytes(8, 3));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.receive(packets[2]);
      if (!b.take_outgoing().empty())
        throw std::runtime_error("F4 prompted mode sent unsolicited NAK");
      a.prompt_nak(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      auto responses = b.take_outgoing();
      if (responses.size() != 1 || kind(responses[0]) != Kind::Nak)
        throw std::runtime_error("F4 prompt NAK missing");
      const auto nak =
          std::get<NAK_PDU>(CFDP_PDU::from_bytes(responses[0]).body);
      if (nak.segmentRequests.size() != 1 ||
          nak.segmentRequests[0].startOffset != 0 ||
          nak.segmentRequests[0].endOffset != 4)
        throw std::runtime_error("F4 prompt NAK range wrong");
      a.prompt_keep_alive(sequence);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      auto keepalives = b.take_outgoing();
      if (keepalives.size() != 1 ||
          std::get<KeepAlive_PDU>(CFDP_PDU::from_bytes(keepalives[0]).body)
                  .progress != 4)
        throw std::runtime_error("F4 prompted Keep Alive failed");
      std::cout << "F4 prompted NAK and Keep Alive passed\n";
    }
    {
      Config receiver_config{
          .entity_id = 2,
          .default_peer = PeerConfig{.segment_size = 4, .ut_address = {}}};
      receiver_config.default_peer.nak_mode = Nak_Mode::Asynchronous;
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(receiver_config);
      const auto sequence = a.put(2, "async.bin", Bytes(8, 3));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.receive(packets[2]);
      b.request_nak(1, sequence);
      auto responses = b.take_outgoing();
      if (responses.size() != 1 || kind(responses[0]) != Kind::Nak ||
          std::get<NAK_PDU>(CFDP_PDU::from_bytes(responses[0]).body)
                  .segmentRequests.size() != 1)
        throw std::runtime_error("F4 asynchronous NAK failed");
      std::cout << "F4 asynchronous NAK passed\n";
    }
    {
      Config receiver_config{
          .entity_id = 2,
          .default_peer = PeerConfig{.segment_size = 4, .ut_address = {}}};
      receiver_config.default_peer.keep_alive_interval_ticks = 2;
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(receiver_config);
      a.put(2, "keepalive.bin", Bytes(8, 3));
      auto packets = a.take_outgoing();
      b.receive(packets[0]);
      b.receive(packets[1]);
      b.tick();
      if (!b.take_outgoing().empty())
        throw std::runtime_error("F4 Keep Alive sent before interval");
      b.tick();
      auto responses = b.take_outgoing();
      if (responses.size() != 1 ||
          std::get<KeepAlive_PDU>(CFDP_PDU::from_bytes(responses[0]).body)
                  .progress != 4)
        throw std::runtime_error("F4 periodic Keep Alive failed");
      b.receive(packets.back());
      b.take_outgoing();
      b.tick();
      b.tick();
      for (auto &wire : b.take_outgoing())
        if (std::holds_alternative<KeepAlive_PDU>(
                CFDP_PDU::from_bytes(wire).body))
          throw std::runtime_error("F4 Keep Alive continued after EOF");
      std::cout << "F4 periodic Keep Alive passed\n";
    }
    {
      Config source_config{
          .entity_id = 1,
          .default_peer = PeerConfig{.segment_size = 128, .ut_address = {}}};
      source_config.default_peer.crc = true;
      Entity a(source_config),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 128, .ut_address = {}}});
      Bytes content(512, 0x61);
      const auto sequence = a.put(2, "crc.bin", content);
      bool corrupted = false;
      for (auto wire : a.take_outgoing()) {
        if (!corrupted && kind(wire) == Kind::Data) {
          wire[wire.size() - 3] ^= 1;
          corrupted = true;
        }
        b.receive(wire);
      }
      for (int i = 0; i < 20; ++i) {
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        if (a.sender_complete(sequence) && b.receiver_complete(1, sequence))
          break;
        a.tick();
        b.tick();
      }
      auto completed = b.take_completed();
      if (!corrupted || completed.size() != 1 || completed[0].data != content ||
          !a.sender_complete(sequence))
        throw std::runtime_error("F4 CRC recovery failed");
      std::cout << "F4 CRC recovery passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 128, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 128, .ut_address = {}}});
      PutOptions options;
      options.unbounded = true;
      Bytes content(500, 0x29);
      const auto sequence = a.put(2, "unbounded.bin", content,
                                  PDU_Transmission_Mode::Acknowledged, options);
      auto packets = a.take_outgoing();
      const auto metadata = CFDP_PDU::from_bytes(packets.front());
      if (metadata.header.largeFileFlag != PDU_Large_File_Flag::LargeFile ||
          std::get<Metadata_PDU>(metadata.body).fileSize != 0)
        throw std::runtime_error("F4 unbounded Metadata encoding failed");
      for (auto &wire : packets)
        b.receive(wire);
      for (auto &wire : b.take_outgoing())
        a.receive(wire);
      for (auto &wire : a.take_outgoing())
        b.receive(wire);
      auto completed = b.take_completed();
      if (completed.size() != 1 || completed[0].data != content ||
          !a.sender_complete(sequence))
        throw std::runtime_error("F4 unbounded transfer failed");
      std::cout << "F4 unbounded transfer passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 4, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 4, .ut_address = {}}});
      PutOptions options;
      options.record_lengths = {3, 9};
      Bytes content(12, 0x52);
      const auto sequence = a.put(2, "records.bin", content,
                                  PDU_Transmission_Mode::Acknowledged, options);
      auto packets = a.take_outgoing();
      const std::vector<Record_Continuation_State> expected_states = {
          Record_Continuation_State::Complete, Record_Continuation_State::Start,
          Record_Continuation_State::NoBoundary,
          Record_Continuation_State::End};
      size_t data_index = 0;
      for (auto &wire : packets) {
        auto pdu = CFDP_PDU::from_bytes(wire);
        if (auto segment = std::get_if<FileData_PDU>(&pdu.body)) {
          if (pdu.header.segmentationControl !=
                  PDU_Segmentation_Control::BoundariesPreserved ||
              pdu.header.segmentMetadataFlag !=
                  PDU_Segment_Metadata_Flag::Present ||
              data_index >= expected_states.size() ||
              segment->recordContinuationState != expected_states[data_index])
            throw std::runtime_error("F4 record continuation state wrong");
          if (++data_index == 2)
            continue; // Force retransmission of a record start.
        }
        b.receive(wire);
      }
      for (int i = 0; i < 10; ++i) {
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
        for (auto &wire : a.take_outgoing()) {
          auto pdu = CFDP_PDU::from_bytes(wire);
          if (auto segment = std::get_if<FileData_PDU>(&pdu.body);
              segment && segment->offset == 3 &&
              segment->recordContinuationState !=
                  Record_Continuation_State::Start)
            throw std::runtime_error("F4 retransmission lost record state");
          b.receive(wire);
        }
        if (a.sender_complete(sequence) && b.receiver_complete(1, sequence))
          break;
        a.tick();
        b.tick();
      }
      auto completed = b.take_completed();
      if (data_index != expected_states.size() || completed.size() != 1 ||
          completed[0].data != content || !a.sender_complete(sequence))
        throw std::runtime_error("F4 record segmentation transfer failed");
      std::cout << "F4 record segmentation passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 256, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 256, .ut_address = {}}});
      for (int i = 0; i < 5; ++i)
        a.put(2, "clean-" + std::to_string(i),
              Bytes(50000, static_cast<uint8_t>(i)));
      for (const auto &wire : a.take_outgoing())
        b.receive(wire);
      for (const auto &wire : b.take_outgoing())
        a.receive(wire);
      for (const auto &wire : a.take_outgoing())
        b.receive(wire);
      auto completed = b.take_completed();
      if (completed.size() != 5)
        throw std::runtime_error("F4-S13 clean concurrent transactions failed");
      for (const auto &item : completed)
        if (item.data !=
                Bytes(50000, static_cast<uint8_t>(item.sequence - 1)) ||
            a.sender_result(item.sequence) != Condition_Code::NoError)
          throw std::runtime_error("F4-S13 concurrent data mismatch");
      std::cout << "F4-S13 five clean concurrent transactions passed\n";
    }
    {
      Entity a(Config{.entity_id = 1,
                      .default_peer =
                          PeerConfig{.segment_size = 64, .ut_address = {}}}),
          b(Config{.entity_id = 2,
                   .default_peer =
                       PeerConfig{.segment_size = 64, .ut_address = {}}});
      std::vector<uint64_t> sequences;
      std::map<uint64_t, Bytes> expected;
      std::set<uint64_t> dropped;
      for (int i = 0; i < 5; ++i) {
        Bytes content(500 + i * 13, static_cast<uint8_t>(i + 1));
        auto sequence = a.put(2, "multi-" + std::to_string(i), content);
        sequences.push_back(sequence);
        expected.emplace(sequence, std::move(content));
      }
      for (auto &wire : a.take_outgoing()) {
        auto pdu = CFDP_PDU::from_bytes(wire);
        if (std::holds_alternative<FileData_PDU>(pdu.body) &&
            dropped.insert(pdu.header.transactionSequenceNumber).second)
          continue;
        b.receive(wire);
      }
      for (int i = 0; i < 40; ++i) {
        for (auto &wire : b.take_outgoing())
          a.receive(wire);
        for (auto &wire : a.take_outgoing())
          b.receive(wire);
        if (std::all_of(sequences.begin(), sequences.end(),
                        [&](uint64_t sequence) {
                          return a.sender_complete(sequence) &&
                                 b.receiver_complete(1, sequence);
                        }))
          break;
        a.tick();
        b.tick();
      }
      auto completed = b.take_completed();
      if (completed.size() != 5 || dropped.size() != 5)
        throw std::runtime_error("F4 multiple transactions missing");
      for (const auto &transfer : completed)
        if (expected.at(transfer.sequence) != transfer.data ||
            !a.sender_complete(transfer.sequence))
          throw std::runtime_error("F4 multiple transaction data mismatch");
      std::cout << "F4-S14 multiple open transactions with loss passed\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
