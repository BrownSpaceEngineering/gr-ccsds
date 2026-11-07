#include "entity.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace cfdp;

namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename F> void rejects(F &&action, const char *message) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}

void class_one_check_timers() {
  Config sender_config{1};
  sender_config.default_peer.inactivity_limit_ticks = 100;
  PeerConfig destination;
  destination.check_limit = 2;
  destination.inactivity_limit_ticks = 100;
  sender_config.peers[2] = destination;
  Entity sender(sender_config);
  PutOptions options;
  options.closure_requested = true;
  const auto sequence = sender.put(
      2, "closed", {1, 2, 3}, PDU_Transmission_Mode::Unacknowledged, options);
  sender.take_outgoing();
  sender.advance(std::chrono::milliseconds(999));
  check(!sender.sender_complete(sequence), "Check timer fired early");
  sender.advance(std::chrono::milliseconds(1));
  check(!sender.sender_complete(sequence),
        "Check limit fired after one interval");
  sender.tick();
  check(sender.sender_result(sequence) == Condition_Code::CheckLimitReached,
        "closure sender did not report Check limit");

  Config receiver_config{2};
  receiver_config.peers[1] = destination;
  Entity receiver(receiver_config);
  Entity source(Config{1});
  source.put(2, "missing", Bytes(10, 7), PDU_Transmission_Mode::Unacknowledged);
  for (const auto &wire : source.take_outgoing()) {
    const auto pdu = CFDP_PDU::from_bytes(wire);
    if (!std::holds_alternative<FileData_PDU>(pdu.body))
      receiver.receive(wire);
  }
  receiver.tick();
  check(!receiver.receiver_result(1, 1), "receiver Check limit fired early");
  receiver.tick();
  check(receiver.receiver_result(1, 1) == Condition_Code::CheckLimitReached,
        "missing Class 1 data did not reach Check limit");
}

void per_peer_and_link_opportunity() {
  Config source_config{1};
  source_config.peers[2].segment_size = 3;
  source_config.peers[2].crc = true;
  Entity source(source_config), destination(Config{2});
  source.set_link_opportunity(2, false, true);
  const auto sequence = source.put(2, "peer.bin", Bytes(7, 0x55));
  check(source.take_outgoing().empty(),
        "PDUs escaped closed transmit opportunity");
  source.tick();
  check(source.take_outgoing().empty() && !source.sender_complete(sequence),
        "closed link advanced a transfer");
  source.set_link_opportunity(2, true, true);
  auto packets = source.take_outgoing();
  check(packets.size() == 5, "peer segment size was ignored");
  for (const auto &packet : packets)
    check(CFDP_PDU::from_bytes(packet).header.crcFlag == PDU_CRC_Flag::Present,
          "peer CRC setting was ignored");
  destination.set_link_opportunity(1, true, false);
  destination.receive(packets.front());
  check(destination.take_events().empty(),
        "closed reception opportunity accepted PDU");
  destination.set_link_opportunity(1, true, true);
  for (const auto &packet : packets)
    destination.receive(packet);
  auto events = destination.take_events();
  const auto metadata =
      std::find_if(events.begin(), events.end(), [](const Event &event) {
        return event.type == Event_Type::MetadataReceived;
      });
  check(metadata != events.end() && metadata->file_size == 7 &&
            metadata->destination_name == "peer.bin",
        "Metadata event lacks details");
  const auto segment =
      std::find_if(events.begin(), events.end(), [](const Event &event) {
        return event.type == Event_Type::FileSegmentReceived;
      });
  check(segment != events.end() && segment->segment_offset == 0 &&
            segment->segment_length == 3,
        "segment event lacks details");
  for (const auto &packet : destination.take_outgoing())
    source.receive(packet);
  for (const auto &packet : source.take_outgoing())
    destination.receive(packet);
  check(source.sender_complete(sequence) &&
            destination.receiver_complete(1, sequence),
        "peer transfer did not complete");
}

