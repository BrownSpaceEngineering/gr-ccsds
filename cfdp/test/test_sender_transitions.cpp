#include "entity.hpp"

#include <iostream>
#include <stdexcept>

using namespace cfdp;

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

void fault_removes_stale_traffic(Fault_Handler handler) {
  Config config{1};
  config.default_peer.ack_retry_limit = 1;
  config.default_fault_handlers[Condition_Code::AckLimitReached] = handler;
  Entity source(config);
  const auto sequence = source.put(2, "first", {1});
  auto header = CFDP_PDU::from_bytes(source.take_outgoing().back()).header;
  header.direction = PDU_Direction::ToFileSender;
  source.tick(); // Leave an EOF retry queued while another transfer starts.
  const auto other = source.put(3, "other", {9});
  source.tick(); // The ACK limit fires without sending that retry.
  size_t cancellations = 0, other_pdus = 0;
  for (const auto &wire : source.take_outgoing()) {
    const auto pdu = CFDP_PDU::from_bytes(wire);
    if (pdu.header.transactionSequenceNumber == other) {
      ++other_pdus;
      continue;
    }
    const auto *eof = std::get_if<EOF_PDU>(&pdu.body);
    check(handler == Fault_Handler::Cancel && eof &&
              eof->condition == Condition_Code::AckLimitReached,
          "fault left stale traffic queued for its sender");
    ++cancellations;
  }
  check(other_pdus == 3, "sender transition removed another transaction");
  check(cancellations == (handler == Fault_Handler::Cancel ? 1U : 0U),
        "fault did not replace pending traffic with one cancellation");
  if (handler == Fault_Handler::Suspend) {
    check(source.report_send(sequence).suspended &&
              !source.sender_complete(sequence),
          "fault did not suspend the sender");
    source.receive(CFDP_PDU{header, NAK_PDU{0, 1, {{0, 1}}}}.to_bytes());
    check(source.take_outgoing().empty(), "NAK retransmitted a suspended sender");
    source.resume_send(sequence);
    check(source.take_outgoing().size() == 3,
          "fault-suspended sender could not resume its full transfer");
  } else {
    check(source.sender_result(sequence) == Condition_Code::AckLimitReached,
          "fault transition lost the sender result");
    if (handler == Fault_Handler::Abandon)
      source.forget_send(sequence);
  }
}

void cancellation_reclaims_queue_capacity() {
  Config config{1};
  config.max_queued_pdu_bytes = 83; // Exactly Metadata + 20 data bytes + EOF.
  Entity source(config);
  const auto sequence = source.put(2, "a", Bytes(20, 1));
  source.cancel_send(sequence);
  const auto packets = source.take_outgoing();
  check(packets.size() == 1, "cancel did not replace a full sender queue");
  const auto pdu = CFDP_PDU::from_bytes(packets.front());
  const auto *eof = std::get_if<EOF_PDU>(&pdu.body);
  check(eof && eof->condition == Condition_Code::CancelRequested,
        "cancel transmitted stale data or a successful EOF");
  source.put(3, "b", Bytes(20, 2));
  check(source.take_outgoing().size() == 3,
        "discarded sender traffic still consumed queue capacity");
}

void failed_cancellation_is_atomic() {
  Config config{1};
  config.max_queued_pdu_bytes = 100;
  Entity source(config);
  const auto sequence = source.put(2, "a", Bytes(20, 1));
  auto header = CFDP_PDU::from_bytes(source.take_outgoing().back()).header;
  header.direction = PDU_Direction::ToFileSender;
  const auto other = source.put(3, "b", Bytes(20, 2));
  source.take_events();
  bool rejected = false;
  try {
    source.cancel_send(sequence);
  } catch (const std::length_error &) {
    rejected = true;
  }
  check(rejected && source.take_events().empty(),
        "failed cancellation emitted a transition event");
  const auto report = source.report_send(sequence);
  check(!report.terminated && report.condition == Condition_Code::NoError,
        "failed cancellation changed the sender result");
  // Its original EOF must still accept a successful ACK.
  source.receive(CFDP_PDU{header, ACK_PDU{}}.to_bytes());
  const auto packets = source.take_outgoing();
  check(packets.size() == 3, "failed cancellation changed the queue");
  for (const auto &wire : packets)
    check(CFDP_PDU::from_bytes(wire).header.transactionSequenceNumber == other,
          "failed cancellation queued a partial replacement");
  source.cancel_send(sequence);
  check(source.take_outgoing().size() == 1,
        "cancellation could not retry after capacity recovered");
}

