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

  std::map<std::string, std::string> live_values;
  for (const SSTableReader* table : input.tables) {
    if (table == nullptr) {
      return Status::InvalidArgument("compaction input table must not be null");
    }

    for (const TableEntry& entry : table->entries()) {
      if (entry.type == RecordType::kDelete) {
        live_values.erase(entry.key);
      } else {
        live_values[entry.key] = entry.value;
      }
    }
  }

  output->entries.clear();
  output->entries.reserve(live_values.size());
  for (const auto& [key, value] : live_values) {
    output->entries.push_back(TableEntry{RecordType::kPut, key, value});
  }

  return Status::OK();
}

}  // namespace stratakv