void independent_timers_and_capacity() {
  Config source_config{1};
  source_config.max_active_transactions = 1;
  PeerConfig settings;
  settings.ack_interval_ticks = 2;
  settings.ack_retry_limit = 1;
  settings.inactivity_limit_ticks = 100;
  source_config.peers[2] = settings;
  Entity source(source_config);
  const auto sequence = source.put(2, "ack", {1});
  rejects([&] { source.put(2, "second", {2}); },
          "active transaction limit was ignored");
  source.take_outgoing();
  source.tick();
  check(source.take_outgoing().empty(), "ACK retry fired before its interval");
  source.tick();
  check(source.take_outgoing().size() == 1, "ACK retry did not fire");
  source.tick();
  check(!source.sender_complete(sequence),
        "ACK limit fired before second interval");
  source.tick();
  check(source.sender_result(sequence) == Condition_Code::AckLimitReached,
        "ACK limit fault not reported");
  const auto cancellation = source.take_outgoing();
  rejects([&] { source.forget_send(sequence); },
          "sender forgot an unacknowledged cancellation");
  check(!cancellation.empty(), "cancellation EOF was not emitted");
  auto response_header = CFDP_PDU::from_bytes(cancellation.back()).header;
  response_header.direction = PDU_Direction::ToFileSender;
  source.receive(
      CFDP_PDU{response_header, ACK_PDU{Acknowledged_Directive::EndOfFile,
                                        Condition_Code::AckLimitReached,
                                        Transaction_Status::Active}}
          .to_bytes());
  source.forget_send(sequence);
  check(!source.sender_result(sequence),
        "forgotten transaction retained result");
  check(source.put(2, "next", {3}) != sequence,
        "sequence was unexpectedly reused before wraparound");
}

void malformed_metadata_is_atomic() {
  Entity source(Config{1}), destination(Config{2});
  source.put(2, "valid", {1, 2, 3});
  auto packets = source.take_outgoing();
  auto invalid = CFDP_PDU::from_bytes(packets[0]);
  std::get<Metadata_PDU>(invalid.body)
      .options.push_back({TLV_Type::FilestoreRequest, {0x20, 3, 'a'}});
  rejects([&] { destination.receive(invalid.to_bytes()); },
          "malformed filestore request was accepted");
  check(destination.take_events().empty(),
        "malformed Metadata changed transaction state");
  for (const auto &packet : packets)
    destination.receive(packet);
  check(destination.read_file("valid") == Bytes({1, 2, 3}),
        "valid retry failed after malformed Metadata");

  PDU_Header header;
  header.sourceEntityID = 7;
  header.destinationEntityID = 2;
  header.transactionSequenceNumber = 8;
  rejects(
      [&] {
        destination.receive(
            CFDP_PDU{header, ACK_PDU{Acknowledged_Directive::Finished,
                                     Condition_Code::NoError,
                                     Transaction_Status::Active}}
                .to_bytes());
      },
      "an ACK created a receiver transaction");
  rejects([&] { destination.report_receive(7, 8); },
          "invalid first PDU left receiver state");
}

void queue_limit_is_atomic() {
  Config config{1};
  config.max_queued_pdu_bytes = 20;
  Entity source(config);
  rejects([&] { source.put(2, "too-big", Bytes(100, 1)); },
          "outgoing queue limit was ignored");
  check(source.take_outgoing().empty() && !source.sender_complete(1),
        "failed Put left transaction or queued PDUs");
}

void finished_waits_for_queue_space() {
  Entity source(Config{1});
  const auto sequence = source.put(2, "queued", {1, 2, 3});
  const auto packets = source.take_outgoing();
  auto reverse_header = CFDP_PDU::from_bytes(packets.back()).header;
  reverse_header.direction = PDU_Direction::ToFileSender;
  const auto ack_size =
      CFDP_PDU{reverse_header,
               ACK_PDU{Acknowledged_Directive::EndOfFile,
                       Condition_Code::NoError, Transaction_Status::Active}}
          .to_bytes()
          .size();
  const auto finished_size =
      CFDP_PDU{reverse_header, Finished_PDU{Condition_Code::NoError,
                                            Delivery_Code::Complete,
                                            File_Status::Retained,
                                            {},
                                            {}}}
          .to_bytes()
          .size();
  Config receiver_config{2};
  receiver_config.max_queued_pdu_bytes = ack_size + finished_size - 1;
  Entity receiver(receiver_config);

  for (const auto &packet : packets)
    receiver.receive(packet);
  auto completed = receiver.take_completed();
  check(completed.size() == 1 && completed.front().data == Bytes({1, 2, 3}),
        "full queue prevented delivery or duplicated the file");
  check(!receiver.receiver_complete(1, sequence),
        "receiver completed before Finished was acknowledged");
  auto replies = receiver.take_outgoing();
  check(replies.size() == 2 &&
            std::holds_alternative<ACK_PDU>(
                CFDP_PDU::from_bytes(replies[0]).body) &&
            std::holds_alternative<Finished_PDU>(
                CFDP_PDU::from_bytes(replies[1]).body),
        "pending Finished was not sent after queue space opened");
  for (const auto &reply : replies)
    source.receive(reply);
  for (const auto &reply : source.take_outgoing())
    receiver.receive(reply);
  check(source.sender_complete(sequence) &&
            receiver.receiver_complete(1, sequence) &&
            receiver.take_completed().empty(),
        "Finished ACK exchange did not complete exactly once");

  Entity closure_source(Config{3}), closure_receiver(Config{4});
  PutOptions options;
  options.closure_requested = true;
  closure_source.put(4, "closure", {7}, PDU_Transmission_Mode::Unacknowledged,
                     options);
  for (const auto &packet : closure_source.take_outgoing())
    closure_receiver.receive(packet);
  rejects([&] { closure_receiver.forget_receive(3, 1); },
          "receiver forgot a pending Class 1 Finished PDU");
}

