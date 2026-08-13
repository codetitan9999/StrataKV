#include "version_set.h"

#include <algorithm>
#include <utility>

namespace stratakv {

Status VersionSet::AddTable(TableMetadata metadata) {
  if (metadata.file_number == 0) {
    return Status::InvalidArgument("version table file number must be nonzero");
  }
  if (metadata.level > 0) {
    for (const auto& [file_number, table] : tables_) {
      if (file_number == metadata.file_number || table.level != metadata.level) {
        continue;
      }
      if (metadata.smallest_key <= table.largest_key &&
          table.smallest_key <= metadata.largest_key) {
        return Status::Corruption("overlapping tables in sorted level " +
                                  std::to_string(metadata.level));
      }
    }
  }
  tables_[metadata.file_number] = std::move(metadata);
  return Status::OK();
}

void VersionSet::DeleteTable(std::uint64_t file_number) {
  tables_.erase(file_number);
}

std::vector<TableMetadata> VersionSet::TablesInReadOrder() const {
  std::vector<TableMetadata> result;
  result.reserve(tables_.size());
  for (const auto& [file_number, table] : tables_) {
    (void)file_number;
    result.push_back(table);
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left.level != right.level) return left.level < right.level;
    if (left.level == 0) return left.file_number > right.file_number;
    return left.smallest_key < right.smallest_key;
  });
  return result;
}

std::vector<TableMetadata> VersionSet::TablesInManifestOrder() const {
  std::vector<TableMetadata> result;
  result.reserve(tables_.size());
  for (const auto& [file_number, table] : tables_) {
    (void)file_number;
    result.push_back(table);
  }
  return result;
}

std::size_t VersionSet::LevelTableCount(std::uint32_t level) const {
  return static_cast<std::size_t>(std::count_if(
      tables_.begin(), tables_.end(), [level](const auto& entry) {
        return entry.second.level == level;
      }));
}

}  // namespace stratakv
