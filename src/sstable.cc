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

constexpr std::string_view kMagic = "STKV0004";
constexpr std::string_view kCompressedLegacyMagic = "STKV0003";
constexpr std::string_view kIndexedLegacyMagic = "STKV0002";
constexpr std::string_view kLegacyMagic = "STKV0001";
constexpr std::size_t kFooterSize =
    sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint64_t) +
    sizeof(std::uint32_t) + 8;
constexpr std::size_t kFilterFooterSize =
    sizeof(std::uint64_t) * 5 + sizeof(std::uint32_t) * 2 + 8;
constexpr std::size_t kLegacyFooterSize =
    sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t) + 8;
constexpr std::size_t kBlockTrailerSize = sizeof(std::uint32_t);
constexpr std::size_t kRestartInterval = 16;
constexpr std::uint32_t kBloomBitsPerKey = 10;
constexpr std::uint32_t kBloomProbes = 7;
constexpr std::uint32_t kBloomFilterVersion = 1;

using BlockIndexEntry = TableBlockIndexEntry;

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

std::uint32_t BloomHash(std::string_view key) {
  return Checksum(key);
}

std::string BuildBloomFilter(const std::vector<TableEntry>& entries) {
  std::uint64_t bit_count =
      std::max<std::uint64_t>(64, entries.size() * kBloomBitsPerKey);
  bit_count = (bit_count + 7) & ~std::uint64_t{7};
  std::string bits(static_cast<std::size_t>(bit_count / 8), '\0');
  for (const auto& entry : entries) {
    std::uint32_t hash = BloomHash(entry.key);
    const std::uint32_t delta = (hash >> 17) | (hash << 15);
    for (std::uint32_t probe = 0; probe < kBloomProbes; ++probe) {
      const std::uint64_t bit = hash % bit_count;
      bits[static_cast<std::size_t>(bit / 8)] |=
          static_cast<char>(1U << (bit % 8));
      hash += delta;
    }
  }
  std::string filter;
  AppendFixed<std::uint32_t>(filter, kBloomFilterVersion);
  AppendFixed<std::uint32_t>(filter, kBloomProbes);
  AppendFixed<std::uint64_t>(filter, bit_count);
  filter.append(bits);
  return filter;
}

bool BloomMayContain(std::string_view filter, std::uint64_t bit_count,
                     std::uint32_t probes, std::string_view key) {
  std::uint32_t hash = BloomHash(key);
  const std::uint32_t delta = (hash >> 17) | (hash << 15);
  for (std::uint32_t probe = 0; probe < probes; ++probe) {
    const std::uint64_t bit = hash % bit_count;
    if ((static_cast<unsigned char>(filter[static_cast<std::size_t>(bit / 8)]) &
         (1U << (bit % 8))) == 0) {
      return false;
    }
    hash += delta;
  }
  return true;
}

std::size_t SharedPrefixLength(std::string_view left, std::string_view right) {
  const std::size_t limit = std::min(left.size(), right.size());
  std::size_t shared = 0;
  while (shared < limit && left[shared] == right[shared]) ++shared;
  return shared;
}