void failed_finished_waits_for_queue_space() {
  Entity source(Config{1});
  const auto sequence = source.put(2, "fault", {1});
  auto packets = source.take_outgoing();
  auto reverse_header = CFDP_PDU::from_bytes(packets.back()).header;
  reverse_header.direction = PDU_Direction::ToFileSender;
  const auto ack_size =
      CFDP_PDU{reverse_header,
               ACK_PDU{Acknowledged_Directive::EndOfFile,
                       Condition_Code::NoError, Transaction_Status::Active}}
          .to_bytes()
          .size();
  const auto finished_size =
      CFDP_PDU{reverse_header, Finished_PDU{Condition_Code::FileChecksumFailure,
                                            Delivery_Code::Incomplete,
                                            File_Status::DiscardedDeliberately,
                                            {},
                                            2}}
          .to_bytes()
          .size();
  Config config{2};
  config.max_queued_pdu_bytes = ack_size + finished_size - 1;
  Entity receiver(config);
  auto corrupted = CFDP_PDU::from_bytes(packets[1]);
  std::get<FileData_PDU>(corrupted.body).data[0] = 2;
  packets[1] = corrupted.to_bytes();
  for (const auto &packet : packets)
    receiver.receive(packet);
  check(receiver.receiver_result(1, sequence) ==
                Condition_Code::FileChecksumFailure &&
            receiver.take_completed().empty(),
        "checksum fault was not retained with a full queue");
  const auto replies = receiver.take_outgoing();
  check(replies.size() == 2 &&
            std::holds_alternative<ACK_PDU>(
                CFDP_PDU::from_bytes(replies[0]).body) &&
            std::get<Finished_PDU>(CFDP_PDU::from_bytes(replies[1]).body)
                    .condition == Condition_Code::FileChecksumFailure,
        "failed Finished was lost when the queue was full");
  for (const auto &reply : replies)
    source.receive(reply);
  for (const auto &reply : source.take_outgoing())
    receiver.receive(reply);
  check(receiver.receiver_complete(1, sequence),
        "failed Finished ACK exchange did not complete");
}

void cancelled_finished_waits_for_queue_space() {
  Entity source(Config{1});
  const auto sequence = source.put(2, "cancel", {1});
  const auto packets = source.take_outgoing();
  auto reverse_header = CFDP_PDU::from_bytes(packets.back()).header;
  reverse_header.direction = PDU_Direction::ToFileSender;
  const auto nak_size =
      CFDP_PDU{reverse_header, NAK_PDU{0, 0, {}}}.to_bytes().size();
  const auto finished_size =
      CFDP_PDU{reverse_header, Finished_PDU{Condition_Code::CancelRequested,
                                            Delivery_Code::Incomplete,
                                            File_Status::DiscardedDeliberately,
                                            {},
                                            2}}
          .to_bytes()
          .size();
  Config config{2};
  config.max_queued_pdu_bytes = nak_size + finished_size - 1;
  Entity receiver(config);
  receiver.receive(packets.front());
  receiver.request_nak(1, sequence);
  receiver.cancel_receive(1, sequence);
  const auto replies = receiver.take_outgoing();
  check(replies.size() == 2 &&
            std::holds_alternative<NAK_PDU>(
                CFDP_PDU::from_bytes(replies[0]).body) &&
            std::get<Finished_PDU>(CFDP_PDU::from_bytes(replies[1]).body)
                    .condition == Condition_Code::CancelRequested,
        "cancelled Finished was lost when the queue was full");
  auto forward_header = reverse_header;
  forward_header.direction = PDU_Direction::ToFileReceiver;
  receiver.receive(
      CFDP_PDU{forward_header, ACK_PDU{Acknowledged_Directive::Finished,
                                       Condition_Code::CancelRequested,
                                       Transaction_Status::Terminated}}
          .to_bytes());
  check(receiver.receiver_complete(1, sequence),
        "cancelled receiver did not complete after Finished ACK");
}

