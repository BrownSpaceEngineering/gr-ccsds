#include "entity.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace cfdp;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

Config disk_config(const std::filesystem::path &directory) {
  Config config{2};
  config.filestore = {Filestore_Mode::Disk, directory};
  return config;
}

void exchange(Entity &sender, Entity &receiver) {
  for (int i = 0; i < 12; ++i) {
    for (const auto &wire : sender.take_outgoing())
      receiver.receive(wire);
    for (const auto &wire : receiver.take_outgoing())
      sender.receive(wire);
  }
}

void operations_and_restart(const std::filesystem::path &directory) {
  const FilestoreConfig config{Filestore_Mode::Disk, directory};
  {
    Filestore memory, disk(config, 1024);
    for (auto *store : {&memory, &disk}) {
      store->write("source", {1, 2}, {1, 1});
      store->write("suffix", {3});
      store->commit();
    }
    const std::vector<TLV> requests{
        filestore_request(Filestore_Action::CreateDirectory, "folder"),
        filestore_request(Filestore_Action::CreateFile, "empty"),
        filestore_request(Filestore_Action::AppendFile, "source", "suffix"),
        filestore_request(Filestore_Action::ReplaceFile, "empty", "suffix"),
        filestore_request(Filestore_Action::RenameFile, "source", "renamed"),
        filestore_request(Filestore_Action::DeleteFile, "suffix"),
        filestore_request(Filestore_Action::DenyFile, "missing"),
        filestore_request(Filestore_Action::RemoveDirectory, "folder"),
        filestore_request(Filestore_Action::DenyDirectory, "absent"),
        // Failure skips the subsequent operation identically in both modes.
        filestore_request(Filestore_Action::DeleteFile, "missing"),
        filestore_request(Filestore_Action::CreateFile, "skipped")};
    const auto expected = memory.execute(requests);
    const auto actual = disk.execute(requests);
    check(expected.size() == actual.size(), "response counts differ");
    for (size_t i = 0; i < actual.size(); ++i)
      check(actual[i].type == expected[i].type &&
                actual[i].value == expected[i].value,
            "disk filestore request status differs from memory");
    disk.commit();
    check(disk.listing("") == memory.listing(""), "disk listing differs");
    for (const auto &name : {"renamed", "empty", "source", "suffix", "skipped"})
      check(disk.read(name) == memory.read(name), "disk file differs");
    disk.write("records", {8, 9, 10}, {1, 2});
    disk.commit();
    // A staged copy must leave the committed state intact when discarded.
    {
      Filestore abandoned(disk);
      abandoned.write("renamed", {99});
      abandoned.execute(
          {filestore_request(Filestore_Action::DeleteFile, "records")});
    }
    size_t objects = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
      if (entry.path().filename().string().starts_with("data-"))
        ++objects;
    check(objects == 3, "discarded or replaced disk objects were retained");
    bool locked = false;
    try {
      Filestore duplicate(config, 1024);
    } catch (const std::runtime_error &) {
      locked = true;
    }
    check(locked, "two entities opened the same disk filestore");
  }
  {
    Filestore restored(config, 1024);
    check(restored.read("renamed") == Bytes({1, 2, 3}) &&
              restored.read("empty") == Bytes({3}) &&
              !restored.read("suffix") && !restored.read("skipped"),
          "file operations did not survive restart");
    check(restored.records("records") == std::vector<size_t>({1, 2}),
          "record layout did not survive restart");
    check(!restored.listing("folder").first,
          "directory removal did not survive restart");
    // Disk mode can export a portable snapshot for memory mode.
    restored.save(directory.parent_path() / "export.state");
  }
  Filestore memory;
  memory.load(directory.parent_path() / "export.state", 1024);
  check(memory.read("records") == Bytes({8, 9, 10}),
        "disk-to-memory snapshot export failed");
  {
    const FilestoreConfig imported{Filestore_Mode::Disk,
                                   directory.parent_path() / "imported"};
    {
      Filestore disk(imported, 1024);
      disk.load(directory.parent_path() / "export.state", 1024);
    }
    Filestore restored(imported, 1024);
    check(restored.read("records") == Bytes({8, 9, 10}) &&
              restored.records("records") == std::vector<size_t>({1, 2}),
          "snapshot import into disk mode was not persistent");
  }
  // Reject a malformed catalog without destroying already stored payloads.
  const auto catalog = directory / "catalog";
  std::ifstream original(catalog, std::ios::binary);
  const std::string valid((std::istreambuf_iterator<char>(original)), {});
  original.close();
  std::ofstream(catalog, std::ios::app) << "invalid trailing data";
  bool rejected = false;
  try {
    Filestore corrupt(config, 1024);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "corrupt disk catalog was accepted");
  std::ofstream(catalog, std::ios::binary | std::ios::trunc) << valid;
  Filestore intact(config, 1024);
  check(intact.read("records") == Bytes({8, 9, 10}),
        "rejecting corrupt catalog destroyed stored data");
}

