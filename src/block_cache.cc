#include "block_cache.h"

#include <functional>

namespace stratakv {

BlockCache::BlockCache(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

std::size_t BlockCache::KeyHash::operator()(const Key& key) const {
  std::size_t hash = std::hash<std::string>{}(key.path);
  hash ^= std::hash<std::uint64_t>{}(key.offset) + 0x9e3779b9 + (hash << 6) +
          (hash >> 2);
  hash ^= std::hash<std::uint64_t>{}(key.size) + 0x9e3779b9 + (hash << 6) +
          (hash >> 2);
  return hash;
}

BlockCache::Key BlockCache::MakeKey(const std::filesystem::path& path,
                                    std::uint64_t offset,
                                    std::uint64_t size) const {
  return Key{path.lexically_normal().string(), offset, size};
}

std::shared_ptr<const std::vector<TableEntry>> BlockCache::Lookup(
    const std::filesystem::path& path, std::uint64_t offset,
    std::uint64_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(MakeKey(path, offset, size));
  if (found == entries_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  lru_.splice(lru_.begin(), lru_, found->second.lru_position);
  return found->second.entries;
}

std::shared_ptr<const std::vector<TableEntry>> BlockCache::Insert(
    const std::filesystem::path& path, std::uint64_t offset,
    std::uint64_t size,
    std::shared_ptr<const std::vector<TableEntry>> entries) {
  if (capacity_bytes_ == 0 || size > capacity_bytes_) return entries;
  const Key key = MakeKey(path, offset, size);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    lru_.splice(lru_.begin(), lru_, existing->second.lru_position);
    return existing->second.entries;
  }
  while (!lru_.empty() && usage_bytes_ + size > capacity_bytes_) {
    const Key& victim = lru_.back();
    usage_bytes_ -= entries_.at(victim).charge;
    entries_.erase(victim);
    lru_.pop_back();
    ++evictions_;
  }
  lru_.push_front(key);
  entries_.emplace(key, Entry{entries, static_cast<std::size_t>(size),
                              lru_.begin()});
  usage_bytes_ += static_cast<std::size_t>(size);
  return entries;
}

BlockCacheStats BlockCache::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return BlockCacheStats{hits_, misses_, evictions_, usage_bytes_,
                         capacity_bytes_};
}

}  // namespace stratakv