void failed_completion_does_not_change_filestore() {
  Entity source(Config{1});
  PutOptions options;
  options.metadata_options.push_back(
      filestore_request(Filestore_Action::CreateFile, "created"));
  const auto sequence = source.put(
      2, "target", {1, 2}, PDU_Transmission_Mode::Acknowledged, options);
  const auto packets = source.take_outgoing();
  auto reverse_header = CFDP_PDU::from_bytes(packets.back()).header;
  reverse_header.direction = PDU_Direction::ToFileSender;
  Config config{2};
  config.max_queued_pdu_bytes =
      CFDP_PDU{reverse_header,
               ACK_PDU{Acknowledged_Directive::EndOfFile,
                       Condition_Code::NoError, Transaction_Status::Active}}
          .to_bytes()
          .size();
  Entity receiver(config);
  receiver.write_file("target", {9});
  for (size_t i = 0; i + 1 < packets.size(); ++i)
    receiver.receive(packets[i]);
  rejects([&] { receiver.receive(packets.back()); },
          "oversize Finished was accepted");
  check(receiver.read_file("target") == Bytes({9}) &&
            !receiver.read_file("created") &&
            receiver.take_completed().empty() &&
            !receiver.receiver_result(1, sequence),
        "failed completion changed filestore or receiver state");
}

void class_one_waits_for_eof_issue() {
  Entity source(Config{1});
  source.set_link_opportunity(2, false, true);
  const auto sequence =
      source.put(2, "one", {1}, PDU_Transmission_Mode::Unacknowledged);
  check(!source.sender_complete(sequence),
        "Class 1 sender completed before issuing EOF");
  source.set_link_opportunity(2, true, true);
  source.take_outgoing();
  check(source.sender_result(sequence) == Condition_Code::NoError,
        "Class 1 sender did not complete after issuing EOF");
}

void finished_ack_limit_is_observable() {
  Config receiver_config{2};
  receiver_config.default_peer.ack_retry_limit = 1;
  receiver_config.default_peer.inactivity_limit_ticks = 100;
  Entity source(Config{1}), receiver(receiver_config);
  source.put(2, "finished", {1});
  for (const auto &packet : source.take_outgoing())
    receiver.receive(packet);
  receiver.take_outgoing(); // Drop ACK(EOF) and the first Finished.
  receiver.tick();
  receiver.take_outgoing(); // Drop the retransmitted Finished.
  receiver.tick();
  check(receiver.receiver_complete(1, 1),
        "Finished ACK limit did not end receiver transaction");
  bool abandoned = false;
  for (const auto &event : receiver.take_events())
    abandoned |= event.type == Event_Type::Abandoned &&
                 event.condition == Condition_Code::AckLimitReached;
  check(abandoned, "Finished ACK limit did not emit abandonment");
}

void keep_alive_fault() {
  Config source_config{1};
  PeerConfig settings;
  settings.keep_alive_limit = 2;
  settings.inactivity_limit_ticks = 100;
  source_config.peers[2] = settings;
  Entity source(source_config);
  const auto sequence = source.put(2, "stalled", Bytes(4, 1));
  source.take_outgoing();
  PDU_Header header;
  header.sourceEntityID = 1;
  header.destinationEntityID = 2;
  header.transactionSequenceNumber = sequence;
  header.direction = PDU_Direction::ToFileSender;
  const auto stalled = CFDP_PDU{header, KeepAlive_PDU{0}}.to_bytes();
  source.receive(stalled);
  check(!source.sender_complete(sequence), "Keep Alive limit fired too early");
  source.receive(stalled);
  check(source.sender_result(sequence) == Condition_Code::KeepAliveLimitReached,
        "stalled Keep Alive progress was ignored");
}

void reject_inconsistent_reverse_pdus() {
  Entity source(Config{1});
  source.put(2, "reverse", {1, 2, 3});
  auto outgoing = source.take_outgoing();
  auto reverse_header = CFDP_PDU::from_bytes(outgoing.back()).header;
  reverse_header.direction = PDU_Direction::ToFileSender;
  auto inconsistent = reverse_header;
  inconsistent.transmissionMode = PDU_Transmission_Mode::Unacknowledged;
  rejects(
      [&] {
        source.receive(
            CFDP_PDU{inconsistent, ACK_PDU{Acknowledged_Directive::EndOfFile,
                                           Condition_Code::NoError,
                                           Transaction_Status::Active}}
                .to_bytes());
      },
      "reverse PDU with a different transmission mode was accepted");
  rejects(
      [&] {
        source.receive(CFDP_PDU{reverse_header, NAK_PDU{0, 3, {{0, 0}, {2, 4}}}}
                           .to_bytes());
      },
      "invalid NAK range was accepted");
  check(source.take_outgoing().empty(),
        "invalid NAK emitted retransmissions before validation");

  Entity unacknowledged(Config{1});
  unacknowledged.put(2, "no-closure", {1},
                     PDU_Transmission_Mode::Unacknowledged);
  auto packets = unacknowledged.take_outgoing();
  auto class_one_reverse = CFDP_PDU::from_bytes(packets.back()).header;
  class_one_reverse.direction = PDU_Direction::ToFileSender;
  rejects(
      [&] {
        unacknowledged.receive(CFDP_PDU{
            class_one_reverse,
            Finished_PDU{Condition_Code::NoError,
                         Delivery_Code::Complete,
                         File_Status::Retained,
                         {},
                         {}}}.to_bytes());
      },
      "Class 1 sender accepted Finished without closure");
}

