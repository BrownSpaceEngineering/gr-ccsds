#include "filestore.hpp"
#include "wire_constants.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cfdp {
namespace {
// Filestore response status 15 means the request was skipped after a failure.
constexpr uint8_t not_performed_status = 15;
struct ParsedRequest {
  Filestore_Action action;
  std::string first;
  std::optional<std::string> second;
};

ParsedRequest parse_request(const TLV &request) {
  const auto &raw = request.value;
  if (raw.empty() || raw.size() >= wire::max_lv_length ||
      (raw[0] & wire::low_nibble_mask) ||
      (raw[0] >> wire::high_nibble_shift) >
          static_cast<uint8_t>(Filestore_Action::DenyDirectory))
    throw std::invalid_argument("invalid filestore request TLV");
  const auto action =
      static_cast<Filestore_Action>(raw[0] >> wire::high_nibble_shift);
  size_t pos = 1;
  auto read_name = [&]() {
    if (pos >= raw.size() || raw.size() - pos - 1 < raw[pos])
      throw std::invalid_argument("truncated filestore name");
    const size_t length = raw[pos++];
    std::string name(raw.begin() + pos, raw.begin() + pos + length);
    pos += length;
    return name;
  };
  auto first = read_name();
  std::optional<std::string> second;
  if (action == Filestore_Action::RenameFile ||
      action == Filestore_Action::AppendFile ||
      action == Filestore_Action::ReplaceFile)
    second = read_name();
  if (pos != raw.size())
    throw std::invalid_argument("extra filestore request data");
  return {action, std::move(first), std::move(second)};
}
} // namespace

void Filestore::validate_request(const TLV &request) {
  (void)parse_request(request);
}

std::vector<TLV> Filestore::execute(const std::vector<TLV> &options) {
  std::vector<TLV> responses;
  bool skip = false;
  for (const auto &request : options) {
    if (request.type != TLV_Type::FilestoreRequest)
      continue;
    const auto [action, first, second] = parse_request(request);
    uint8_t status = skip ? not_performed_status : 0;
    if (!skip) {
      // Status 1 and 2 are defined per action; the condition beside each
      // assignment identifies the failure represented by that status.
      switch (action) {
      case Filestore_Action::CreateFile:
        if (files_.contains(first))
          status = 1;
        else
          files_.emplace(first, make_file({}, {}));
        break;
      case Filestore_Action::DeleteFile:
        if (!files_.erase(first))
          status = 1;
        break;
      case Filestore_Action::RenameFile: {
        auto it = files_.find(first);
        if (it == files_.end())
          status = 1;
        else if (files_.contains(*second))
          status = 2;
        else {
          files_.emplace(*second, std::move(it->second));
          files_.erase(it);
        }
        break;
      }
      case Filestore_Action::AppendFile:
        if (!files_.contains(first))
          status = 1;
        else if (!files_.contains(*second))
          status = 2;
        else {
          auto target = files_.at(first)->read();
          const auto suffix = files_.at(*second)->read();
          target.insert(target.end(), suffix.begin(), suffix.end());
          files_.at(first) = make_file(std::move(target), {});
        }
        break;
      case Filestore_Action::ReplaceFile:
        if (!files_.contains(first))
          status = 1;
        else if (!files_.contains(*second))
          status = 2;
        else {
          files_.at(first) = make_file(files_.at(*second)->read(), {});
        }
        break;
      case Filestore_Action::CreateDirectory:
        if (!directories_.insert(first).second)
          status = 1;
        break;
      case Filestore_Action::RemoveDirectory:
      case Filestore_Action::DenyDirectory:
        if (!directories_.contains(first)) {
          if (action == Filestore_Action::RemoveDirectory)
            status = 1;
        } else {
          const std::string prefix = first + "/";
          bool nonempty = false;
          for (const auto &[name, value] : files_) {
            (void)value;
            if (name.starts_with(prefix)) {
              nonempty = true;
              break;
            }
          }
          if (!nonempty)
            for (const auto &name : directories_)
              if (name != first && name.starts_with(prefix)) {
                nonempty = true;
                break;
              }
          if (nonempty)
            status = 2;
          else
            directories_.erase(first);
        }
        break;
      case Filestore_Action::DenyFile:
        files_.erase(first);
        break;
      }
      if (status)
        skip = true;
    }
    responses.push_back(filestore_response(action, status, first, second));
  }
  return responses;
}

void Filestore::write(std::string name, Bytes data,
                      std::vector<size_t> record_lengths) {
  if (name.empty())
    throw std::invalid_argument("invalid filestore file");
  if (!record_lengths.empty()) {
    size_t total = 0;
    for (size_t length : record_lengths) {
      if (!length || length > data.size() - total)
        throw std::invalid_argument("invalid filestore record lengths");
      total += length;
    }
    if (total != data.size())
      throw std::invalid_argument("filestore record lengths do not cover file");
  }
  auto file = make_file(std::move(data), std::move(record_lengths));
  files_.insert_or_assign(std::move(name), std::move(file));
}

std::optional<Bytes> Filestore::read(const std::string &name) const {
  const auto file = files_.find(name);
  if (file == files_.end())
    return std::nullopt;
  return file->second->read();
}

std::optional<std::vector<size_t>>
Filestore::records(const std::string &name) const {
  const auto file = files_.find(name);
  if (file == files_.end() || file->second->record_lengths.empty())
    return std::nullopt;
  return file->second->record_lengths;
}

std::pair<bool, std::string>
Filestore::listing(const std::string &directory) const {
  const bool exists = directory.empty() || directories_.contains(directory);
  std::string listing;
  if (exists) {
    const auto prefix = directory.empty() ? std::string{} : directory + "/";
    for (const auto &[name, file] : files_) {
      (void)file;
      if (name.starts_with(prefix))
        listing += name + "\n";
    }
    for (const auto &name : directories_)
      if (name.starts_with(prefix))
        listing += name + "/\n";
  }
  return {exists, std::move(listing)};
}

void Filestore::swap(Filestore &other) noexcept {
  files_.swap(other.files_);
  directories_.swap(other.directories_);
  disk_.swap(other.disk_);
}

} // namespace cfdp
