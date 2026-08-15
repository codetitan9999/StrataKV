#include "compaction.h"

#include <map>
#include <utility>

namespace stratakv {

CompactionJob::CompactionJob(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {}

Status CompactionJob::Run(const CompactionInput& input,
                          CompactionOutput* output) {
  if (output == nullptr) {
    return Status::InvalidArgument("compaction output must not be null");
  }

  std::map<std::string, TableEntry> latest_entries;
  for (const SSTableReader* table : input.tables) {
    if (table == nullptr) {
      return Status::InvalidArgument("compaction input table must not be null");
    }

    auto [entries, read_status] = table->ReadAll();
    if (!read_status.ok()) {
      return read_status;
    }
    for (const TableEntry& entry : entries) {
      if (entry.type == RecordType::kDelete) {
        latest_entries[entry.key] = entry;
      } else {
        latest_entries[entry.key] = entry;
      }
    }
  }

  output->files.clear();
  std::size_t output_bytes = 0;
  for (const auto& [key, entry] : latest_entries) {
    if (entry.type == RecordType::kDelete && input.drop_tombstones) continue;
    const std::size_t entry_bytes = key.size() + entry.value.size() + 16;
    if (!output->files.empty() && !output->files.back().empty() &&
        input.max_output_file_size > 0 &&
        output_bytes + entry_bytes > input.max_output_file_size) {
      output->files.emplace_back();
      output_bytes = 0;
    }
    if (output->files.empty()) output->files.emplace_back();
    output->files.back().push_back(entry);
    output_bytes += entry_bytes;
  }

  return Status::OK();
}

}  // namespace stratakv