void generated_record_segments() {
  Entity source(
      Config{.entity_id = 1,
             .default_peer = PeerConfig{.segment_size = 3, .ut_address = {}}}),
      destination(Config{2});
  PutOptions options;
  options.record_delimiter = '\n';
  options.segment_metadata = {0xa5, 0x5a};
  const Bytes content = {'a', 'b', '\n', 'c', 'd', 'e', 'f', '\n', 'x', 'y'};
  const auto sequence = source.put(
      2, "records", content, PDU_Transmission_Mode::Acknowledged, options);
  const std::vector<Record_Continuation_State> expected = {
      Record_Continuation_State::Complete, Record_Continuation_State::Start,
      Record_Continuation_State::End, Record_Continuation_State::Complete};
  size_t index = 0;
  for (const auto &wire : source.take_outgoing()) {
    const auto pdu = CFDP_PDU::from_bytes(wire);
    if (const auto *segment = std::get_if<FileData_PDU>(&pdu.body)) {
      check(index < expected.size() &&
                segment->recordContinuationState == expected[index],
            "automatic record boundaries were encoded incorrectly");
      check(pdu.header.segmentMetadataFlag ==
                    PDU_Segment_Metadata_Flag::Present &&
                segment->segmentMetadata == options.segment_metadata,
            "generated segment metadata was lost");
      ++index;
    }
    destination.receive(wire);
  }
  for (const auto &wire : destination.take_outgoing())
    source.receive(wire);
  for (const auto &wire : source.take_outgoing())
    destination.receive(wire);
  check(index == expected.size() && source.sender_complete(sequence) &&
            destination.take_completed().front().data == content,
        "automatic record transfer did not complete");

  PutOptions bad;
  bad.segment_metadata = Bytes(64, 0);
  rejects(
      [&] {
        source.put(2, "too-long", {1}, PDU_Transmission_Mode::Acknowledged,
                   bad);
      },
      "oversize segment metadata was accepted");
  bad.segment_metadata.clear();
  bad.record_delimiter = '\n';
  bad.record_lengths = {1};
  rejects(
      [&] {
        source.put(2, "ambiguous", {1}, PDU_Transmission_Mode::Acknowledged,
                   bad);
      },
      "two record boundary policies were accepted");
}

void optional_file_checksums() {
  const Bytes vector = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(file_checksum(vector, 2) == 0xe3069283u, "CRC-32C vector failed");
  check(file_checksum(vector, 3) == 0xcbf43926u, "CRC-32 vector failed");
  for (const uint8_t type : {uint8_t{2}, uint8_t{3}}) {
    Entity source(Config{1}), destination(Config{2});
    PutOptions options;
    options.checksum_type = type;
    const auto sequence = source.put(
        2, "crc", vector, PDU_Transmission_Mode::Acknowledged, options);
    for (const auto &wire : source.take_outgoing())
      destination.receive(wire);
    for (const auto &wire : destination.take_outgoing())
      source.receive(wire);
    for (const auto &wire : source.take_outgoing())
      destination.receive(wire);
    check(source.sender_result(sequence) == Condition_Code::NoError &&
              destination.take_completed().front().data == vector,
          "optional checksum transfer failed");
  }
}

void failed_transport_send_is_retained() {
  Entity source(Config{1});
  const auto sequence =
      source.put(2, "retry", {1, 2, 3}, PDU_Transmission_Mode::Unacknowledged);
  size_t attempts = 0;
  check(source.send_outgoing([&](uint64_t peer, const Bytes &) {
    check(peer == 2, "transport received the wrong peer ID");
    ++attempts;
    return false;
  }) == 0 && attempts == 1 &&
            !source.sender_complete(sequence),
        "failed transport send advanced the transaction");
  std::vector<Bytes> delivered;
  check(source.send_outgoing([&](uint64_t, const Bytes &wire) {
    delivered.push_back(wire);
    return true;
  }) == 3 && delivered.size() == 3 &&
            source.sender_complete(sequence),
        "retained PDUs were not retried");
  check(source.take_outgoing().empty(),
        "successful transport send left queued PDUs");
}