Status EncodeCompressedEntry(std::string& out, const TableEntry& entry,
                             std::string_view previous_key, bool restart) {
  if (entry.key.size() > std::numeric_limits<std::uint32_t>::max() ||
      entry.value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("SSTable key/value is too large");
  }
  const std::size_t shared =
      restart ? 0 : SharedPrefixLength(previous_key, entry.key);
  const std::size_t suffix = entry.key.size() - shared;
  AppendFixed<std::uint32_t>(out, static_cast<std::uint32_t>(shared));
  AppendFixed<std::uint32_t>(out, static_cast<std::uint32_t>(suffix));
  AppendFixed<std::uint32_t>(out,
                             static_cast<std::uint32_t>(entry.value.size()));
  AppendFixed<std::uint8_t>(out, static_cast<std::uint8_t>(entry.type));
  out.append(entry.key.data() + shared, suffix);
  out.append(entry.value);
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

Status DecodeBlock(std::string_view block, std::uint64_t entry_count,
                   bool compressed, std::vector<TableEntry>* out) {
  if (block.size() < kBlockTrailerSize) {
    return Status::Corruption("SSTable data block is smaller than trailer");
  }
  const std::string_view data = block.substr(0, block.size() - kBlockTrailerSize);
  std::size_t trailer_offset = data.size();
  std::uint32_t expected_checksum = 0;
  if (!ReadFixed(block, &trailer_offset, &expected_checksum)) {
    return Status::Corruption("short SSTable data block trailer");
  }
  if (Checksum(data) != expected_checksum) {
    return Status::Corruption("SSTable data block checksum mismatch");
  }
  if (!compressed) return DecodeEntries(data, entry_count, out);
  if (data.size() < sizeof(std::uint32_t)) {
    return Status::Corruption("SSTable compressed block lacks restart count");
  }
  std::size_t count_offset = data.size() - sizeof(std::uint32_t);
  std::uint32_t restart_count = 0;
  if (!ReadFixed(data, &count_offset, &restart_count) || restart_count == 0 ||
      restart_count !=
          (entry_count + kRestartInterval - 1) / kRestartInterval) {
    return Status::Corruption("invalid SSTable restart count");
  }
  const std::uint64_t restart_bytes =
      static_cast<std::uint64_t>(restart_count) * sizeof(std::uint32_t);
  if (restart_bytes > data.size() - sizeof(std::uint32_t)) {
    return Status::Corruption("SSTable restart array exceeds data block");
  }
  const std::size_t entries_end = data.size() - sizeof(std::uint32_t) -
                                  static_cast<std::size_t>(restart_bytes);
  std::vector<std::uint32_t> restarts;
  restarts.reserve(restart_count);
  std::size_t restart_offset = entries_end;
  for (std::uint32_t i = 0; i < restart_count; ++i) {
    std::uint32_t value = 0;
    if (!ReadFixed(data, &restart_offset, &value) || value >= entries_end ||
        (!restarts.empty() && value <= restarts.back())) {
      return Status::Corruption("invalid SSTable restart offset");
    }
    restarts.push_back(value);
  }
  if (restarts.front() != 0) {
    return Status::Corruption("SSTable first restart is not block start");
  }

  out->clear();
  out->reserve(static_cast<std::size_t>(entry_count));
  std::size_t offset = 0;
  std::size_t next_restart = 0;
  std::string previous_key;
  while (offset < entries_end) {
    const bool at_restart = next_restart < restarts.size() &&
                            offset == restarts[next_restart];
    if (next_restart < restarts.size() && offset > restarts[next_restart]) {
      return Status::Corruption("SSTable restart does not align with entry");
    }
    if (at_restart) ++next_restart;
    if (at_restart != (out->size() % kRestartInterval == 0)) {
      return Status::Corruption("SSTable restart interval mismatch");
    }
    std::uint32_t shared = 0;
    std::uint32_t suffix = 0;
    std::uint32_t value_size = 0;
    std::uint8_t type = 0;
    if (!ReadFixed(data, &offset, &shared) ||
        !ReadFixed(data, &offset, &suffix) ||
        !ReadFixed(data, &offset, &value_size) ||
        !ReadFixed(data, &offset, &type)) {
      return Status::Corruption("short compressed SSTable entry header");
    }
    if ((at_restart && shared != 0) || shared > previous_key.size() ||
        static_cast<std::uint64_t>(offset) + suffix + value_size > entries_end) {
      return Status::Corruption("invalid compressed SSTable entry length");
    }
    std::string key = previous_key.substr(0, shared);
    key.append(data.substr(offset, suffix));
    offset += suffix;
    std::string value(data.substr(offset, value_size));
    offset += value_size;
    if (key.empty() || (!out->empty() && out->back().key >= key) ||
        (type != static_cast<std::uint8_t>(RecordType::kPut) &&
         type != static_cast<std::uint8_t>(RecordType::kDelete)) ||
        (type == static_cast<std::uint8_t>(RecordType::kDelete) &&
         !value.empty())) {
      return Status::Corruption("invalid compressed SSTable entry");
    }
    previous_key = key;
    out->push_back(TableEntry{static_cast<RecordType>(type), std::move(key),
                              std::move(value)});
  }
  if (offset != entries_end || next_restart != restarts.size() ||
      out->size() != entry_count) {
    return Status::Corruption("SSTable compressed entry count mismatch");
  }
  return Status::OK();
}

struct CompressedBlockView {
  std::string_view entries;
  std::vector<std::uint32_t> restarts;
};

Status ParseCompressedBlock(std::string_view block, std::uint64_t entry_count,
                            CompressedBlockView* view) {
  if (block.size() < kBlockTrailerSize + sizeof(std::uint32_t)) {
    return Status::Corruption("SSTable compressed block is too small");
  }
  const std::string_view data = block.substr(0, block.size() - kBlockTrailerSize);
  std::size_t checksum_offset = data.size();
  std::uint32_t expected_checksum = 0;
  if (!ReadFixed(block, &checksum_offset, &expected_checksum) ||
      Checksum(data) != expected_checksum) {
    return Status::Corruption("SSTable data block checksum mismatch");
  }
  std::size_t count_offset = data.size() - sizeof(std::uint32_t);
  std::uint32_t restart_count = 0;
  if (!ReadFixed(data, &count_offset, &restart_count) || restart_count == 0 ||
      restart_count !=
          (entry_count + kRestartInterval - 1) / kRestartInterval) {
    return Status::Corruption("invalid SSTable restart count");
  }
  const std::uint64_t restart_bytes =
      static_cast<std::uint64_t>(restart_count) * sizeof(std::uint32_t);
  if (restart_bytes > data.size() - sizeof(std::uint32_t)) {
    return Status::Corruption("SSTable restart array exceeds data block");
  }
  const std::size_t entries_end = data.size() - sizeof(std::uint32_t) -
                                  static_cast<std::size_t>(restart_bytes);
  view->entries = data.substr(0, entries_end);
  view->restarts.clear();
  view->restarts.reserve(restart_count);
  std::size_t offset = entries_end;
  for (std::uint32_t i = 0; i < restart_count; ++i) {
    std::uint32_t restart = 0;
    if (!ReadFixed(data, &offset, &restart) || restart >= entries_end ||
        (!view->restarts.empty() && restart <= view->restarts.back())) {
      return Status::Corruption("invalid SSTable restart offset");
    }
    view->restarts.push_back(restart);
  }
  if (view->restarts.front() != 0) {
    return Status::Corruption("SSTable first restart is not block start");
  }
  return Status::OK();
}

Status DecodeCompressedEntry(std::string_view entries, std::size_t* offset,
                             std::string_view previous_key, bool restart,
                             TableEntry* entry) {
  std::uint32_t shared = 0;
  std::uint32_t suffix = 0;
  std::uint32_t value_size = 0;
  std::uint8_t type = 0;
  if (!ReadFixed(entries, offset, &shared) ||
      !ReadFixed(entries, offset, &suffix) ||
      !ReadFixed(entries, offset, &value_size) ||
      !ReadFixed(entries, offset, &type)) {
    return Status::Corruption("short compressed SSTable entry header");
  }
  if ((restart && shared != 0) || shared > previous_key.size() ||
      static_cast<std::uint64_t>(*offset) + suffix + value_size >
          entries.size()) {
    return Status::Corruption("invalid compressed SSTable entry length");
  }
  std::string key(previous_key.substr(0, shared));
  key.append(entries.substr(*offset, suffix));
  *offset += suffix;
  std::string value(entries.substr(*offset, value_size));
  *offset += value_size;
  if (key.empty() ||
      (type != static_cast<std::uint8_t>(RecordType::kPut) &&
       type != static_cast<std::uint8_t>(RecordType::kDelete)) ||
      (type == static_cast<std::uint8_t>(RecordType::kDelete) &&
       !value.empty())) {
    return Status::Corruption("invalid compressed SSTable entry");
  }
  *entry = TableEntry{static_cast<RecordType>(type), std::move(key),
                      std::move(value)};
  return Status::OK();
}

TableLookup LookupCompressedBlock(std::string_view block,
                                  std::uint64_t entry_count,
                                  std::string_view target) {
  CompressedBlockView view;
  Status status = ParseCompressedBlock(block, entry_count, &view);
  if (!status.ok()) return TableLookup{false, false, "", status};

  const auto restart_key = [&](std::size_t index, std::string* key) {
    std::size_t offset = view.restarts[index];
    TableEntry entry;
    Status decode =
        DecodeCompressedEntry(view.entries, &offset, {}, true, &entry);
    if (decode.ok()) *key = std::move(entry.key);
    return decode;
  };
  std::size_t left = 0;
  std::size_t right = view.restarts.size();
  while (left < right) {
    const std::size_t middle = left + (right - left) / 2;
    std::string key;
    status = restart_key(middle, &key);
    if (!status.ok()) return TableLookup{false, false, "", status};
    if (key <= target) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  const std::size_t restart_index = left == 0 ? 0 : left - 1;
  const std::size_t limit =
      restart_index + 1 < view.restarts.size()
          ? view.restarts[restart_index + 1]
          : view.entries.size();
  std::size_t offset = view.restarts[restart_index];
  std::string previous_key;
  while (offset < limit) {
    TableEntry entry;
    status = DecodeCompressedEntry(view.entries, &offset, previous_key,
                                   previous_key.empty(), &entry);
    if (!status.ok()) return TableLookup{false, false, "", status};
    if (!previous_key.empty() && previous_key >= entry.key) {
      return TableLookup{false, false, "",
                         Status::Corruption(
                             "SSTable keys are not strictly sorted")};
    }
    if (entry.key == target) {
      return TableLookup{true, entry.type == RecordType::kDelete,
                         entry.type == RecordType::kPut ? std::move(entry.value)
                                                        : "",
                         Status::OK()};
    }
    if (entry.key > target) return {};
    previous_key = std::move(entry.key);
  }
  return {};
}

Status DecodeIndex(std::string_view index, std::vector<BlockIndexEntry>* out) {
  std::size_t offset = 0;
  out->clear();
  while (offset < index.size()) {
    std::uint32_t key_size = 0;
    BlockIndexEntry entry;
    if (!ReadFixed(index, &offset, &key_size) ||
        static_cast<std::uint64_t>(offset) + key_size > index.size()) {
      return Status::Corruption("short SSTable index key");
    }
    entry.last_key = std::string(index.substr(offset, key_size));
    offset += key_size;
    if (!ReadFixed(index, &offset, &entry.offset) ||
        !ReadFixed(index, &offset, &entry.size) ||
        !ReadFixed(index, &offset, &entry.entry_count)) {
      return Status::Corruption("short SSTable index entry");
    }
    if (entry.last_key.empty() || entry.size < kBlockTrailerSize ||
        entry.entry_count == 0) {
      return Status::Corruption("invalid SSTable index entry");
    }
    if (!out->empty() && out->back().last_key >= entry.last_key) {
      return Status::Corruption("SSTable index keys are not strictly sorted");
    }
    out->push_back(std::move(entry));
  }
  if (out->empty()) {
    return Status::Corruption("SSTable index is empty");
  }
  return Status::OK();
}

class SSTableIterator final : public Iterator {
 public:
  explicit SSTableIterator(std::vector<std::pair<std::string, std::string>> rows,
                           Status status = Status::OK())
      : rows_(std::move(rows)), status_(std::move(status)) {}

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

  Status status() const override { return status_; }

 private:
  std::vector<std::pair<std::string, std::string>> rows_;
  std::size_t index_ = 0;
  Status status_;
};

}  // namespace

class SSTableEntryIterator final : public InternalIterator {
 public:
  explicit SSTableEntryIterator(std::shared_ptr<const SSTableReader> reader)
      : reader_(std::move(reader)) {}

  bool Valid() const override {
    return status_.ok() && entries_ != nullptr && entry_index_ < entries_->size();
  }

  void SeekToFirst() override {
    status_ = Status::OK();
    block_index_ = 0;
    entry_index_ = 0;
    LoadBlock();
  }

  void Seek(std::string_view target) override {
    status_ = Status::OK();
    if (!reader_->legacy_entries_.empty()) {
      block_index_ = 0;
    } else {
      const auto it = std::lower_bound(
          reader_->index_.begin(), reader_->index_.end(), target,
          [](const BlockIndexEntry& entry, std::string_view key) {
            return entry.last_key < key;
          });
      block_index_ = static_cast<std::size_t>(it - reader_->index_.begin());
    }
    entry_index_ = 0;
    LoadBlock();
    if (!Valid()) return;
    const auto it = std::lower_bound(
        entries_->begin(), entries_->end(), target,
        [](const TableEntry& entry, std::string_view key) {
          return entry.key < key;
        });
    entry_index_ = static_cast<std::size_t>(it - entries_->begin());
    AdvancePastBlockEnd();
  }

  void Next() override {
    if (!Valid()) return;
    ++entry_index_;
    AdvancePastBlockEnd();
  }

  RecordType type() const override { return (*entries_)[entry_index_].type; }
  std::string_view key() const override {
    return Valid() ? (*entries_)[entry_index_].key : std::string_view{};
  }
  std::string_view value() const override {
    return Valid() ? (*entries_)[entry_index_].value : std::string_view{};
  }
  Status status() const override { return status_; }

 private:
  std::size_t BlockCount() const {
    return reader_->legacy_entries_.empty() ? reader_->index_.size() : 1;
  }

  void LoadBlock() {
    entries_.reset();
    if (block_index_ >= BlockCount()) return;
    if (!reader_->legacy_entries_.empty()) {
      entries_ = std::shared_ptr<const std::vector<TableEntry>>(
          reader_, &reader_->legacy_entries_);
      return;
    }
    auto [entries, read_status] = reader_->ReadBlock(block_index_);
    status_ = read_status;
    entries_ = std::move(entries);
  }

  void AdvancePastBlockEnd() {
    while (status_.ok() && entries_ != nullptr &&
           entry_index_ >= entries_->size()) {
      ++block_index_;
      entry_index_ = 0;
      LoadBlock();
    }
  }

  std::shared_ptr<const SSTableReader> reader_;
  std::shared_ptr<const std::vector<TableEntry>> entries_;
  std::size_t block_index_ = 0;
  std::size_t entry_index_ = 0;
  Status status_ = Status::OK();
};

SSTableBuilder::SSTableBuilder(std::filesystem::path path,
                               std::size_t target_block_size)
    : path_(std::move(path)),
      target_block_size_(std::max<std::size_t>(target_block_size, 1)) {}

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

  std::string file;
  std::string data_block;
  std::vector<std::uint32_t> restart_offsets;
  std::vector<BlockIndexEntry> index_entries;
  std::uint64_t block_entry_count = 0;
  std::size_t entries_written = 0;

  const auto finish_block = [&]() {
    if (block_entry_count == 0) {
      return;
    }
    BlockIndexEntry index_entry;
    index_entry.last_key = entries_[entries_written - 1].key;
    index_entry.offset = static_cast<std::uint64_t>(file.size());
    for (std::uint32_t restart : restart_offsets) {
      AppendFixed<std::uint32_t>(data_block, restart);
    }
    AppendFixed<std::uint32_t>(data_block,
                               static_cast<std::uint32_t>(restart_offsets.size()));
    index_entry.size =
        static_cast<std::uint64_t>(data_block.size() + kBlockTrailerSize);
    index_entry.entry_count = block_entry_count;
    file.append(data_block);
    AppendFixed<std::uint32_t>(file, Checksum(data_block));
    index_entries.push_back(std::move(index_entry));
    data_block.clear();
    restart_offsets.clear();
    block_entry_count = 0;
  };

  for (const auto& entry : entries_) {
    std::string encoded;
    const bool restart = block_entry_count % kRestartInterval == 0;
    Status encode_status = EncodeCompressedEntry(
        encoded, entry,
        block_entry_count == 0 ? std::string_view{}
                               : std::string_view(entries_[entries_written - 1].key),
        restart);
    if (!encode_status.ok()) {
      return encode_status;
    }
    if (!data_block.empty() &&
        data_block.size() + encoded.size() +
                (restart_offsets.size() + (restart ? 1 : 0) + 1) *
                    sizeof(std::uint32_t) >
            target_block_size_) {
      finish_block();
      encoded.clear();
      encode_status = EncodeCompressedEntry(encoded, entry, {}, true);
      if (!encode_status.ok()) return encode_status;
    }
    if (block_entry_count % kRestartInterval == 0) {
      restart_offsets.push_back(static_cast<std::uint32_t>(data_block.size()));
    }
    data_block.append(encoded);
    ++block_entry_count;
    ++entries_written;
    if (data_block.size() >= target_block_size_) {
      finish_block();
    }
  }
  finish_block();

  const std::uint64_t index_offset = static_cast<std::uint64_t>(file.size());
  std::string index_block;
  for (const BlockIndexEntry& entry : index_entries) {
    AppendFixed<std::uint32_t>(
        index_block, static_cast<std::uint32_t>(entry.last_key.size()));
    index_block.append(entry.last_key);
    AppendFixed<std::uint64_t>(index_block, entry.offset);
    AppendFixed<std::uint64_t>(index_block, entry.size);
    AppendFixed<std::uint64_t>(index_block, entry.entry_count);
  }
  file.append(index_block);
  const std::uint64_t filter_offset = static_cast<std::uint64_t>(file.size());
  const std::string filter = BuildBloomFilter(entries_);
  file.append(filter);
  std::string footer;
  footer.reserve(kFilterFooterSize);
  AppendFixed<std::uint64_t>(footer,
                             static_cast<std::uint64_t>(entries_.size()));
  AppendFixed<std::uint64_t>(footer, index_offset);
  AppendFixed<std::uint64_t>(footer,
                             static_cast<std::uint64_t>(index_block.size()));
  AppendFixed<std::uint32_t>(footer, Checksum(index_block));
  AppendFixed<std::uint64_t>(footer, filter_offset);
  AppendFixed<std::uint64_t>(footer, static_cast<std::uint64_t>(filter.size()));
  AppendFixed<std::uint32_t>(footer, Checksum(filter));
  footer.append(kMagic);

  std::ofstream stream(path_, std::ios::binary | std::ios::out |
                                  std::ios::trunc);
  if (!stream) {
    return Status::IOError("failed to open SSTable for writing: " +
                           path_.string());
  }

  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
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
      static_cast<std::uint64_t>(file.size() + footer.size());

  finished_ = true;
  return Status::OK();
}

