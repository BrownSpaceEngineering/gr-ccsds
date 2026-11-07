#include "entity.hpp"
#include "udp_receiver.hpp"
#include "wire_constants.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace cfdp;
constexpr int udp_poll_timeout_ms = 20;
constexpr int sequence_decimal_digits =
    std::numeric_limits<uint64_t>::digits10 + 1;
constexpr auto send_timeout = std::chrono::seconds(60);
constexpr auto receive_timeout = std::chrono::seconds(120);
constexpr auto completed_receive_retention = std::chrono::seconds(30);
constexpr auto protocol_tick = std::chrono::seconds(1);
constexpr unsigned max_linger_seconds = 10;

uint16_t port(const char *arg) {
  const auto value = std::stoul(arg);
  if (!value || value > std::numeric_limits<uint16_t>::max())
    throw std::invalid_argument("invalid UDP port");
  return static_cast<uint16_t>(value);
}

class SequenceFile {
public:
  explicit SequenceFile(const char *path) {
    fd_ = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd_ < 0)
      throw std::runtime_error("cannot open sequence state file");
    if (flock(fd_, LOCK_EX) < 0) {
      close(fd_);
      throw std::runtime_error("cannot lock sequence state file");
    }
  }
  SequenceFile(const SequenceFile &) = delete;
  SequenceFile &operator=(const SequenceFile &) = delete;
  ~SequenceFile() { close(fd_); }

  uint64_t next() const {
    char digits[sequence_decimal_digits + 1]{};
    const auto size = pread(fd_, digits, sizeof(digits), 0);
    if (size == 0)
      return 1;
    if (size != sequence_decimal_digits)
      throw std::runtime_error("invalid sequence state file");
    for (int i = 0; i < sequence_decimal_digits; ++i)
      if (digits[i] < '0' || digits[i] > '9')
        throw std::runtime_error("invalid sequence state file");
    return std::stoull(std::string(digits, sequence_decimal_digits));
  }

  void reserve_next(uint64_t next) {
    std::ostringstream text;
    text << std::setfill('0') << std::setw(sequence_decimal_digits) << next;
    const auto value = text.str();
    if (pwrite(fd_, value.data(), value.size(), 0) !=
            static_cast<ssize_t>(value.size()) ||
        fsync(fd_) < 0)
      throw std::runtime_error("cannot persist sequence state");
  }

private:
  int fd_;
};

int socket_on(uint16_t local_port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    throw std::runtime_error("cannot create UDP socket");
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(local_port);
  if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
    close(fd);
    throw std::runtime_error("cannot bind UDP socket");
  }
  return fd;
}

