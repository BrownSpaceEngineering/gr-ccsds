#pragma once

#include "filestore.hpp"
#include "user_operations.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cfdp {

struct Transfer {
  uint64_t source_id = 0;
  uint64_t sequence = 0;
  std::string destination_name;
  Bytes data;
  std::string source_name;
  std::vector<TLV> metadata_options;
  std::vector<TLV> filestore_responses;
  std::vector<size_t> record_lengths;
};

struct PutOptions {
  std::string source_name;
  std::optional<bool> closure_requested;
  bool unbounded = false;
  std::optional<uint8_t> checksum_type;
  // Nonempty record lengths request preserved record boundaries.
  std::vector<size_t> record_lengths;
  // Split records after each occurrence of this byte, including it in the
  // record.
  std::optional<uint8_t> record_delimiter;
  // Application-defined metadata copied into every File Data PDU (at most 63
  // octets).
  Bytes segment_metadata;
  std::vector<TLV> metadata_options;
};

struct ProxyOptions {
  std::optional<PDU_Transmission_Mode> mode;
  std::optional<bool> closure_requested;
  std::vector<Bytes> user_messages;
  std::vector<TLV> filestore_requests;
  std::vector<TLV> fault_handler_overrides;
  Bytes flow_label;
  std::optional<bool> preserve_record_boundaries;
};

enum class Nak_Mode { Deferred, Immediate, Prompted, Asynchronous };

struct IndicationConfig {
  bool eof_sent = true;
  bool eof_received = true;
  bool file_segment_received = true;
  bool receiver_finished = true;
  bool receiver_suspended = true;
  bool receiver_resumed = true;
};

// A complete set of settings for one remote entity.
// TODO: Need to update these to be reasonable values for Space
struct PeerConfig {
  size_t segment_size = 1024;
  bool crc = false;
  unsigned ack_interval_ticks = 1;
  unsigned ack_retry_limit = 5;
  unsigned nak_interval_ticks = 1;
  unsigned nak_retry_limit = 5;
  unsigned check_interval_ticks = 1;
  unsigned check_limit = 5;
  unsigned inactivity_limit_ticks = 30;
  unsigned keep_alive_interval_ticks = 0;
  unsigned keep_alive_limit = 0; // Zero disables the progress limit.
  Nak_Mode nak_mode = Nak_Mode::Deferred;
  uint8_t protocol_version = 1;
  std::string ut_address;
  PDU_Transmission_Mode default_mode = PDU_Transmission_Mode::Acknowledged;
  bool default_closure_requested = false;
  uint8_t default_checksum_type = 0;
};

struct Config {
  uint64_t entity_id = 0;
  uint8_t id_width = 1;
  uint8_t sequence_width = 1;
  size_t max_file_size = 64 * 1024 * 1024;
  PeerConfig default_peer = {};
  std::map<Condition_Code, Fault_Handler> default_fault_handlers = {};
  std::map<uint64_t, PeerConfig> peers = {};
  size_t max_active_transactions = 16;
  size_t max_active_data_bytes = 128 * 1024 * 1024;
  size_t max_queued_pdu_bytes = 128 * 1024 * 1024;
  uint64_t initial_sequence = 1;
  IndicationConfig indications = {};
  // Called before receive success, with the proposed filestore after requests.
  // Persist required storage here; throwing rejects delivery. Must not reenter
  // this Entity. Disk mode also commits its own storage before success.
  std::function<void(const Transfer &, const Filestore &)> commit_received = {};
  FilestoreConfig filestore = {};
  // Run receive writes, filestore requests, and commit_received on one worker.
  // Entity itself remains single-threaded. Call poll_storage() regularly.
  bool asynchronous_storage = false;
};

enum class Event_Type {
  TransactionStarted,
  MetadataReceived,
  FileSegmentReceived,
  EOFSent,
  EOFReceived,
  TransactionFinished,
  Fault,
  Suspended,
  Resumed,
  Report,
  Abandoned
};

