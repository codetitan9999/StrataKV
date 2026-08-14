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

VersionSet::CompactionSelection VersionSet::PickLevel0Compaction(
    std::size_t level0_input_count) const {
  CompactionSelection selection;
  if (level0_input_count == 0) return selection;

  std::vector<TableMetadata> level0;
  for (const auto& [file_number, table] : tables_) {
    (void)file_number;
    if (table.level == 0) level0.push_back(table);
  }
  std::sort(level0.begin(), level0.end(), [](const auto& left, const auto& right) {
    return left.file_number < right.file_number;
  });
  if (level0.empty()) return selection;
  level0.resize(std::min(level0.size(), level0_input_count));

  selection.smallest_key = level0.front().smallest_key;
  selection.largest_key = level0.front().largest_key;
  for (const auto& table : level0) {
    selection.smallest_key = std::min(selection.smallest_key, table.smallest_key);
    selection.largest_key = std::max(selection.largest_key, table.largest_key);
  }

  // Existing level-1 data is older than every level-0 input and must be
  // merged first so newer level-0 records win.
  for (const auto& [file_number, table] : tables_) {
    (void)file_number;
    if (table.level == 1 && table.smallest_key <= selection.largest_key &&
        selection.smallest_key <= table.largest_key) {
      selection.inputs.push_back(table);
    }
  }
  std::sort(selection.inputs.begin(), selection.inputs.end(),
            [](const auto& left, const auto& right) {
              return left.smallest_key < right.smallest_key;
            });
  selection.inputs.insert(selection.inputs.end(), level0.begin(), level0.end());
  return selection;
}

}  // namespace stratakv