void remote_status_report() {
  Entity requester(Config{1}), remote(Config{2});
  const auto target = requester.put(2, "target", {4, 5, 6});
  for (const auto &wire : requester.take_outgoing())
    remote.receive(wire);
  for (const auto &wire : remote.take_outgoing())
    requester.receive(wire);
  for (const auto &wire : requester.take_outgoing())
    remote.receive(wire);
  const auto request = requester.remote_status(2, 1, target, "status.txt");
  for (int i = 0; i < 12; ++i) {
    for (const auto &wire : requester.take_outgoing())
      remote.receive(wire);
    for (const auto &wire : remote.take_outgoing())
      requester.receive(wire);
  }
  const auto report = requester.read_file("status.txt");
  check(requester.sender_result(request) == Condition_Code::NoError && report &&
            std::string(report->begin(), report->end())
                    .find("source=1 sequence=") == 0,
        "remote status report did not return a file");
  const auto unknown = requester.remote_status(2, 1, 99, "unknown.txt");
  for (int i = 0; i < 12; ++i) {
    for (const auto &wire : requester.take_outgoing())
      remote.receive(wire);
    for (const auto &wire : remote.take_outgoing())
      requester.receive(wire);
  }
  check(requester.sender_result(unknown) == Condition_Code::NoError &&
            !requester.read_file("unknown.txt"),
        "unknown remote status generated a report file");
}

void remote_suspension_orders() {
  Entity requester(Config{1}), remote(Config{2});
  auto pump = [&] {
    for (int i = 0; i < 12; ++i) {
      for (const auto &wire : requester.take_outgoing())
        remote.receive(wire);
      for (const auto &wire : remote.take_outgoing())
        requester.receive(wire);
    }
  };
  const auto target = remote.put(1, "suspended", {1, 2, 3});
  const auto suspend = requester.remote_suspend(2, 2, target);
  pump();
  check(requester.sender_result(suspend) == Condition_Code::NoError &&
            remote.report_send(target).suspended,
        "remote suspend did not suspend target sender");
  const auto resume = requester.remote_resume(2, 2, target);
  pump();
  check(requester.sender_result(resume) == Condition_Code::NoError &&
            !remote.report_send(target).suspended &&
            requester.read_file("suspended") == Bytes({1, 2, 3}),
        "remote resume did not release target sender");

  const auto local_target = remote.put(1, "local", {7});
  remote.suspend_send(local_target);
  requester.remote_suspend(2, 2, local_target);
  pump();
  requester.remote_resume(2, 2, local_target);
  pump();
  check(remote.report_send(local_target).suspended,
        "remote resume cleared a local suspension");
  remote.resume_send(local_target);
  pump();
  check(requester.read_file("local") == Bytes({7}),
        "local resume did not release target after remote order ended");
}

void proxy_options_reach_beneficiary() {
  Entity requester(Config{1}), respondent(Config{2});
  respondent.write_file("source", {9, 8, 7});
  ProxyOptions options;
  options.mode = PDU_Transmission_Mode::Unacknowledged;
  options.closure_requested = true;
  options.user_messages = {{'h', 'i'}};
  options.filestore_requests = {
      filestore_request(Filestore_Action::CreateFile, "created")};
  options.flow_label = {0xa5};
  options.fault_handler_overrides = {fault_handler_override(
      Condition_Code::FileChecksumFailure, Fault_Handler::Cancel)};
  const auto request = requester.proxy_put(2, 1, "source", "fetched", options);
  bool forwarded_mode = false;
  for (int i = 0; i < 16; ++i) {
    for (const auto &wire : requester.take_outgoing())
      respondent.receive(wire);
    for (const auto &wire : respondent.take_outgoing()) {
      const auto pdu = CFDP_PDU::from_bytes(wire);
      if (const auto *metadata = std::get_if<Metadata_PDU>(&pdu.body);
          metadata && metadata->destinationFileName == "fetched") {
        forwarded_mode = pdu.header.transmissionMode ==
                             PDU_Transmission_Mode::Unacknowledged &&
                         metadata->closureRequested;
      }
      requester.receive(wire);
    }
  }
  check(requester.sender_result(request) == Condition_Code::NoError &&
            forwarded_mode &&
            requester.read_file("fetched") == Bytes({9, 8, 7}) &&
            requester.read_file("created") == Bytes{},
        "proxy options did not reach beneficiary");
  bool message = false, flow = false, fault_override = false;
  for (const auto &transfer : requester.take_completed()) {
    if (transfer.destination_name != "fetched")
      continue;
    for (const auto &option : transfer.metadata_options) {
      message |= option.type == TLV_Type::MessageToUser &&
                 option.value == Bytes({'h', 'i'});
      flow |=
          option.type == TLV_Type::FlowLabel && option.value == Bytes({0xa5});
      fault_override |=
          option.type == TLV_Type::FaultHandlerOverrides &&
          option.value == options.fault_handler_overrides[0].value;
    }
  }
  check(message && flow && fault_override,
        "proxy user message, flow label, or fault override was not forwarded");
}

