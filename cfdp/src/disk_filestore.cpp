#include "filestore.hpp"
#include "wire_constants.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace cfdp {
namespace {
constexpr size_t max_catalog_bytes = 64 * 1024 * 1024;
constexpr size_t max_catalog_entries = 10000;
constexpr char catalog_magic[] = "CFDPDS01";

void sync_directory(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    throw std::runtime_error("cannot open disk filestore directory");
  const int result = ::fsync(fd);
  ::close(fd);
  if (result < 0)
    throw std::runtime_error("cannot sync disk filestore directory");
}

bool object_name(const std::string &name) {
  return name.size() == 11 && name.starts_with("data-") &&
         std::all_of(name.begin() + 5, name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');
         });
}

void write_all(int fd, const Bytes &data) {
  size_t pos = 0;
  while (pos < data.size()) {
    const auto n = ::write(fd, data.data() + pos, data.size() - pos);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("cannot write disk filestore");
    pos += static_cast<size_t>(n);
  }
  if (::fsync(fd) < 0)
    throw std::runtime_error("cannot sync disk filestore file");
}

Bytes read_all(const std::filesystem::path &path, size_t expected) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    throw std::runtime_error("cannot open disk filestore file");
  try {
    struct stat info{};
    if (::fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) != expected)
      throw std::runtime_error("invalid disk filestore file size");
    Bytes data(expected);
    size_t pos = 0;
    while (pos < data.size()) {
      const auto n = ::read(fd, data.data() + pos, data.size() - pos);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("cannot read disk filestore file");
      pos += static_cast<size_t>(n);
    }
    ::close(fd);
    return data;
  } catch (...) {
    ::close(fd);
    throw;
  }
}
} // namespace

// The catalog maps virtual names to immutable files. Copying a Filestore stages
// metadata changes without copying file contents or changing the live catalog.
struct DiskDirectory {
  std::filesystem::path path;
  int lock = -1;
  bool ready = false;
  std::set<std::string> current;
  std::map<std::string, std::weak_ptr<const DiskFile>> objects;

