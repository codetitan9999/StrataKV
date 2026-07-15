#include "sstable.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace stratakv {
namespace {

constexpr std::string_view kMagic = "STKV0001";
constexpr std::size_t kFooterSize =
    sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t) + 8;

template <typename UInt>
void AppendFixed(std::string& out, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xffU));
  }
}

template <typename UInt>
bool ReadFixed(std::string_view input, std::size_t* offset, UInt* value) {
  static_assert(std::is_unsigned_v<UInt>);
  if (*offset + sizeof(UInt) > input.size()) {
    return false;
  }

  UInt result = 0;
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    result |= static_cast<UInt>(
                  static_cast<unsigned char>(input[*offset + i]))
              << (8 * i);
  }

  *offset += sizeof(UInt);
  *value = result;
  return true;
}

std::uint32_t Checksum(std::string_view payload) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char byte : payload) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

Status EncodeEntry(std::string& out, const TableEntry& entry) {
  const std::string_view key = entry.key;
  const std::string_view value = entry.value;
  if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("SSTable key/value is too large");
  }
  if (entry.type != RecordType::kPut && entry.type != RecordType::kDelete) {
    return Status::InvalidArgument("invalid SSTable entry type");
  }
  if (entry.type == RecordType::kDelete && !value.empty()) {
    return Status::InvalidArgument("delete entries must not store values");
  }

  AppendFixed<std::uint8_t>(out, static_cast<std::uint8_t>(entry.type));
  AppendFixed<std::uint32_t>(out, static_cast<std::uint32_t>(key.size()));
  AppendFixed<std::uint32_t>(out, static_cast<std::uint32_t>(value.size()));
  out.append(key);
  out.append(value);
  return Status::OK();
}

Status DecodeEntries(std::string_view data_block, std::uint64_t entry_count,
                     std::vector<TableEntry>* out) {
  if (entry_count > data_block.size()) {
    return Status::Corruption("SSTable entry count exceeds data block size");
  }

  std::size_t offset = 0;
  out->clear();
  out->reserve(static_cast<std::size_t>(entry_count));

  while (offset < data_block.size()) {
    std::uint8_t type = 0;
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;
    if (!ReadFixed(data_block, &offset, &type) ||
        !ReadFixed(data_block, &offset, &key_size) ||
        !ReadFixed(data_block, &offset, &value_size)) {
      return Status::Corruption("short SSTable entry header");
    }

    const std::uint64_t total_size =
        static_cast<std::uint64_t>(offset) + key_size + value_size;
    if (total_size > data_block.size()) {
      return Status::Corruption("SSTable entry length exceeds data block");
    }

    std::string key(data_block.substr(offset, key_size));
    offset += key_size;
    std::string value(data_block.substr(offset, value_size));
    offset += value_size;

    if (key.empty()) {
      return Status::Corruption("SSTable contains an empty key");
    }
    if (!out->empty() && out->back().key >= key) {
      return Status::Corruption("SSTable keys are not strictly sorted");
    }
    if (type != static_cast<std::uint8_t>(RecordType::kPut) &&
        type != static_cast<std::uint8_t>(RecordType::kDelete)) {
      return Status::Corruption("SSTable contains an invalid entry type");
    }
    if (type == static_cast<std::uint8_t>(RecordType::kDelete) &&
        !value.empty()) {
      return Status::Corruption("SSTable tombstone stores a value");
    }

    out->push_back(TableEntry{static_cast<RecordType>(type), std::move(key),
                              std::move(value)});
  }

  if (out->size() != entry_count) {
    return Status::Corruption("SSTable entry count mismatch");
  }

  return Status::OK();
}

class SSTableIterator final : public Iterator {
 public:
  explicit SSTableIterator(std::vector<std::pair<std::string, std::string>> rows)
      : rows_(std::move(rows)) {}

  bool Valid() const override { return index_ < rows_.size(); }

  void SeekToFirst() override { index_ = 0; }

  void Seek(std::string_view target) override {
    const auto it = std::lower_bound(
        rows_.begin(), rows_.end(), target,
        [](const auto& row, std::string_view key) { return row.first < key; });
    index_ = static_cast<std::size_t>(it - rows_.begin());
  }

  void Next() override {
    if (Valid()) {
      ++index_;
    }
  }

  std::string_view key() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].first;
  }

  std::string_view value() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].second;
  }

  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::pair<std::string, std::string>> rows_;
  std::size_t index_ = 0;
};

}  // namespace

SSTableBuilder::SSTableBuilder(std::filesystem::path path)
    : path_(std::move(path)) {}

Status SSTableBuilder::Add(std::string_view key, std::string_view value) {
  return AddInternal(RecordType::kPut, key, value);
}

Status SSTableBuilder::AddDeletion(std::string_view key) {
  return AddInternal(RecordType::kDelete, key, "");
}

Status SSTableBuilder::AddInternal(RecordType type, std::string_view key,
                                   std::string_view value) {
  if (finished_) {
    return Status::InvalidArgument("cannot add entries after Finish");
  }
  if (key.empty()) {
    return Status::InvalidArgument("SSTable keys must not be empty");
  }
  if (has_last_key_ && last_key_ >= key) {
    return Status::InvalidArgument(
        "SSTable keys must be added in strictly increasing order");
  }
  if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("SSTable key/value is too large");
  }
  if (type == RecordType::kDelete && !value.empty()) {
    return Status::InvalidArgument("delete entries must not store values");
  }

  entries_.push_back(TableEntry{type, std::string(key), std::string(value)});
  last_key_ = std::string(key);
  has_last_key_ = true;
  return Status::OK();
}