void flush(int fd, Entity &entity, const sockaddr_in &peer) {
  entity.send_outgoing([&](uint64_t, const Bytes &pdu) {
    const auto sent =
        sendto(fd, pdu.data(), pdu.size(), 0,
               reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
    return sent == static_cast<ssize_t>(pdu.size());
  });
}

bool wait_packet(UdpReceiver &receiver, Entity &entity, sockaddr_in &peer,
                 const sockaddr_in *expected_peer = nullptr,
                 uint64_t *source_id = nullptr) {
  entity.poll_storage();
  auto incoming_packet = receiver.receive(std::chrono::milliseconds(udp_poll_timeout_ms));
  if (const auto drops = receiver.take_drop_count())
    std::cerr << "UDP receive queue dropped " << drops << " packets\n";
  if (!incoming_packet)
    return false;
  auto &packet = incoming_packet->bytes;
  const auto incoming = incoming_packet->peer;
  if (expected_peer &&
      (incoming.sin_addr.s_addr != expected_peer->sin_addr.s_addr ||
       incoming.sin_port != expected_peer->sin_port))
    return false;
  try {
    const auto header = CFDP_PDU::from_bytes(packet).header;
    entity.receive(packet);
    if (source_id)
      *source_id = header.direction == PDU_Direction::ToFileSender
                       ? header.destinationEntityID : header.sourceEntityID;
  } catch (const std::invalid_argument &) {
    return false;
  } catch (const std::length_error &) {
    return false;
  }
  peer = incoming;
  return true;
}

void save(const std::filesystem::path &directory, const Transfer &transfer) {
  const std::filesystem::path name(transfer.destination_name);
  if (name.empty() || name.is_absolute() || name.filename() != name ||
      name == "." || name == ".." ||
      transfer.destination_name.find('\0') != std::string::npos)
    throw std::invalid_argument("destination must be a simple file name");
  const auto temporary =
      directory / (".cfdp-" + std::to_string(getpid()) + "-" +
                   std::to_string(transfer.source_id) + "-" +
                   std::to_string(transfer.sequence) + ".part");
  const int fd =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create temporary destination file");
  bool closed = false;
  try {
    size_t offset = 0;
    while (offset < transfer.data.size()) {
      const auto written = write(fd, transfer.data.data() + offset,
                                 transfer.data.size() - offset);
      if (written <= 0)
        throw std::runtime_error("cannot write destination file");
      offset += static_cast<size_t>(written);
    }
    if (fsync(fd) < 0)
      throw std::runtime_error("cannot sync destination file");
    if (close(fd) < 0)
      throw std::runtime_error("cannot close destination file");
    closed = true;
    std::filesystem::rename(temporary, directory / name);
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0)
      throw std::runtime_error("cannot open destination directory for sync");
    const int synced = fsync(directory_fd);
    close(directory_fd);
    if (synced < 0)
      throw std::runtime_error("cannot sync destination directory");
  } catch (...) {
    if (!closed)
      close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  std::cout << "Received " << name << " (" << transfer.data.size()
            << " bytes)\n";
}

FilestoreConfig receiver_filestore(int argc, char **argv,
                                   const std::filesystem::path &directory) {
  if (argc == 5)
    return {};
  if (std::string(argv[5]) != "--filestore")
    throw std::invalid_argument("expected --filestore memory|disk");
  const std::string mode(argv[6]);
  if (mode == "memory")
    return {};
  if (mode == "disk")
    return {Filestore_Mode::Disk, directory / ".cfdp-store"};
  throw std::invalid_argument("filestore must be memory or disk");
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc >= 9 && argc <= 15 && std::string(argv[1]) == "send") {
      const auto local_port = port(argv[2]);
      sockaddr_in peer{};
      peer.sin_family = AF_INET;
      peer.sin_port = htons(port(argv[4]));
      if (inet_pton(AF_INET, argv[3], &peer.sin_addr) != 1)
        throw std::invalid_argument("destination must be an IPv4 address");
      std::ifstream file(argv[5], std::ios::binary);
      if (!file)
        throw std::runtime_error("cannot open source file");
      if (std::filesystem::file_size(argv[5]) > Config{}.max_file_size)
        throw std::length_error("source file exceeds 64 MiB example limit");
      Bytes data((std::istreambuf_iterator<char>(file)),
                 std::istreambuf_iterator<char>());
      if (file.bad())
        throw std::runtime_error("cannot read source file");
      bool unack = false;
      bool crc = false;
      unsigned linger_seconds = 0;
      const char *state_path = nullptr;
      for (int i = 9; i < argc; ++i) {
        if (std::string(argv[i]) == "unack" && !unack)
          unack = true;
        else if (std::string(argv[i]) == "--crc" && !crc)
          crc = true;
        else if (std::string(argv[i]) == "--state" && !state_path &&
                 i + 1 < argc)
          state_path = argv[++i];
        else if (std::string(argv[i]) == "--linger" && !linger_seconds &&
                 i + 1 < argc) {
          linger_seconds = std::stoul(argv[++i]);
          if (linger_seconds == 0 || linger_seconds > max_linger_seconds)
            throw std::invalid_argument("linger must be 1 to 10 seconds");
        } else
          throw std::invalid_argument("invalid send option");
      }
      const auto mode = unack ? PDU_Transmission_Mode::Unacknowledged
                              : PDU_Transmission_Mode::Acknowledged;
      Config config{std::stoull(argv[7])};
      config.default_peer.crc = crc;
      std::optional<SequenceFile> sequence_file;
      if (state_path) {
        sequence_file.emplace(state_path);
        config.initial_sequence = sequence_file->next();
      }
      Entity entity(config);
      const auto sequence =
          entity.put(std::stoull(argv[8]), argv[6], data, mode);
      if (sequence_file)
        sequence_file->reserve_next(entity.next_sequence());
      UdpReceiver receiver(socket_on(local_port));
      const int fd = receiver.socket();
      flush(fd, entity, peer);
      auto last_tick = std::chrono::steady_clock::now();
      const auto deadline = last_tick + send_timeout;
      while (!entity.sender_complete(sequence) &&
             std::chrono::steady_clock::now() < deadline) {
        sockaddr_in incoming{};
        wait_packet(receiver, entity, incoming, &peer);
        flush(fd, entity, peer);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= protocol_tick) {
          entity.tick();
          flush(fd, entity, peer);
          last_tick = now;
        }
      }
      if (!entity.sender_complete(sequence))
        throw std::runtime_error("transfer timed out");
      if (entity.sender_result(sequence) != Condition_Code::NoError)
        throw std::runtime_error("transfer failed with CFDP condition " +
                                 std::to_string(static_cast<unsigned>(
                                     *entity.sender_result(sequence))));
      const auto linger_until = std::chrono::steady_clock::now() +
                                std::chrono::seconds(linger_seconds);
      while (std::chrono::steady_clock::now() < linger_until) {
        sockaddr_in incoming{};
        wait_packet(receiver, entity, incoming, &peer);
        flush(fd, entity, peer);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= protocol_tick) {
          entity.tick();
          flush(fd, entity, peer);
          last_tick = now;
        }
      }
      std::cout << "Transfer complete\n";
      return 0;
    }
    if ((argc == 5 || argc == 7) && std::string(argv[1]) == "recv") {
      const auto local_port = port(argv[2]);
      const std::filesystem::path directory(argv[3]);
      if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("output directory does not exist");
      Config config{std::stoull(argv[4])};
      config.asynchronous_storage = true;
      config.filestore = receiver_filestore(argc, argv, directory);
      config.commit_received = [&](const Transfer &transfer, const Filestore &) {
        if (!transfer.destination_name.empty())
          save(directory, transfer);
      };
      Entity entity(std::move(config));
      UdpReceiver receiver(socket_on(local_port));
      const int fd = receiver.socket();
      auto last_tick = std::chrono::steady_clock::now();
      const auto deadline = last_tick + receive_timeout;
      sockaddr_in peer{};
      bool have_peer = false;
      bool saved = false;
      uint64_t source_id = 0, sequence = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        if (wait_packet(receiver, entity, peer, have_peer ? &peer : nullptr))
          have_peer = true;
        if (have_peer)
          flush(fd, entity, peer);
        for (const auto &transfer : entity.take_completed()) {
          if (transfer.destination_name.empty())
            continue;
          source_id = transfer.source_id;
          sequence = transfer.sequence;
          saved = true;
        }
        if (saved && entity.receiver_complete(source_id, sequence)) {
          if (entity.receiver_result(source_id, sequence) !=
              Condition_Code::NoError)
            throw std::runtime_error("received transfer failed");
          return 0;
        }
        for (const auto &event : entity.take_events())
          if (event.type == Event_Type::TransactionFinished &&
              event.condition != Condition_Code::NoError)
            throw std::runtime_error(
                "received transfer failed with CFDP condition " +
                std::to_string(static_cast<unsigned>(event.condition)));
        const auto now = std::chrono::steady_clock::now();
        if (have_peer && now - last_tick >= protocol_tick) {
          entity.tick();
          flush(fd, entity, peer);
          last_tick = now;
        }
      }
      throw std::runtime_error("receive timed out");
    }
    if ((argc == 5 || argc == 7) && std::string(argv[1]) == "serve") {
      const auto local_port = port(argv[2]);
      const std::filesystem::path directory(argv[3]);
      if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("output directory does not exist");
      const auto local_id = std::stoull(argv[4]);
      const auto filestore_state = directory / ".cfdp-filestore-state";
      Config config{local_id};
      config.asynchronous_storage = true;
      config.filestore = receiver_filestore(argc, argv, directory);
      const bool memory = config.filestore.mode == Filestore_Mode::Memory;
      config.commit_received = [&](const Transfer &transfer,
                                   const Filestore &filestore) {
        if (!transfer.destination_name.empty())
          save(directory, transfer);
        if (memory)
          filestore.save(filestore_state);
      };
      Entity entity(std::move(config));
      if (memory)
        entity.load_filestore(filestore_state);
      UdpReceiver receiver(socket_on(local_port));
      const int fd = receiver.socket();
      std::map<uint64_t, sockaddr_in> routes;
      std::map<std::pair<uint64_t, uint64_t>,
               std::chrono::steady_clock::time_point>
          saved;
      auto last_tick = std::chrono::steady_clock::now();
      while (true) {
        sockaddr_in incoming{};
        uint64_t source_id = 0;
        if (wait_packet(receiver, entity, incoming, nullptr, &source_id))
          routes[source_id] = incoming;
        entity.send_outgoing([&](uint64_t peer_id, const Bytes &pdu) {
          const auto it = routes.find(peer_id);
          if (it == routes.end())
            return false;
          const auto sent =
              sendto(fd, pdu.data(), pdu.size(), 0,
                     reinterpret_cast<const sockaddr *>(&it->second),
                     sizeof(it->second));
          return sent == static_cast<ssize_t>(pdu.size());
        });
        const auto transfers = entity.take_completed();
        for (const auto &transfer : transfers) {
          saved[{transfer.source_id, transfer.sequence}] =
              std::chrono::steady_clock::now();
        }
        for (const auto &event : entity.take_events()) {
          if ((event.type == Event_Type::TransactionFinished ||
               event.type == Event_Type::Abandoned) &&
              event.source_id != local_id &&
              entity.receiver_result(event.source_id, event.sequence))
            saved[{event.source_id, event.sequence}] =
                std::chrono::steady_clock::now();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= protocol_tick) {
          entity.advance(std::chrono::duration_cast<std::chrono::milliseconds>(
              now - last_tick));
          last_tick = now;
        }
        for (auto it = saved.begin(); it != saved.end();) {
          if (now - it->second >= completed_receive_retention &&
              entity.receiver_complete(it->first.first, it->first.second)) {
            entity.forget_receive(it->first.first, it->first.second);
            it = saved.erase(it);
          } else
            ++it;
        }
      }
    }
    std::cerr << "Usage:\n  cfdp send LOCAL_PORT PEER_IP PEER_PORT SOURCE_FILE "
                 "DEST_NAME SOURCE_ID DEST_ID [unack] [--crc] [--state FILE] "
                 "[--linger SECONDS]\n"
                 "  cfdp recv LOCAL_PORT OUTPUT_DIR ENTITY_ID "
                 "[--filestore memory|disk]\n"
                 "  cfdp serve LOCAL_PORT OUTPUT_DIR ENTITY_ID "
                 "[--filestore memory|disk]\n";
    return 2;
  } catch (const std::exception &e) {
    std::cerr << "CFDP: " << e.what() << '\n';
    return 1;
  }
}
