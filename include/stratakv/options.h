#pragma once

#include <cstddef>
#include <cstdint>

namespace stratakv {

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;

  // Phase 1 will flush the mutable memtable when this threshold is crossed.
  std::uint64_t write_buffer_size = 64 * 1024 * 1024;

  // Kept in the public options now so table cache design has a stable home.
  std::size_t max_open_files = 512;

  // Current implementation flushes the WAL stream; durable fsync is a later
  // platform-specific storage milestone.
  bool fsync_wal = false;
};

struct ReadOptions {
  bool verify_checksums = true;
};

struct WriteOptions {
  bool sync = false;
};

}  // namespace stratakv
