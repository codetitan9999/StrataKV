#include "compaction.h"

#include <utility>

namespace stratakv {

CompactionJob::CompactionJob(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {}

Status CompactionJob::Run(const CompactionInput& input,
                          CompactionOutput* output) {
  (void)input;
  (void)output;
  return Status::NotSupported("compaction starts after SSTable read/write");
}

}  // namespace stratakv
