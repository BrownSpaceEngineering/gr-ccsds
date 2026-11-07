#pragma once

#include "cfdp.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cfdp {

using Bytes = std::vector<uint8_t>;

enum class Filestore_Mode { Memory, Disk };

struct FilestoreConfig {
  Filestore_Mode mode = Filestore_Mode::Memory;
  // Private managed directory for Disk mode, opened by one entity at a time.
  std::filesystem::path directory = {};
};

struct DiskFile;
struct DiskDirectory;

class Filestore {
public:
  Filestore() = default;
  explicit Filestore(const FilestoreConfig &config, size_t max_file_size);
  static void validate_request(const TLV &request);
  void write(std::string name, Bytes data,
             std::vector<size_t> record_lengths = {});
  std::optional<Bytes> read(const std::string &name) const;
  std::optional<std::vector<size_t>> records(const std::string &name) const;
  std::pair<bool, std::string> listing(const std::string &directory) const;
  std::vector<TLV> execute(const std::vector<TLV> &options);
  void save(const std::filesystem::path &path) const;
  void load(const std::filesystem::path &path, size_t max_file_size);
  // Publish staged changes durably in Disk mode; no-op in Memory mode.
  void commit();
  void swap(Filestore &other) noexcept;

private:
  struct File {
    Bytes data;
    std::vector<size_t> record_lengths;
    std::shared_ptr<const DiskFile> disk = {};
    Bytes read() const;
    size_t size() const;
  };
  std::shared_ptr<const File> make_file(Bytes data,
                                      std::vector<size_t> record_lengths);
  std::map<std::string, std::shared_ptr<const File>> files_;
  std::set<std::string> directories_;
  std::shared_ptr<DiskDirectory> disk_;
};

} // namespace cfdp
