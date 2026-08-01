#pragma once

#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "record.h"
#include "stratakv/block_cache.h"

namespace stratakv {

class BlockCache {
 public:
  explicit BlockCache(std::size_t capacity_bytes);
  std::shared_ptr<const std::vector<TableEntry>> Lookup(
      const std::filesystem::path& path, std::uint64_t offset,
      std::uint64_t size);
  std::shared_ptr<const std::vector<TableEntry>> Insert(
      const std::filesystem::path& path, std::uint64_t offset,
      std::uint64_t size,
      std::shared_ptr<const std::vector<TableEntry>> entries);
  [[nodiscard]] BlockCacheStats stats() const;

 private:
  struct Key {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash { std::size_t operator()(const Key& key) const; };
  struct Entry {
    std::shared_ptr<const std::vector<TableEntry>> entries;
    std::size_t charge = 0;
    std::list<Key>::iterator lru_position;
  };
  Key MakeKey(const std::filesystem::path& path, std::uint64_t offset,
              std::uint64_t size) const;

  const std::size_t capacity_bytes_;
  mutable std::mutex mutex_;
  std::size_t usage_bytes_ = 0;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t evictions_ = 0;
  std::list<Key> lru_;
  std::unordered_map<Key, Entry, KeyHash> entries_;
};

}  // namespace stratakv