struct Event {
  Event_Type type;
  uint64_t source_id;
  uint64_t sequence;
  Condition_Code condition = Condition_Code::NoError;
  uint64_t progress = 0;
  std::optional<uint64_t> file_size;
  std::string source_name;
  std::string destination_name;
  std::vector<TLV> metadata_options;
  std::optional<uint64_t> segment_offset;
  std::optional<size_t> segment_length;
  std::optional<Record_Continuation_State> record_state;
  Bytes segment_metadata;
  std::optional<Delivery_Code> delivery;
  std::optional<File_Status> file_status;
  std::vector<TLV> filestore_responses;
};

struct Transaction_Report {
  uint64_t source_id;
  uint64_t sequence;
  uint64_t progress;
  bool terminated;
  bool suspended;
  Condition_Code condition;
};

// A CFDP entity with in-memory transaction state and a selectable filestore.
// The caller transports emitted PDUs, advances the
// timers with advance() or tick(), and drains delivered files with
// take_completed().
class ReceiveStorage;
struct StoredTransfer;

class Entity {
public:
  explicit Entity(Config config);
  ~Entity();
  // Omitted mode, checksum, and closure options use the destination peer defaults.
  uint64_t put(uint64_t destination_id, std::string destination_name,
               Bytes data,
               std::optional<PDU_Transmission_Mode> mode = std::nullopt,
               PutOptions options = {});
  uint64_t proxy_put(uint64_t respondent_id, uint64_t beneficiary_id,
                     std::string source_name, std::string destination_name,
                     ProxyOptions options = {});
  uint64_t cancel_proxy(uint64_t respondent_id, uint64_t originating_sequence);
  uint64_t directory_listing(uint64_t remote_id, std::string directory_name,
                             std::string output_name);
  uint64_t remote_status(uint64_t remote_id, uint64_t source_id,
                         uint64_t sequence, std::string report_file_name);
  uint64_t remote_suspend(uint64_t remote_id, uint64_t source_id,
                          uint64_t sequence);
  uint64_t remote_resume(uint64_t remote_id, uint64_t source_id,
                         uint64_t sequence);
  void receive(const Bytes &pdu);
  // Nonblocking: publish finished storage jobs and start the next waiting one.
  void poll_storage();
  bool storage_busy() const { return storage_key_.has_value(); }
  void cancel_send(uint64_t sequence);
  void cancel_receive(uint64_t source_id, uint64_t sequence);
  void suspend_send(uint64_t sequence);
  void resume_send(uint64_t sequence);
  void suspend_receive(uint64_t source_id, uint64_t sequence);
  void resume_receive(uint64_t source_id, uint64_t sequence);
  void request_nak(uint64_t source_id, uint64_t sequence);
  void prompt_nak(uint64_t sequence);
  void prompt_keep_alive(uint64_t sequence);
  Transaction_Report report_send(uint64_t sequence);
  Transaction_Report report_receive(uint64_t source_id, uint64_t sequence);
  void configure_peer(uint64_t peer_id, PeerConfig config);
  PeerConfig peer_settings(uint64_t peer_id) const;
  Config local_settings() const;
  void set_link_opportunity(uint64_t peer_id, bool transmit, bool receive);
  void forget_send(uint64_t sequence);
  void forget_receive(uint64_t source_id, uint64_t sequence);
  void advance(std::chrono::milliseconds elapsed);
  void tick();
  // Calls send(peer_id, bytes) for each eligible PDU. False retains it for
  // retry. The callback must not reenter this Entity or throw.
  size_t
  send_outgoing(const std::function<bool(uint64_t, const Bytes &)> &send);
  std::vector<Bytes> take_outgoing();
  std::vector<Transfer> take_completed();
  std::vector<Event> take_events();
  bool sender_complete(uint64_t sequence) const;
  std::optional<Condition_Code> sender_result(uint64_t sequence) const;
  uint64_t next_sequence() const;
  std::vector<TLV> sender_filestore_responses(uint64_t sequence) const;
  bool receiver_complete(uint64_t source_id, uint64_t sequence) const;
  std::optional<Condition_Code> receiver_result(uint64_t source_id,
                                                uint64_t sequence) const;
  void write_file(std::string name, Bytes data,
                  std::vector<size_t> record_lengths = {});
  std::optional<Bytes> read_file(const std::string &name) const;
  void save_filestore(const std::filesystem::path &path) const;
  void load_filestore(const std::filesystem::path &path);

private:
  friend class UserOperations;
  struct Key {
    uint64_t source;
    uint64_t sequence;
    bool operator<(const Key &other) const {
      return source < other.source ||
             (source == other.source && sequence < other.sequence);
    }
  };
  struct RemoteOrderResult {
    Transaction_Status status;
    bool suspended;
  };
  struct Sender {
    PDU_Header header;
    Metadata_PDU metadata;
    size_t file_size = 0;
    std::vector<FileData_PDU> segments;
    EOF_PDU eof;
    bool eof_issued = false;
    bool eof_acked = false;
    bool done = false;
    bool cancelling = false;
    Condition_Code result = Condition_Code::NoError;
    unsigned ack_retries = 0;
    unsigned idle_ticks = 0;
    unsigned ack_ticks = 0;
    unsigned check_ticks = 0;
    unsigned check_retries = 0;
    unsigned keep_alive_stalls = 0;
    uint64_t keep_alive_progress = 0;
    uint64_t sent_progress = 0;
    std::vector<TLV> filestore_responses;
    bool suspended = false;
    bool local_suspended = false;
  };
  struct Receiver {
    PDU_Header header;
    std::optional<Metadata_PDU> metadata;
    std::optional<EOF_PDU> eof;
    Bytes data;
    std::vector<uint8_t> present;
    std::set<size_t> record_ends;
    bool records_preserved = false;
    size_t covered = 0;
    bool delivered = false;
    bool terminated = false;
    bool finished_acked = false;
    bool finished_issued = false;
    bool finished_pending = false;
    Finished_PDU finished;
    unsigned nak_retries = 0;
    unsigned ack_retries = 0;
    unsigned idle_ticks = 0;
    unsigned keep_alive_ticks = 0;
    unsigned nak_ticks = 0;
    unsigned ack_ticks = 0;
    unsigned check_ticks = 0;
    unsigned check_retries = 0;
    bool suspended = false;
    bool local_suspended = false;
    bool storage_pending = false;
    std::vector<size_t> storage_records;
  };
  void emit(PDU_Header header, PDU_Body body);
  bool queue_pending_finished();
  void mark_issued(const Bytes &bytes);
  const PeerConfig &peer(uint64_t id) const;
  size_t active_data_bytes() const;
  void step();
  void request_missing(Receiver &r, bool force = false, uint64_t scope_end = 0);
  void fail(Sender &s, Condition_Code condition);
  void fail(Receiver &r, Condition_Code condition);
  void maybe_complete(Receiver &r);
  void start_receive_storage(Receiver &r);
  void apply_receive_storage(Receiver &r, StoredTransfer result);
  void validate_finished_capacity(const Receiver &r,
                                  const Finished_PDU &finished) const;
  RemoteOrderResult apply_remote_order(uint64_t requester, uint64_t source_id,
                                       uint64_t sequence, bool suspend);
  std::optional<Transaction_Report> find_report(uint64_t source_id,
                                                uint64_t sequence);
  void event(Event_Type type, const PDU_Header &header,
             Condition_Code condition = Condition_Code::NoError,
             uint64_t progress = 0);
  Config config_;
  std::chrono::milliseconds elapsed_remainder_{0};
  uint64_t next_sequence_ = 1;
  std::map<uint64_t, Sender> senders_;
  std::map<Key, Receiver> receivers_;
  std::map<Key, std::set<uint64_t>> remote_suspensions_;
  struct Queued_PDU {
    uint64_t peer;
    Bytes bytes;
  };
  std::vector<Queued_PDU> outgoing_;
  size_t queued_bytes_ = 0;
  std::map<uint64_t, std::pair<bool, bool>> link_opportunities_;
  std::vector<Transfer> completed_;
  std::vector<Event> events_;
  Filestore filestore_;
  UserOperations user_operations_;
  std::optional<Key> storage_key_;
  size_t storage_data_bytes_ = 0;
  // Destroy/join the worker before any Entity-owned storage or callbacks.
  std::unique_ptr<ReceiveStorage> storage_;
};
} // namespace cfdp