void receive_and_proxy(const std::filesystem::path &directory) {
  const auto config = disk_config(directory);
  {
    Entity source(Config{1}), destination(config);
    PutOptions options;
    options.record_lengths = {2, 2};
    options.metadata_options = {
        filestore_request(Filestore_Action::CreateFile, "created")};
    const auto sequence =
        source.put(2, "records", {1, 2, 3, 4},
                   PDU_Transmission_Mode::Acknowledged, options);
    exchange(source, destination);
    check(source.sender_result(sequence) == Condition_Code::NoError,
          "disk receive did not succeed");
    destination.take_completed();
    destination.take_events();
    destination.forget_receive(1, sequence);
  }
  {
    Entity restored(config), requester(Config{1});
    check(restored.read_file("records") == Bytes({1, 2, 3, 4}) &&
              restored.read_file("created") == Bytes{},
          "receive success preceded durable storage");
    ProxyOptions options;
    options.preserve_record_boundaries = true;
    requester.proxy_put(2, 1, "records", "copy", options);
    exchange(requester, restored);
    check(requester.read_file("copy") == Bytes({1, 2, 3, 4}),
          "proxy could not read disk-backed file");
    bool preserved = false;
    for (const auto &transfer : requester.take_completed())
      if (transfer.destination_name == "copy")
        preserved = transfer.record_lengths == std::vector<size_t>({2, 2});
    check(preserved, "proxy lost persisted record boundaries");
  }
}

void failed_commit(const std::filesystem::path &directory) {
  const auto config = disk_config(directory);
  {
    Entity destination(config);
    destination.write_file("target", {9});
    // Block catalog replacement to simulate a disk commit failure after data
    // has been staged, without relying on permissions or available disk space.
    std::filesystem::rename(directory / "catalog", directory / "backup");
    std::filesystem::create_directory(directory / "catalog");
    Entity source(Config{1});
    const auto sequence = source.put(2, "target", {1, 2});
    exchange(source, destination);
    check(source.sender_result(sequence) == Condition_Code::FilestoreRejection,
          "disk commit failure was acknowledged as success");
    check(destination.read_file("target") == Bytes({9}) &&
              destination.take_completed().empty(),
          "failed disk commit changed live filestore");
    bool reported = false;
    for (const auto &event : destination.take_events())
      if (event.type == Event_Type::TransactionFinished)
        reported = event.delivery == Delivery_Code::Complete &&
                   event.file_status == File_Status::DiscardedByFilestore;
    check(reported,
          "storage failure did not report filestore rejection status");
    std::filesystem::remove(directory / "catalog");
    std::filesystem::rename(directory / "backup", directory / "catalog");
  }
  Entity restored(config);
  check(restored.read_file("target") == Bytes({9}),
        "failed disk commit changed persisted filestore");
}
} // namespace

int main() {
  std::string pattern = "/tmp/cfdp-disk-test-XXXXXX";
  if (!mkdtemp(pattern.data()))
    return 1;
  const std::filesystem::path directory(pattern);
  try {
    const auto occupied = directory / "occupied";
    std::filesystem::create_directory(occupied);
    std::ofstream(occupied / "data-ABC123") << "unrelated file";
    bool rejected = false;
    try {
      Entity unsafe(disk_config(occupied));
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected && std::filesystem::exists(occupied / "data-ABC123"),
          "disk mode adopted or cleaned an unrelated directory");
    operations_and_restart(directory / "operations");
    receive_and_proxy(directory / "receive");
    failed_commit(directory / "failure");
    std::filesystem::remove_all(directory);
    std::cout << "CFDP disk filestore tests passed\n";
  } catch (const std::exception &error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