void proxy_cancel_request() {
  Entity requester(Config{1}), respondent(Config{2});
  respondent.write_file("source", Bytes(128, 7));
  const auto origin = requester.proxy_put(2, 1, "source", "cancelled");
  for (const auto &wire : requester.take_outgoing())
    respondent.receive(wire);
  const auto cancel = requester.cancel_proxy(2, origin);
  for (const auto &wire : requester.take_outgoing())
    respondent.receive(wire);
  for (int i = 0; i < 16; ++i) {
    for (const auto &wire : respondent.take_outgoing())
      requester.receive(wire);
    for (const auto &wire : requester.take_outgoing())
      respondent.receive(wire);
  }
  check(requester.sender_result(cancel) == Condition_Code::NoError &&
            respondent.sender_result(1) == Condition_Code::CancelRequested &&
            !requester.read_file("cancelled"),
        "proxy cancellation did not stop the respondent transfer");
}

void three_party_proxy_transfer() {
  Entity originator(Config{
      .entity_id = 1,
      .default_peer = PeerConfig{.segment_size = 256, .ut_address = {}}}),
      respondent(Config{.entity_id = 2,
                        .default_peer =
                            PeerConfig{.segment_size = 256, .ut_address = {}}}),
      beneficiary(Config{
          .entity_id = 3,
          .default_peer = PeerConfig{.segment_size = 256, .ut_address = {}}});
  Bytes content(50000);
  for (size_t i = 0; i < content.size(); ++i)
    content[i] = static_cast<uint8_t>((i * 11) & 255);
  respondent.write_file("source", content);
  const auto request = originator.proxy_put(2, 3, "source", "delivered");
  for (int i = 0; i < 20; ++i) {
    for (const auto &wire : originator.take_outgoing())
      respondent.receive(wire);
    for (const auto &wire : respondent.take_outgoing()) {
      const auto pdu = CFDP_PDU::from_bytes(wire);
      const uint64_t peer =
          pdu.header.direction == PDU_Direction::ToFileReceiver
              ? pdu.header.destinationEntityID
              : pdu.header.sourceEntityID;
      if (peer == 1)
        originator.receive(wire);
      else if (peer == 3)
        beneficiary.receive(wire);
      else
        throw std::runtime_error("three-party proxy routed to an unknown peer");
    }
    for (const auto &wire : beneficiary.take_outgoing())
      respondent.receive(wire);
  }
  bool reported = false;
  for (const auto &transfer : originator.take_completed())
    for (const auto &option : transfer.metadata_options)
      reported |= option.type == TLV_Type::MessageToUser &&
                  option.value.size() == 6 && option.value[4] == 0x07 &&
                  option.value[5] == 2;
  check(originator.sender_result(request) == Condition_Code::NoError &&
            beneficiary.read_file("delivered") == content && reported,
        "three-party proxy transfer or report failed");
}

void configured_sequence_seed() {
  Config config{1};
  config.initial_sequence = 37;
  Entity source(config);
  check(source.put(2, "seeded", {1}) == 37 && source.next_sequence() == 38,
        "configured sequence seed was ignored");
  config.initial_sequence = 0;
  rejects([&] { Entity invalid(config); }, "zero sequence seed was accepted");
  config.initial_sequence = 256;
  rejects([&] { Entity invalid(config); },
          "oversize sequence seed was accepted");
}