std::pair<std::shared_ptr<SSTableReader>, Status> SSTableReader::Open(
    std::filesystem::path path, std::shared_ptr<BlockCache> block_cache) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return {nullptr, Status::IOError("failed to open SSTable for reading: " +
                                    path.string())};
  }
  const std::streamoff end = stream.tellg();
  if (end < static_cast<std::streamoff>(kLegacyFooterSize)) {
    return {nullptr, Status::Corruption("SSTable is smaller than footer")};
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(end);
  std::array<char, 8> magic{};
  stream.seekg(end - static_cast<std::streamoff>(magic.size()));
  stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (!stream) {
    return {nullptr, Status::IOError("failed to read SSTable footer")};
  }

  if (std::string_view(magic.data(), magic.size()) == kLegacyMagic) {
    std::string file(static_cast<std::size_t>(file_size), '\0');
    stream.clear();
    stream.seekg(0);
    stream.read(file.data(), static_cast<std::streamsize>(file.size()));
    const std::string_view footer(file.data() + file.size() - kLegacyFooterSize,
                                  kLegacyFooterSize);
    std::size_t offset = 0;
    std::uint64_t count = 0;
    std::uint64_t data_size = 0;
    std::uint32_t checksum = 0;
    if (!stream || !ReadFixed(footer, &offset, &count) ||
        !ReadFixed(footer, &offset, &data_size) ||
        !ReadFixed(footer, &offset, &checksum) ||
        data_size + kLegacyFooterSize != file_size) {
      return {nullptr, Status::Corruption("invalid legacy SSTable footer")};
    }
    const std::string_view data(file.data(), static_cast<std::size_t>(data_size));
    if (Checksum(data) != checksum) {
      return {nullptr, Status::Corruption("SSTable data block checksum mismatch")};
    }
    std::vector<TableEntry> entries;
    Status status = DecodeEntries(data, count, &entries);
    if (!status.ok()) {
      return {nullptr, status};
    }
    TableMetadata metadata{0, 0, path, entries.front().key, entries.back().key,
                           count, file_size};
    return {std::shared_ptr<SSTableReader>(new SSTableReader(
                path, {}, std::move(entries), false, {}, 0, 0,
                std::move(metadata),
                std::move(block_cache))),
            Status::OK()};
  }
  const bool has_filter = std::string_view(magic.data(), magic.size()) == kMagic;
  const bool compressed_blocks = has_filter ||
      std::string_view(magic.data(), magic.size()) == kCompressedLegacyMagic;
  if ((!compressed_blocks &&
       std::string_view(magic.data(), magic.size()) != kIndexedLegacyMagic) ||
      file_size < (has_filter ? kFilterFooterSize : kFooterSize)) {
    return {nullptr, Status::Corruption("SSTable footer magic mismatch")};
  }

  const std::size_t footer_size = has_filter ? kFilterFooterSize : kFooterSize;
  std::string footer(footer_size, '\0');
  stream.seekg(end - static_cast<std::streamoff>(footer_size));
  stream.read(footer.data(), static_cast<std::streamsize>(footer.size()));
  std::size_t offset = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t index_offset = 0;
  std::uint64_t index_size = 0;
  std::uint32_t index_checksum = 0;
  if (!stream || !ReadFixed(std::string_view(footer), &offset, &entry_count) ||
      !ReadFixed(std::string_view(footer), &offset, &index_offset) ||
      !ReadFixed(std::string_view(footer), &offset, &index_size) ||
      !ReadFixed(std::string_view(footer), &offset, &index_checksum) ||
      index_offset > file_size - footer_size) {
    return {nullptr, Status::Corruption("SSTable index bounds mismatch")};
  }
  std::uint64_t filter_offset = file_size - footer_size;
  std::uint64_t filter_size = 0;
  std::uint64_t filter_bit_count = 0;
  std::uint32_t filter_probes = 0;
  std::uint32_t filter_checksum = 0;
  std::string filter;
  if (has_filter) {
    if (!ReadFixed(std::string_view(footer), &offset, &filter_offset) ||
        !ReadFixed(std::string_view(footer), &offset, &filter_size) ||
        !ReadFixed(std::string_view(footer), &offset, &filter_checksum) ||
        filter_offset != index_offset + index_size || filter_size < 24 ||
        filter_size != file_size - footer_size - filter_offset) {
      return {nullptr, Status::Corruption("SSTable filter bounds mismatch")};
    }
    filter.resize(static_cast<std::size_t>(filter_size));
    stream.seekg(static_cast<std::streamoff>(filter_offset));
    stream.read(filter.data(), static_cast<std::streamsize>(filter.size()));
    if (!stream) return {nullptr, Status::IOError("failed to read SSTable filter")};
    if (Checksum(filter) != filter_checksum) {
      return {nullptr, Status::Corruption("SSTable filter checksum mismatch")};
    }
    std::size_t filter_cursor = 0;
    std::uint32_t filter_version = 0;
    if (!ReadFixed(std::string_view(filter), &filter_cursor, &filter_version) ||
        !ReadFixed(std::string_view(filter), &filter_cursor, &filter_probes) ||
        !ReadFixed(std::string_view(filter), &filter_cursor, &filter_bit_count) ||
        filter_version != kBloomFilterVersion || filter_probes == 0 ||
        filter_probes > 30 || filter_bit_count < 64 ||
        filter_bit_count % 8 != 0 ||
        filter_cursor + filter_bit_count / 8 != filter.size()) {
      return {nullptr, Status::Corruption("invalid SSTable Bloom filter")};
    }
    filter.erase(0, filter_cursor);
  } else if (index_size != file_size - footer_size - index_offset) {
    return {nullptr, Status::Corruption("SSTable index bounds mismatch")};
  }
  std::string index_data(static_cast<std::size_t>(index_size), '\0');
  stream.seekg(static_cast<std::streamoff>(index_offset));
  stream.read(index_data.data(), static_cast<std::streamsize>(index_data.size()));
  if (!stream) {
    return {nullptr, Status::IOError("failed to read SSTable index")};
  }
  if (Checksum(index_data) != index_checksum) {
    return {nullptr, Status::Corruption("SSTable index checksum mismatch")};
  }
  std::vector<BlockIndexEntry> index;
  Status status = DecodeIndex(index_data, &index);
  if (!status.ok()) {
    return {nullptr, status};
  }
  std::uint64_t expected_offset = 0;
  std::uint64_t indexed_entries = 0;
  for (const auto& block : index) {
    if (block.offset != expected_offset || block.offset > index_offset ||
        block.size > index_offset - block.offset) {
      return {nullptr, Status::Corruption("SSTable data block bounds mismatch")};
    }
    expected_offset += block.size;
    indexed_entries += block.entry_count;
  }
  if (expected_offset != index_offset || indexed_entries != entry_count) {
    return {nullptr, Status::Corruption("SSTable entry count mismatch")};
  }
  TableMetadata metadata{0, 0, path, {}, index.back().last_key, entry_count,
                         file_size};
  auto reader = std::shared_ptr<SSTableReader>(new SSTableReader(
      path, std::move(index), {}, compressed_blocks, std::move(filter),
      filter_bit_count, filter_probes, std::move(metadata),
      std::move(block_cache)));
  auto [first, first_status] = reader->ReadBlock(0);
  if (!first_status.ok()) {
    return {nullptr, first_status};
  }
  reader->metadata_.smallest_key = first->front().key;
  return {std::move(reader), Status::OK()};
}

