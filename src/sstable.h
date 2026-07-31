#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "record.h"
#include "stratakv/iterator.h"
#include "stratakv/status.h"

namespace stratakv {

struct TableMetadata {
  std::uint64_t file_number = 0;
  std::filesystem::path file_path;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t entry_count = 0;
  std::uint64_t file_size_bytes = 0;
};

struct TableEntry {
  RecordType type = RecordType::kPut;
  std::string key;
  std::string value;
};

struct TableLookup {
  bool found = false;
  bool deleted = false;
  std::string value;
  Status status = Status::OK();
};

struct TableBlockIndexEntry {
  std::string last_key;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t entry_count = 0;
};

class SSTableBuilder {
 public:
  explicit SSTableBuilder(std::filesystem::path path,
                          std::size_t target_block_size = 4096);

  Status Add(std::string_view key, std::string_view value);
  Status AddDeletion(std::string_view key);
  Status Finish(TableMetadata* metadata);

 private:
  Status AddInternal(RecordType type, std::string_view key,
                     std::string_view value);

  std::filesystem::path path_;
  std::size_t target_block_size_;
  std::vector<TableEntry> entries_;
  std::string last_key_;
  bool has_last_key_ = false;
  bool finished_ = false;
};

class SSTableReader {
 public:
  static std::pair<std::unique_ptr<SSTableReader>, Status> Open(
      std::filesystem::path path, std::size_t block_cache_capacity = 8 << 20);

  [[nodiscard]] TableLookup Lookup(std::string_view key) const;
  [[nodiscard]] std::pair<std::string, Status> Get(std::string_view key) const;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;
  [[nodiscard]] std::pair<std::vector<TableEntry>, Status> ReadAll() const;
  [[nodiscard]] const TableMetadata& metadata() const;

 private:
  SSTableReader(std::filesystem::path path,
                std::vector<TableBlockIndexEntry> index,
                std::vector<TableEntry> legacy_entries,
                TableMetadata metadata, std::size_t block_cache_capacity);
  std::pair<std::shared_ptr<const std::vector<TableEntry>>, Status> ReadBlock(
      std::size_t block_index) const;

  std::filesystem::path path_;
  std::vector<TableBlockIndexEntry> index_;
  std::vector<TableEntry> legacy_entries_;
  TableMetadata metadata_;
  std::size_t block_cache_capacity_;
  mutable std::size_t block_cache_usage_ = 0;
  mutable std::list<std::size_t> lru_;
  struct CachedBlock {
    std::shared_ptr<const std::vector<TableEntry>> entries;
    std::size_t charge = 0;
    std::list<std::size_t>::iterator lru_position;
  };
  mutable std::unordered_map<std::size_t, CachedBlock> block_cache_;
  mutable std::mutex cache_mutex_;
};

}  // namespace stratakv