void mib_defaults_and_indications() {
  Config sender_config{1};
  PeerConfig peer;
  peer.default_mode = PDU_Transmission_Mode::Unacknowledged;
  peer.default_closure_requested = true;
  peer.default_checksum_type = 2;
  peer.crc = true;
  peer.ut_address = "127.0.0.1:5002";
  sender_config.peers[2] = peer;
  sender_config.indications.eof_sent = false;
  Entity sender(sender_config);
  check(sender.peer_settings(2).ut_address == peer.ut_address,
        "remote UT address was not exposed");
  const auto sequence = sender.put(2, "mib", {1, 2, 3});
  auto packets = sender.take_outgoing();
  Entity overrides(sender_config);
  PutOptions explicit_options;
  explicit_options.checksum_type = 0;
  explicit_options.closure_requested = false;
  overrides.put(2, "override", {1}, PDU_Transmission_Mode::Unacknowledged,
                explicit_options);
  const auto explicit_metadata = CFDP_PDU::from_bytes(overrides.take_outgoing().front());
  check(std::get<Metadata_PDU>(explicit_metadata.body).checksumType == 0 &&
            !std::get<Metadata_PDU>(explicit_metadata.body).closureRequested,
        "explicit zero checksum or false closure did not override peer defaults");
  overrides.put(2, "mode", {1}, PDU_Transmission_Mode::Acknowledged);
  check(CFDP_PDU::from_bytes(overrides.take_outgoing().front())
                .header.transmissionMode == PDU_Transmission_Mode::Acknowledged,
        "explicit transmission mode did not override peer default");
  const auto metadata = CFDP_PDU::from_bytes(packets.front());
  check(metadata.header.transmissionMode ==
                PDU_Transmission_Mode::Unacknowledged &&
            metadata.header.crcFlag == PDU_CRC_Flag::Present &&
            std::get<Metadata_PDU>(metadata.body).closureRequested &&
            std::get<Metadata_PDU>(metadata.body).checksumType == 2,
        "remote MIB defaults were not applied");
  for (const auto &event : sender.take_events())
    check(event.type != Event_Type::EOFSent,
          "disabled EOF-Sent indication was returned");
  Config receiver_config{2};
  receiver_config.indications.file_segment_received = false;
  receiver_config.indications.eof_received = false;
  receiver_config.indications.receiver_finished = false;
  Entity receiver(receiver_config);
  for (const auto &wire : packets)
    receiver.receive(wire);
  for (const auto &wire : receiver.take_outgoing())
    sender.receive(wire);
  check(sender.sender_result(sequence) == Condition_Code::NoError &&
            receiver.take_completed().front().data == Bytes({1, 2, 3}),
        "MIB default transfer failed");
  for (const auto &event : receiver.take_events())
    check(event.type != Event_Type::FileSegmentReceived &&
              event.type != Event_Type::EOFReceived &&
              event.type != Event_Type::TransactionFinished,
          "disabled receiver indication was returned");
}

void proxy_preserved_records() {
  Entity requester(Config{1}), respondent(Config{2});
  respondent.write_file("structured", Bytes(12, 0x42), {3, 9});
  ProxyOptions options;
  options.preserve_record_boundaries = true;
  requester.proxy_put(2, 1, "structured", "result", options);
  size_t preserved = 0;
  for (int i = 0; i < 14; ++i) {
    for (const auto &wire : requester.take_outgoing())
      respondent.receive(wire);
    for (const auto &wire : respondent.take_outgoing()) {
      const auto pdu = CFDP_PDU::from_bytes(wire);
      if (const auto *data = std::get_if<FileData_PDU>(&pdu.body);
          data &&
          pdu.header.segmentationControl ==
              PDU_Segmentation_Control::BoundariesPreserved &&
          data->recordContinuationState == Record_Continuation_State::Complete)
        ++preserved;
      requester.receive(wire);
    }
  }
  check(preserved == 2 && requester.read_file("result") == Bytes(12, 0x42),
        "proxy record boundaries were not preserved");

  respondent.write_file("unstructured", {1, 2, 3});
  requester.proxy_put(2, 1, "unstructured", "missing-layout", options);
  for (int i = 0; i < 12; ++i) {
    for (const auto &wire : requester.take_outgoing())
      respondent.receive(wire);
    for (const auto &wire : respondent.take_outgoing())
      requester.receive(wire);
  }
  bool failure_report = false;
  for (const auto &transfer : requester.take_completed())
    for (const auto &option : transfer.metadata_options)
      failure_report |=
          option.type == TLV_Type::MessageToUser && option.value.size() == 6 &&
          option.value[4] == 0x07 &&
          (option.value[5] >> 4) ==
              static_cast<uint8_t>(Condition_Code::InvalidFileStructure);
  check(failure_report && !requester.read_file("missing-layout"),
        "proxy missing record layout was not reported");
}
} // namespace

int main() {
  try {
    class_one_check_timers();
    per_peer_and_link_opportunity();
    independent_timers_and_capacity();
    malformed_metadata_is_atomic();
    queue_limit_is_atomic();
    finished_waits_for_queue_space();
    failed_finished_waits_for_queue_space();
    cancelled_finished_waits_for_queue_space();
    failed_completion_does_not_change_filestore();
    class_one_waits_for_eof_issue();
    finished_ack_limit_is_observable();
    keep_alive_fault();
    reject_inconsistent_reverse_pdus();
    generated_record_segments();
    optional_file_checksums();
    failed_transport_send_is_retained();
    remote_status_report();
    remote_suspension_orders();
    proxy_options_reach_beneficiary();
    proxy_cancel_request();
    three_party_proxy_transfer();
    configured_sequence_seed();
    mib_defaults_and_indications();
    proxy_preserved_records();
    std::cout << "CFDP completion tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
