#include "filestore.hpp"
#include "wire_constants.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace cfdp {
namespace {
constexpr size_t max_snapshot_bytes = 512ull * 1024 * 1024;
constexpr uint32_t max_entries = 10000;
// Local snapshot v1: magic, then two 32-bit entry counts.
constexpr char snapshot_magic[] = "CFDPFS01";
constexpr size_t snapshot_magic_size = sizeof(snapshot_magic) - 1;
constexpr size_t snapshot_header_size =
    snapshot_magic_size + 2 * sizeof(uint32_t);
constexpr size_t snapshot_count_width = sizeof(uint32_t);
constexpr size_t snapshot_length_width = sizeof(uint64_t);
constexpr size_t snapshot_name_width = sizeof(uint16_t);

void append_uint(Bytes &out, uint64_t value, size_t width) {
  for (size_t i = width; i > 0; --i)
    out.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
}
void append_name(Bytes &out, const std::string &name) {
  if (name.size() > wire::max_lv_length)
    throw std::length_error("filestore name too long");
  append_uint(out, name.size(), snapshot_name_width);
  out.insert(out.end(), name.begin(), name.end());
}
uint64_t read_uint(const Bytes &bytes, size_t &pos, size_t width) {
  if (width > bytes.size() - pos)
    throw std::invalid_argument("truncated filestore snapshot");
  uint64_t result = 0;
  for (size_t i = 0; i < width; ++i)
    result = (result << 8) | bytes[pos++];
  return result;
}
std::string read_name(const Bytes &bytes, size_t &pos) {
  const size_t length = read_uint(bytes, pos, snapshot_name_width);
  if (length > wire::max_lv_length || length > bytes.size() - pos)
    throw std::invalid_argument("invalid filestore snapshot name");
  std::string result(bytes.begin() + pos, bytes.begin() + pos + length);
  pos += length;
  return result;
}
void write_all(int fd, const Bytes &bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n <= 0)
      throw std::runtime_error("cannot write filestore snapshot");
    offset += static_cast<size_t>(n);
  }
}
} // namespace

void Filestore::save(const std::filesystem::path &path) const {
  if (files_.size() > max_entries || directories_.size() > max_entries)
    throw std::length_error("filestore snapshot has too many entries");
  Bytes bytes(snapshot_magic, snapshot_magic + snapshot_magic_size);
  append_uint(bytes, directories_.size(), snapshot_count_width);
  append_uint(bytes, files_.size(), snapshot_count_width);
  for (const auto &name : directories_)
    append_name(bytes, name);
  for (const auto &[name, file] : files_) {
    append_name(bytes, name);
    const auto data = file->read();
    append_uint(bytes, data.size(), snapshot_length_width);
    bytes.insert(bytes.end(), data.begin(), data.end());
    const size_t count = file->record_lengths.size();
    if (count > max_entries)
      throw std::length_error("too many filestore records");
    append_uint(bytes, count, snapshot_count_width);
    if (count)
      for (size_t length : file->record_lengths)
        append_uint(bytes, length, snapshot_length_width);
    if (bytes.size() > max_snapshot_bytes)
      throw std::length_error("filestore snapshot exceeds 512 MiB");
  }
  std::string pattern = path.string() + ".tmpXXXXXX";
  const int fd = mkstemp(pattern.data());
  if (fd < 0)
    throw std::runtime_error("cannot create filestore snapshot temporary file");
  bool closed = false;
  try {
    write_all(fd, bytes);
    if (fsync(fd) < 0)
      throw std::runtime_error("cannot sync filestore snapshot");
    if (close(fd) < 0)
      throw std::runtime_error("cannot close filestore snapshot");
    closed = true;
    if (::rename(pattern.c_str(), path.c_str()) < 0)
      throw std::runtime_error("cannot replace filestore snapshot");
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                   : path.parent_path();
    const int directory_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0 || fsync(directory_fd) < 0) {
      if (directory_fd >= 0)
        close(directory_fd);
      throw std::runtime_error("cannot sync filestore directory");
    }
    close(directory_fd);
  } catch (...) {
    if (!closed)
      close(fd);
    std::error_code ignored;
    std::filesystem::remove(pattern, ignored);
    throw;
  }
}

void Filestore::load(const std::filesystem::path &path, size_t max_file_size) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error)
      throw std::runtime_error("cannot stat filestore snapshot");
    return;
  }
  const auto size = std::filesystem::file_size(path);
  if (size > max_snapshot_bytes)
    throw std::length_error("filestore snapshot exceeds 512 MiB");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open filestore snapshot");
  Bytes bytes((std::istreambuf_iterator<char>(file)),
              std::istreambuf_iterator<char>());
  if (file.bad() || bytes.size() != size ||
      bytes.size() < snapshot_header_size ||
      !std::equal(bytes.begin(), bytes.begin() + snapshot_magic_size,
                  snapshot_magic))
    throw std::invalid_argument("invalid filestore snapshot header");
  size_t pos = snapshot_magic_size;
  const auto directory_count = read_uint(bytes, pos, snapshot_count_width);
  const auto file_count = read_uint(bytes, pos, snapshot_count_width);
  if (directory_count > max_entries || file_count > max_entries)
    throw std::invalid_argument("too many filestore snapshot entries");
  std::set<std::string> directories;
  std::map<std::string, std::shared_ptr<const File>> files;
  for (size_t i = 0; i < directory_count; ++i)
    if (!directories.insert(read_name(bytes, pos)).second)
      throw std::invalid_argument("duplicate filestore directory");
  for (size_t i = 0; i < file_count; ++i) {
    const auto name = read_name(bytes, pos);
    const uint64_t length = read_uint(bytes, pos, snapshot_length_width);
    if (length > max_file_size || length > bytes.size() - pos)
      throw std::invalid_argument("invalid filestore file length");
    File stored;
    stored.data = Bytes(bytes.begin() + pos, bytes.begin() + pos + length);
    pos += static_cast<size_t>(length);
    const auto count = read_uint(bytes, pos, snapshot_count_width);
    if (count > max_entries)
      throw std::invalid_argument("too many filestore records");
    if (count) {
      size_t total = 0;
      for (size_t j = 0; j < count; ++j) {
        const uint64_t record = read_uint(bytes, pos, snapshot_length_width);
        if (!record || record > length - total)
          throw std::invalid_argument("invalid filestore record length");
        total += static_cast<size_t>(record);
        stored.record_lengths.push_back(static_cast<size_t>(record));
      }
      if (total != length)
        throw std::invalid_argument("filestore records do not cover file");
    }
    if (!files.emplace(name, std::make_shared<const File>(std::move(stored)))
             .second)
      throw std::invalid_argument("duplicate filestore file");
  }
  if (pos != bytes.size())
    throw std::invalid_argument("trailing filestore snapshot data");
  Filestore prepared(*this);
  prepared.directories_ = std::move(directories);
  prepared.files_.clear();
  for (const auto &[name, file] : files)
    prepared.files_.emplace(name, prepared.make_file(file->data,
                                                   file->record_lengths));
  prepared.commit();
  swap(prepared);
}
} // namespace cfdp
