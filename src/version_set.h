#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

class VersionSet {
 public:
  struct CompactionSelection {
    std::vector<TableMetadata> inputs;
    std::string smallest_key;
    std::string largest_key;
    std::uint32_t output_level = 0;
    bool drop_tombstones = false;
  };

  Status AddTable(TableMetadata metadata);
  void DeleteTable(std::uint64_t file_number);

  [[nodiscard]] std::vector<TableMetadata> TablesInReadOrder() const;
  [[nodiscard]] std::vector<TableMetadata> TablesInManifestOrder() const;
  [[nodiscard]] std::size_t LevelTableCount(std::uint32_t level) const;
  [[nodiscard]] std::uint64_t LevelSizeBytes(std::uint32_t level) const;
  [[nodiscard]] std::uint32_t PickCompactionLevel(
      std::uint64_t level1_target_bytes, std::uint32_t size_multiplier,
      std::uint32_t max_output_level) const;
  [[nodiscard]] CompactionSelection PickLevel0Compaction(
      std::size_t level0_input_count) const;
  [[nodiscard]] CompactionSelection PickLevelCompaction(
      std::uint32_t level) const;

 private:
  std::map<std::uint64_t, TableMetadata> tables_;
};

}  // namespace stratakv
