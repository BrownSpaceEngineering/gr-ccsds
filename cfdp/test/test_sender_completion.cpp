#include "entity.hpp"

#include <iostream>
#include <stdexcept>

using namespace cfdp;

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

void deliver(Entity &source, Entity &destination) {
  for (const auto &wire : source.take_outgoing())
    destination.receive(wire);
}

void check_response(Entity &originator, Condition_Code condition) {
  size_t responses = 0;
  for (const auto &transfer : originator.take_completed())
    for (const auto &option : transfer.metadata_options)
      if (option.type == TLV_Type::MessageToUser && option.value.size() == 6 &&
          option.value[4] == 0x07) {
        check((option.value[5] >> 4) == static_cast<uint8_t>(condition),
              "proxy response reported the wrong completion condition");
        ++responses;
      }
  check(responses == 1, "proxy completion did not produce exactly one response");
}

void unacknowledged_proxy_without_closure() {
  Entity originator(Config{1}), respondent(Config{2}), beneficiary(Config{3});
  respondent.write_file("source", {1, 2, 3});
  ProxyOptions options;
  options.mode = PDU_Transmission_Mode::Unacknowledged;
  options.closure_requested = false;
  originator.proxy_put(2, 3, "source", "target", options);
  for (int i = 0; i < 8; ++i) {
    deliver(originator, respondent);
    respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
      (peer == 1 ? originator : beneficiary).receive(wire);
      return true;
    });
    deliver(beneficiary, respondent);
  }
  check(beneficiary.read_file("target") == Bytes({1, 2, 3}),
        "unacknowledged proxy did not deliver the file");
  check_response(originator, Condition_Code::NoError);
}

void faulted_proxy(Fault_Handler handler) {
  Config config{2};
  config.default_peer.ack_retry_limit = 1;
  config.default_fault_handlers[Condition_Code::AckLimitReached] = handler;
  Entity originator(Config{1}), respondent(config);
  respondent.write_file("source", {4, 5});
  originator.proxy_put(2, 3, "source", "target");
  for (int i = 0; i < 8; ++i) {
    deliver(originator, respondent);
    respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
      // The beneficiary is unreachable; all its PDUs are lost.
      if (peer == 1)
        originator.receive(wire);
      return true;
    });
    respondent.tick();
  }
  check(respondent.sender_result(1) == Condition_Code::AckLimitReached,
        "proxy sender did not reach its fault completion");
  check_response(originator, Condition_Code::AckLimitReached);
}

void canceled_proxy(PDU_Transmission_Mode mode) {
  Config config{2};
  config.default_peer.ack_retry_limit = 1;
  Entity originator(Config{1}), respondent(config);
  respondent.write_file("source", {6});
  ProxyOptions options;
  options.mode = mode;
  originator.proxy_put(2, 3, "source", "target", options);
  deliver(originator, respondent);
  respondent.cancel_send(1);
  for (int i = 0; i < 8; ++i) {
    respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
      if (peer == 1)
        originator.receive(wire);
      return true;
    });
    deliver(originator, respondent);
    respondent.tick();
  }
  check(respondent.sender_result(1) == Condition_Code::CancelRequested,
        "proxy sender did not retain its cancellation result");
  check_response(originator, Condition_Code::CancelRequested);
}

void proxy_response_waits_for_capacity(bool transaction_limit) {
  Config config{2};
  if (transaction_limit)
    config.max_active_transactions = 1;
  else
    config.max_queued_pdu_bytes = 160;
  Entity originator(Config{1}), respondent(config), beneficiary(Config{3});
  respondent.write_file("source", {1, 2, 3});
  originator.proxy_put(2, 3, "source", "target");
  deliver(originator, respondent);
  respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
    (peer == 1 ? originator : beneficiary).receive(wire);
    return true;
  });
  deliver(originator, respondent);
  Bytes finished;
  for (const auto &wire : beneficiary.take_outgoing()) {
    if (std::holds_alternative<Finished_PDU>(CFDP_PDU::from_bytes(wire).body))
      finished = wire;
    respondent.receive(wire);
  }
  check(respondent.sender_complete(1), "proxy sender did not complete");

  // Hold another transaction in the queue so the response cannot be queued.
  respondent.put(9, "b", Bytes(60, 7), PDU_Transmission_Mode::Unacknowledged);
  respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
    if (peer == 9)
      return false;
    (peer == 1 ? originator : beneficiary).receive(wire);
    return true;
  });
  check(originator.take_completed().empty(),
        "response bypassed the configured capacity limit");
  bool retained = false;
  try {
    respondent.forget_send(1);
  } catch (const std::logic_error &) {
    retained = true;
  }
  check(retained, "forget discarded a pending proxy response");

  // Draining the blocker frees both transaction and byte capacity.
  respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
    if (peer != 9)
      (peer == 1 ? originator : beneficiary).receive(wire);
    return true;
  });
  check_response(originator, Condition_Code::NoError);
  // Duplicate Finished must be ACKed without generating another response.
  check(!finished.empty(), "beneficiary did not send Finished");
  respondent.receive(finished);
  for (int i = 0; i < 4; ++i) {
    deliver(originator, respondent);
    respondent.send_outgoing([&](uint64_t peer, const Bytes &wire) {
      (peer == 1 ? originator : beneficiary).receive(wire);
      return true;
    });
  }
  check(originator.take_completed().empty(), "duplicate proxy response");
  respondent.forget_send(1);
}
} // namespace

int main() {
  try {
    unacknowledged_proxy_without_closure();
    faulted_proxy(Fault_Handler::Cancel);
    faulted_proxy(Fault_Handler::Abandon);
    canceled_proxy(PDU_Transmission_Mode::Acknowledged);
    canceled_proxy(PDU_Transmission_Mode::Unacknowledged);
    proxy_response_waits_for_capacity(false);
    proxy_response_waits_for_capacity(true);
    std::cout << "CFDP sender completion tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
