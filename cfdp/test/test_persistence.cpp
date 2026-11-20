#include "entity.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace cfdp;

int main() {
  try {
    std::string pattern = "/tmp/cfdp-filestore-test-XXXXXX";
    if (!mkdtemp(pattern.data()))
      throw std::runtime_error("cannot create test directory");
    const std::filesystem::path directory(pattern);
    const auto snapshot = directory / "filestore.state";
    try {
      // Success must wait for storage, and a storage exception must reach the
      // sender as failure without publishing a completed file.
      for (const bool reject : {false, true}) {
        Entity sender(Config{1});
        Config config{2};
        bool committed = false;
        config.commit_received = [&](const Transfer &transfer,
                                     const Filestore &proposed) {
          if (transfer.data != Bytes({4, 5}) ||
              proposed.read("stored") != transfer.data)
            throw std::logic_error("storage callback received wrong file");
          if (reject)
            throw std::runtime_error("simulated disk failure");
          proposed.save(snapshot);
          committed = true;
        };
        Entity destination(config);
        const auto sequence = sender.put(2, "stored", {4, 5});
        for (const auto &wire : sender.take_outgoing())
          destination.receive(wire);
        if (!reject) {
          Entity disk(Config{2});
          disk.load_filestore(snapshot);
          if (!committed || disk.read_file("stored") != Bytes({4, 5}))
            throw std::runtime_error("success preceded persistent storage");
        }
        for (int i = 0; i < 4; ++i) {
          for (const auto &wire : destination.take_outgoing())
            sender.receive(wire);
          for (const auto &wire : sender.take_outgoing())
            destination.receive(wire);
        }
        const auto expected = reject ? Condition_Code::FilestoreRejection
                                     : Condition_Code::NoError;
        if (!sender.sender_complete(sequence) ||
            sender.sender_result(sequence) != expected ||
            destination.receiver_result(1, sequence) != expected)
          throw std::runtime_error("storage result did not reach sender");
        if (reject && (destination.read_file("stored") ||
                       !destination.take_completed().empty()))
          throw std::runtime_error("failed storage published a file");
      }
      Entity source(Config{1}), receiver(Config{2});
      receiver.write_file("records", {1, 2, 3, 4}, {2, 2});
      PutOptions options;
      options.metadata_options.push_back(
          filestore_request(Filestore_Action::CreateDirectory, "folder"));
      source.put(2, "folder/item", {7, 8}, PDU_Transmission_Mode::Acknowledged,
                 options);
      for (const auto &wire : source.take_outgoing())
        receiver.receive(wire);
      receiver.save_filestore(snapshot);

      Entity restored(Config{2});
      restored.load_filestore(snapshot);
      if (restored.read_file("records") != Bytes({1, 2, 3, 4}) ||
          restored.read_file("folder/item") != Bytes({7, 8}))
        throw std::runtime_error("filestore files did not survive restart");
      Entity requester(Config{1});
      ProxyOptions proxy;
      proxy.preserve_record_boundaries = true;
      requester.proxy_put(2, 1, "records", "copied", proxy);
      for (int i = 0; i < 14; ++i) {
        for (const auto &wire : requester.take_outgoing())
          restored.receive(wire);
        for (const auto &wire : restored.take_outgoing())
          requester.receive(wire);
      }
      if (requester.read_file("copied") != Bytes({1, 2, 3, 4}))
        throw std::runtime_error("persisted record layout was not usable");

      std::ofstream(snapshot, std::ios::binary | std::ios::trunc) << "broken";
      Entity intact(Config{2});
      intact.write_file("sentinel", {9});
      bool rejected = false;
      try {
        intact.load_filestore(snapshot);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      if (!rejected || intact.read_file("sentinel") != Bytes({9}))
        throw std::runtime_error("corrupt snapshot changed live filestore");
    } catch (...) {
      std::filesystem::remove_all(directory);
      throw;
    }
    std::filesystem::remove_all(directory);
    std::cout << "CFDP persistence tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