SSTableReader::~SSTableReader() {
  if (!obsolete_) return;
  std::error_code ec;
  std::filesystem::remove(path_, ec);
}

TableLookup SSTableReader::Lookup(std::string_view key) const {
  if (!legacy_entries_.empty()) {
    const auto it = std::lower_bound(
        legacy_entries_.begin(), legacy_entries_.end(), key,
        [](const TableEntry& entry, std::string_view target) {
          return entry.key < target;
        });
    if (it == legacy_entries_.end() || it->key != key) {
      return {};
    }
    return TableLookup{true, it->type == RecordType::kDelete,
                       it->type == RecordType::kPut ? it->value : "",
                       Status::OK()};
  }
  if (!bloom_filter_.empty() &&
      !BloomMayContain(bloom_filter_, bloom_bit_count_, bloom_probes_, key)) {
    return {};
  }
  const auto block_it = std::lower_bound(
      index_.begin(), index_.end(), key,
      [](const BlockIndexEntry& block, std::string_view target) {
        return block.last_key < target;
      });
  if (block_it == index_.end()) {
    return {};
  }
  const std::size_t block_number =
      static_cast<std::size_t>(block_it - index_.begin());
  if (compressed_blocks_) {
    auto [encoded, status] = ReadEncodedBlock(block_number);
    if (!status.ok()) return TableLookup{false, false, "", status};
    return LookupCompressedBlock(*encoded, block_it->entry_count, key);
  }
  auto [block, status] = ReadBlock(block_number);
  if (!status.ok()) {
    return TableLookup{false, false, "", status};
  }
  const auto it = std::lower_bound(
      block->begin(), block->end(), key,
      [](const TableEntry& entry, std::string_view target) {
        return entry.key < target;
      });

  if (it == block->end() || it->key != key) {
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
  if (!lookup.status.ok()) {
    return {"", lookup.status};
  }
  if (!lookup.found || lookup.deleted) {
    return {"", Status::NotFound("key not found in SSTable")};
  }

  return {lookup.value, Status::OK()};
}

std::unique_ptr<Iterator> SSTableReader::NewIterator() const {
  auto [entries, status] = ReadAll();
  std::vector<std::pair<std::string, std::string>> rows;
  if (!status.ok()) {
    return std::make_unique<SSTableIterator>(std::move(rows), status);
  }
  rows.reserve(entries.size());
  for (const TableEntry& entry : entries) {
    if (entry.type == RecordType::kPut) {
      rows.emplace_back(entry.key, entry.value);
    }
  }
  return std::make_unique<SSTableIterator>(std::move(rows));
}

std::unique_ptr<InternalIterator> SSTableReader::NewEntryIterator() const {
  return std::make_unique<SSTableEntryIterator>(shared_from_this());
}

std::pair<std::vector<TableEntry>, Status> SSTableReader::ReadAll() const {
  if (!legacy_entries_.empty()) {
    return {legacy_entries_, Status::OK()};
  }
  std::vector<TableEntry> result;
  result.reserve(static_cast<std::size_t>(metadata_.entry_count));
  for (std::size_t i = 0; i < index_.size(); ++i) {
    auto [block, status] = ReadBlock(i);
    if (!status.ok()) {
      return {{}, status};
    }
    result.insert(result.end(), block->begin(), block->end());
  }
  return {std::move(result), Status::OK()};
}

const TableMetadata& SSTableReader::metadata() const { return metadata_; }

void SSTableReader::MarkObsolete() { obsolete_ = true; }

SSTableReader::SSTableReader(
    std::filesystem::path path, std::vector<BlockIndexEntry> index,
    std::vector<TableEntry> legacy_entries, bool compressed_blocks,
    std::string bloom_filter, std::uint64_t bloom_bit_count,
    std::uint32_t bloom_probes, TableMetadata metadata,
    std::shared_ptr<BlockCache> block_cache)
    : path_(std::move(path)),
      index_(std::move(index)),
      legacy_entries_(std::move(legacy_entries)),
      compressed_blocks_(compressed_blocks),
      bloom_filter_(std::move(bloom_filter)),
      bloom_bit_count_(bloom_bit_count),
      bloom_probes_(bloom_probes),
      metadata_(std::move(metadata)),
      block_cache_(std::move(block_cache)) {}

std::pair<std::shared_ptr<const std::vector<TableEntry>>, Status>
SSTableReader::ReadBlock(std::size_t block_index) const {
  auto [encoded, read_status] = ReadEncodedBlock(block_index);
  if (!read_status.ok()) return {nullptr, read_status};
  const BlockIndexEntry& descriptor = index_[block_index];
  auto decoded = std::make_shared<std::vector<TableEntry>>();
  Status status = DecodeBlock(*encoded, descriptor.entry_count,
                              compressed_blocks_, decoded.get());
  if (!status.ok()) {
    return {nullptr, status};
  }
  if (decoded->empty() || decoded->back().key != descriptor.last_key ||
      (block_index > 0 &&
       decoded->front().key <= index_[block_index - 1].last_key)) {
    return {nullptr,
            Status::Corruption("SSTable index does not match data block")};
  }
  return {std::move(decoded), Status::OK()};
}

std::pair<std::shared_ptr<const std::string>, Status>
SSTableReader::ReadEncodedBlock(std::size_t block_index) const {
  if (block_index >= index_.size()) {
    return {nullptr, Status::InvalidArgument("SSTable block index out of range")};
  }
  const BlockIndexEntry& descriptor = index_[block_index];
  if (block_cache_) {
    auto cached = block_cache_->Lookup(path_, descriptor.offset,
                                       descriptor.size);
    if (cached) return {std::move(cached), Status::OK()};
  }
  std::ifstream stream(path_, std::ios::binary);
  if (!stream) {
    return {nullptr, Status::IOError("failed to open SSTable block")};
  }
  auto encoded = std::make_shared<std::string>(
      static_cast<std::size_t>(descriptor.size), '\0');
  stream.seekg(static_cast<std::streamoff>(descriptor.offset));
  stream.read(encoded->data(), static_cast<std::streamsize>(encoded->size()));
  if (!stream) {
    return {nullptr, Status::IOError("failed to read SSTable block")};
  }
  if (block_cache_) {
    auto cached = block_cache_->Insert(path_, descriptor.offset,
                                       descriptor.size, encoded);
    return {std::move(cached), Status::OK()};
  }
  return {std::move(encoded), Status::OK()};
}

}  // namespace stratakv
