#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>

namespace stratakv {

class FileSystem;

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;

  // Phase 1 will flush the mutable memtable when this threshold is crossed.
  std::uint64_t write_buffer_size = 64 * 1024 * 1024;

  // Kept in the public options now so table cache design has a stable home.
  std::size_t max_open_files = 512;

  // Maximum decoded SSTable data-block bytes retained across the database.
  // A value of 0 disables block caching while preserving lazy reads.
  std::size_t block_cache_size = 8 * 1024 * 1024;

  // Number of flushed SSTables that triggers level-0 compaction.
  // A value of 0 disables automatic compaction.
  std::size_t level0_compaction_trigger = 4;

  // Approximate maximum logical bytes emitted into one compaction output.
  // A value of 0 emits a single output table.
  std::size_t max_compaction_output_file_size = 2 * 1024 * 1024;

  // Durably sync the WAL file after each write.
  bool fsync_wal = false;

  // Maximum encoded WAL record payload accepted for writes and recovery.
  // Recovery validates this bound before allocating the payload buffer.
  std::size_t max_wal_record_size = 64 * 1024 * 1024;

  // Optional filesystem implementation for durability operations and fault
  // injection. A null pointer selects the platform default.
  std::shared_ptr<FileSystem> file_system;
};

struct ReadOptions {
  bool verify_checksums = true;

  // Inclusive lower and exclusive upper bounds for iterators. Point reads
  // ignore these fields. An empty optional means that side is unbounded.
  std::optional<std::string> lower_bound;
  std::optional<std::string> upper_bound;

  // When set, iterators return only keys beginning with this prefix. Prefix
  // filtering is intersected with explicit bounds.
  std::optional<std::string> prefix;
};

struct WriteOptions {
  bool sync = false;
};

}  // namespace stratakv
