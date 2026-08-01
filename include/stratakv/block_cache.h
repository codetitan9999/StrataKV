#pragma once

#include <cstddef>
#include <cstdint>

namespace stratakv {

struct BlockCacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::size_t usage_bytes = 0;
  std::size_t capacity_bytes = 0;
};

}  // namespace stratakv
