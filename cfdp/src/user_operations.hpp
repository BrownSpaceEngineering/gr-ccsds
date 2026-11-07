#pragma once

#include "filestore.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cfdp {

class Entity;
struct ProxyOptions;

// Handles reserved Messages to User after the protocol entity delivers them.
class UserOperations {
public:
  uint64_t proxy_put(Entity &entity, uint64_t respondent_id,
                     uint64_t beneficiary_id, std::string source_name,
                     std::string destination_name, ProxyOptions options);
  uint64_t cancel_proxy(Entity &entity, uint64_t respondent_id,
                        uint64_t originating_sequence);
  uint64_t directory_listing(Entity &entity, uint64_t remote_id,
                             std::string directory_name,
                             std::string output_name);
  uint64_t remote_status(Entity &entity, uint64_t remote_id, uint64_t source_id,
                         uint64_t sequence, std::string report_file_name);
  uint64_t remote_suspend(Entity &entity, uint64_t remote_id,
                          uint64_t source_id, uint64_t sequence);
  uint64_t remote_resume(Entity &entity, uint64_t remote_id, uint64_t source_id,
                         uint64_t sequence);
  void process(Entity &entity, const PDU_Header &header,
               const Metadata_PDU &metadata, size_t covered);
  void on_sender_finished(Entity &entity, uint64_t sequence,
                          Condition_Code result,
                          const std::vector<TLV> &filestore_responses);
  void forget_sender(uint64_t sequence);

private:
  struct ReservedMessage {
    uint8_t type;
    const Bytes *value;
  };
  struct ProxyContext {
    uint64_t originator_id;
    uint64_t originating_sequence;
  };
  void report_proxy(Entity &entity, const ProxyContext &context,
                    Condition_Code result,
                    const std::vector<TLV> &filestore_responses);
  void handle_proxy_cancel(Entity &entity, const PDU_Header &header,
                           const Bytes &value,
                           const std::vector<ReservedMessage> &messages);
  void handle_remote_request(Entity &entity, const PDU_Header &header,
                             uint8_t type, const Bytes &value);
  void handle_directory_listing(Entity &entity, const PDU_Header &header,
                                const Bytes &value);
  void handle_proxy_put(Entity &entity, const PDU_Header &header,
                        const Bytes &value,
                        const std::vector<ReservedMessage> &messages);
  std::map<uint64_t, ProxyContext> pending_proxies_;
};

} // namespace cfdp