void cancellation_overrides_suspension() {
  Config config{1};
  config.default_peer.ack_retry_limit = 1;
  Entity source(config);
  const auto sequence = source.put(2, "a", {1});
  auto header = CFDP_PDU::from_bytes(source.take_outgoing().back()).header;
  header.direction = PDU_Direction::ToFileSender;
  source.suspend_send(sequence);
  source.cancel_send(sequence);
  check(!source.report_send(sequence).suspended,
        "suspension prevented the cancellation handshake");
  source.receive(CFDP_PDU{header, NAK_PDU{0, 1, {{0, 1}}}}.to_bytes());
  for (int i = 0; i < 4; ++i) {
    for (const auto &wire : source.take_outgoing()) {
      const auto pdu = CFDP_PDU::from_bytes(wire);
      const auto *eof = std::get_if<EOF_PDU>(&pdu.body);
      check(eof && eof->condition == Condition_Code::CancelRequested,
            "cancelling sender retransmitted file data");
    }
    source.tick();
  }
  check(source.sender_result(sequence) == Condition_Code::CancelRequested,
        "cancelled suspended sender did not finish");
  source.forget_send(sequence);
}

void finished_replaces_pending_retries() {
  Entity source(Config{1});
  const auto sequence = source.put(2, "a", {1});
  auto header = CFDP_PDU::from_bytes(source.take_outgoing().back()).header;
  header.direction = PDU_Direction::ToFileSender;
  source.tick();
  const auto other = source.put(3, "b", {2});
  source.take_events();
  const auto finished = CFDP_PDU{header, Finished_PDU{}}.to_bytes();
  source.receive(finished);
  source.receive(finished); // Keep the ACK without duplicating completion.
  const auto events = source.take_events();
  check(events.size() == 1 && events.front().type == Event_Type::TransactionFinished,
        "duplicate Finished emitted duplicate completion events");
  size_t acknowledgments = 0, other_pdus = 0;
  for (const auto &wire : source.take_outgoing()) {
    const auto pdu = CFDP_PDU::from_bytes(wire);
    if (pdu.header.transactionSequenceNumber == other) {
      ++other_pdus;
      continue;
    }
    const auto *ack = std::get_if<ACK_PDU>(&pdu.body);
    check(ack && ack->directive == Acknowledged_Directive::Finished,
          "completed sender transmitted an obsolete retry");
    ++acknowledgments;
  }
  check(acknowledgments == 1 && other_pdus == 3,
        "Finished replaced traffic from the wrong transaction");
  source.forget_send(sequence);
}

void opposite_direction_transactions_are_isolated() {
  Entity local(Config{1}), remote(Config{2});
  const auto local_sequence = local.put(2, "outbound", {1});
  const auto remote_sequence = remote.put(1, "inbound", {2});
  check(local_sequence == remote_sequence, "test needs matching sequence numbers");
  for (const auto &wire : remote.take_outgoing())
    local.receive(wire);
  local.suspend_send(local_sequence);
  const auto replies = local.take_outgoing();
  check(replies.size() == 2, "suspend removed receiver protocol replies");
  for (const auto &wire : replies) {
    check(CFDP_PDU::from_bytes(wire).header.sourceEntityID == 2,
          "suspended sender retained its outbound traffic");
    remote.receive(wire);
  }
  for (const auto &wire : remote.take_outgoing())
    local.receive(wire);
  check(local.receiver_complete(2, remote_sequence),
        "suspending a sender broke the opposite-direction transaction");
  local.resume_send(local_sequence);
  for (int i = 0; i < 4; ++i) {
    for (const auto &wire : local.take_outgoing())
      remote.receive(wire);
    for (const auto &wire : remote.take_outgoing())
      local.receive(wire);
  }
  check(local.sender_result(local_sequence) == Condition_Code::NoError &&
            remote.read_file("outbound") == Bytes({1}),
        "sender did not resume after an opposite-direction transfer");
}
} // namespace

int main() {
  try {
    fault_removes_stale_traffic(Fault_Handler::Suspend);
    fault_removes_stale_traffic(Fault_Handler::Cancel);
    fault_removes_stale_traffic(Fault_Handler::Abandon);
    cancellation_reclaims_queue_capacity();
    failed_cancellation_is_atomic();
    cancellation_overrides_suspension();
    finished_replaces_pending_retries();
    opposite_direction_transactions_are_isolated();
    std::cout << "CFDP sender transition tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
