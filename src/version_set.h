#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

class VersionSet {
 public:
  Status AddTable(TableMetadata metadata);
  void DeleteTable(std::uint64_t file_number);

  [[nodiscard]] std::vector<TableMetadata> TablesInReadOrder() const;
  [[nodiscard]] std::vector<TableMetadata> TablesInManifestOrder() const;
  [[nodiscard]] std::size_t LevelTableCount(std::uint32_t level) const;

 private:
  std::map<std::uint64_t, TableMetadata> tables_;
};

}  // namespace stratakv