  explicit DiskDirectory(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> new_parents;
    for (auto candidate = std::filesystem::absolute(directory);
         !std::filesystem::exists(candidate);
         candidate = candidate.parent_path())
      new_parents.push_back(candidate.parent_path());
    std::filesystem::create_directories(directory);
    for (const auto &parent : new_parents)
      sync_directory(parent);
    path = std::filesystem::canonical(directory);
    lock =
        ::open((path / ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0)
      throw std::runtime_error("cannot open disk filestore lock");
    if (::flock(lock, LOCK_EX | LOCK_NB) < 0) {
      ::close(lock);
      lock = -1;
      throw std::runtime_error("disk filestore is already in use");
    }
  }
  ~DiskDirectory() {
    if (lock >= 0)
      ::close(lock);
  }
  void sync() const { sync_directory(path); }
};

struct DiskFile {
  std::shared_ptr<DiskDirectory> directory;
  std::string name;
  size_t length;
  bool loaded;
  DiskFile(std::shared_ptr<DiskDirectory> root, std::string object, size_t size,
           bool existing)
      : directory(std::move(root)), name(std::move(object)), length(size),
        loaded(existing) {}
  ~DiskFile() {
    // Never delete objects while rejecting a corrupt catalog at startup.
    if ((!loaded || directory->ready) && !directory->current.contains(name)) {
      std::error_code ignored;
      std::filesystem::remove(directory->path / name, ignored);
    }
    directory->objects.erase(name);
  }
};

Bytes Filestore::File::read() const {
  return disk ? read_all(disk->directory->path / disk->name, disk->length)
              : data;
}

size_t Filestore::File::size() const {
  return disk ? disk->length : data.size();
}

std::shared_ptr<const Filestore::File>
Filestore::make_file(Bytes data, std::vector<size_t> record_lengths) {
  auto file = std::make_shared<File>();
  file->record_lengths = std::move(record_lengths);
  if (!disk_) {
    file->data = std::move(data);
    return file;
  }
  std::string pattern = (disk_->path / "data-XXXXXX").string();
  const int fd = ::mkstemp(pattern.data());
  if (fd < 0)
    throw std::runtime_error("cannot create disk filestore file");
  bool closed = false;
  try {
    file->disk = std::make_shared<DiskFile>(
        disk_, std::filesystem::path(pattern).filename().string(), data.size(),
        false);
    disk_->objects[file->disk->name] = file->disk;
    write_all(fd, data);
    const int result = ::close(fd);
    closed = true;
    if (result < 0)
      throw std::runtime_error("cannot close disk filestore file");
    return file;
  } catch (...) {
    if (!closed)
      ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(pattern, ignored);
    throw;
  }
}

Filestore::Filestore(const FilestoreConfig &config, size_t max_file_size) {
  if (config.mode == Filestore_Mode::Memory) {
    if (!config.directory.empty())
      throw std::invalid_argument("memory filestore does not use a directory");
    return;
  }
  if (config.mode != Filestore_Mode::Disk || config.directory.empty())
    throw std::invalid_argument("disk filestore requires a directory");
  disk_ = std::make_shared<DiskDirectory>(config.directory);
  const auto catalog = disk_->path / "catalog";
  if (std::filesystem::exists(catalog)) {
    const auto length = std::filesystem::file_size(catalog);
    if (length > max_catalog_bytes)
      throw std::invalid_argument("disk filestore catalog is too large");
    const auto bytes = read_all(catalog, length);
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::string magic;
    size_t directory_count = 0, file_count = 0;
    if (!(input >> magic >> directory_count >> file_count) ||
        magic != catalog_magic || directory_count > max_catalog_entries ||
        file_count > max_catalog_entries)
      throw std::invalid_argument("invalid disk filestore catalog");
    for (size_t i = 0; i < directory_count; ++i) {
      std::string name;
      if (!(input >> std::quoted(name)) || name.empty() ||
          name.size() > wire::max_lv_length ||
          !directories_.insert(name).second)
        throw std::invalid_argument("invalid disk filestore directory entry");
    }
    for (size_t i = 0; i < file_count; ++i) {
      std::string name, object;
      size_t size = 0, records = 0;
      if (!(input >> std::quoted(name) >> std::quoted(object) >> size >>
            records) ||
          name.empty() || name.size() > wire::max_lv_length ||
          !object_name(object) || size > max_file_size || records > size ||
          records > max_catalog_bytes / sizeof(size_t))
        throw std::invalid_argument("invalid disk filestore file entry");
      auto file = std::make_shared<File>();
      size_t total = 0;
      for (size_t j = 0; j < records; ++j) {
        size_t record = 0;
        if (!(input >> record) || !record || record > size - total)
          throw std::invalid_argument("invalid disk filestore record length");
        file->record_lengths.push_back(record);
        total += record;
      }
      if (records && total != size)
        throw std::invalid_argument("disk filestore records do not cover file");
      const auto object_path = disk_->path / object;
      if (!std::filesystem::is_regular_file(
              std::filesystem::symlink_status(object_path)) ||
          std::filesystem::file_size(object_path) != size)
        throw std::invalid_argument("missing or invalid disk filestore object");
      disk_->current.insert(object);
      file->disk = disk_->objects[object].lock();
      if (file->disk && file->disk->length != size)
        throw std::invalid_argument("inconsistent disk filestore object size");
      if (!file->disk) {
        file->disk = std::make_shared<DiskFile>(disk_, object, size, true);
        disk_->objects[object] = file->disk;
      }
      if (!files_.emplace(name, std::move(file)).second)
        throw std::invalid_argument("duplicate disk filestore file");
    }
    input >> std::ws;
    if (!input.eof())
      throw std::invalid_argument("extra disk filestore catalog data");
  } else {
    // Initialize an empty catalog before any payload can be staged. A later
    // restart can then safely identify orphaned writes. Never adopt or clean
    // an unrelated nonempty directory as though it were a managed store.
    for (const auto &entry : std::filesystem::directory_iterator(disk_->path))
      if (entry.path().filename() != ".lock")
        throw std::invalid_argument("disk filestore directory is not empty");
    commit();
  }
  disk_->ready = true;
  // Remove orphaned objects from interrupted writes, retaining only the
  // successfully committed catalog. The directory lock excludes other users.
  for (const auto &entry : std::filesystem::directory_iterator(disk_->path)) {
    const auto name = entry.path().filename().string();
    if (object_name(name) && !disk_->current.contains(name))
      std::filesystem::remove(entry.path());
  }
}

void Filestore::commit() {
  if (!disk_)
    return;
  if (files_.size() > max_catalog_entries ||
      directories_.size() > max_catalog_entries)
    throw std::length_error("disk filestore has too many entries");
  std::ostringstream catalog;
  catalog << catalog_magic << '\n'
          << directories_.size() << ' ' << files_.size() << '\n';
  for (const auto &name : directories_) {
    if (name.empty() || name.size() > wire::max_lv_length)
      throw std::invalid_argument("invalid disk filestore directory name");
    catalog << std::quoted(name) << '\n';
  }
  std::set<std::string> current;
  for (const auto &[name, file] : files_) {
    if (name.empty() || name.size() > wire::max_lv_length || !file->disk ||
        file->disk->directory != disk_)
      throw std::invalid_argument("invalid disk filestore file");
    current.insert(file->disk->name);
    catalog << std::quoted(name) << ' ' << std::quoted(file->disk->name) << ' '
            << file->size() << ' ' << file->record_lengths.size();
    for (const auto length : file->record_lengths)
      catalog << ' ' << length;
    catalog << '\n';
    if (catalog.tellp() > static_cast<std::streamoff>(max_catalog_bytes))
      throw std::length_error("disk filestore catalog is too large");
  }
  const auto text = catalog.str();
  const Bytes bytes(text.begin(), text.end());
  std::string temporary = (disk_->path / "catalog-XXXXXX").string();
  const int fd = ::mkstemp(temporary.data());
  if (fd < 0)
    throw std::runtime_error("cannot create disk filestore catalog");
  bool closed = false;
  try {
    write_all(fd, bytes);
    const int result = ::close(fd);
    closed = true;
    if (result < 0)
      throw std::runtime_error("cannot close disk filestore catalog");
    // Make object directory entries durable before publishing their names.
    disk_->sync();
    std::filesystem::rename(temporary, disk_->path / "catalog");
    // Protect newly published objects even if the final sync fails.
    disk_->current.swap(current);
    disk_->sync();
    for (const auto &old : current) {
      if (!disk_->current.contains(old) && !disk_->objects.contains(old)) {
        std::error_code ignored;
        std::filesystem::remove(disk_->path / old, ignored);
      }
    }
  } catch (...) {
    if (!closed)
      ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}
} // namespace cfdp