Status SSTableBuilder::Finish(TableMetadata* metadata) {
  if (metadata == nullptr) {
    return Status::InvalidArgument("metadata output must not be null");
  }
  if (finished_) {
    return Status::InvalidArgument("SSTable builder is already finished");
  }
  if (entries_.empty()) {
    return Status::InvalidArgument("cannot finish an empty SSTable");
  }

  std::string data_block;
  for (const auto& entry : entries_) {
    Status encode_status = EncodeEntry(data_block, entry);
    if (!encode_status.ok()) {
      return encode_status;
    }
  }

  std::string footer;
  footer.reserve(kFooterSize);
  AppendFixed<std::uint64_t>(footer,
                             static_cast<std::uint64_t>(entries_.size()));
  AppendFixed<std::uint64_t>(footer,
                             static_cast<std::uint64_t>(data_block.size()));
  AppendFixed<std::uint32_t>(footer, Checksum(data_block));
  footer.append(kMagic);

  std::ofstream stream(path_, std::ios::binary | std::ios::out |
                                  std::ios::trunc);
  if (!stream) {
    return Status::IOError("failed to open SSTable for writing: " +
                           path_.string());
  }

  stream.write(data_block.data(),
               static_cast<std::streamsize>(data_block.size()));
  stream.write(footer.data(), static_cast<std::streamsize>(footer.size()));
  stream.flush();

  if (!stream) {
    return Status::IOError("failed to write SSTable: " + path_.string());
  }

  metadata->file_path = path_;
  metadata->smallest_key = entries_.front().key;
  metadata->largest_key = entries_.back().key;
  metadata->entry_count = static_cast<std::uint64_t>(entries_.size());
  metadata->file_size_bytes =
      static_cast<std::uint64_t>(data_block.size() + footer.size());

  finished_ = true;
  return Status::OK();
}

std::pair<std::unique_ptr<SSTableReader>, Status> SSTableReader::Open(
    std::filesystem::path path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return {nullptr,
            Status::IOError("failed to open SSTable for reading: " +
                            path.string())};
  }

  const std::streamoff file_size = stream.tellg();
  if (file_size < static_cast<std::streamoff>(kFooterSize)) {
    return {nullptr, Status::Corruption("SSTable is smaller than footer")};
  }

  std::string file(static_cast<std::size_t>(file_size), '\0');
  stream.seekg(0);
  stream.read(file.data(), static_cast<std::streamsize>(file.size()));
  if (!stream) {
    return {nullptr,
            Status::IOError("failed to read SSTable: " + path.string())};
  }

  const std::string_view footer(
      file.data() + file.size() - static_cast<std::ptrdiff_t>(kFooterSize),
      kFooterSize);
  std::size_t footer_offset = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t data_block_size = 0;
  std::uint32_t expected_checksum = 0;

  if (!ReadFixed(footer, &footer_offset, &entry_count) ||
      !ReadFixed(footer, &footer_offset, &data_block_size) ||
      !ReadFixed(footer, &footer_offset, &expected_checksum)) {
    return {nullptr, Status::Corruption("short SSTable footer")};
  }

  if (footer.substr(footer_offset, kMagic.size()) != kMagic) {
    return {nullptr, Status::Corruption("SSTable footer magic mismatch")};
  }

  if (data_block_size + kFooterSize != file.size()) {
    return {nullptr, Status::Corruption("SSTable data block size mismatch")};
  }

  const std::string_view data_block(file.data(),
                                    static_cast<std::size_t>(data_block_size));
  if (Checksum(data_block) != expected_checksum) {
    return {nullptr, Status::Corruption("SSTable data block checksum mismatch")};
  }

  std::vector<TableEntry> entries;
  Status decode_status = DecodeEntries(data_block, entry_count, &entries);
  if (!decode_status.ok()) {
    return {nullptr, decode_status};
  }

  TableMetadata metadata;
  metadata.file_path = path;
  metadata.entry_count = entry_count;
  metadata.file_size_bytes = static_cast<std::uint64_t>(file.size());
  if (!entries.empty()) {
    metadata.smallest_key = entries.front().key;
    metadata.largest_key = entries.back().key;
  }

  return {std::unique_ptr<SSTableReader>(
              new SSTableReader(path, std::move(entries), metadata)),
          Status::OK()};
}

TableLookup SSTableReader::Lookup(std::string_view key) const {
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const TableEntry& entry, std::string_view target) {
        return entry.key < target;
      });

  if (it == entries_.end() || it->key != key) {
    return {};
  }

  TableLookup lookup;
  lookup.found = true;
  lookup.deleted = it->type == RecordType::kDelete;
  if (!lookup.deleted) {
    lookup.value = it->value;
  }
  return lookup;
}

std::pair<std::string, Status> SSTableReader::Get(std::string_view key) const {
  const TableLookup lookup = Lookup(key);
  if (!lookup.found || lookup.deleted) {
    return {"", Status::NotFound("key not found in SSTable")};
  }

  return {lookup.value, Status::OK()};
}

std::unique_ptr<Iterator> SSTableReader::NewIterator() const {
  std::vector<std::pair<std::string, std::string>> rows;
  rows.reserve(entries_.size());
  for (const TableEntry& entry : entries_) {
    if (entry.type == RecordType::kPut) {
      rows.emplace_back(entry.key, entry.value);
    }
  }
  return std::make_unique<SSTableIterator>(std::move(rows));
}

const std::vector<TableEntry>& SSTableReader::entries() const {
  return entries_;
}

const TableMetadata& SSTableReader::metadata() const { return metadata_; }

SSTableReader::SSTableReader(
    std::filesystem::path path,
    std::vector<TableEntry> entries,
    TableMetadata metadata)
    : path_(std::move(path)),
      entries_(std::move(entries)),
      metadata_(std::move(metadata)) {}

}  // namespace stratakv
