#pragma once

#include <filesystem>
#include <vector>

#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

struct CompactionInput {
  std::vector<TableMetadata> tables;
};

struct CompactionOutput {
  std::vector<TableMetadata> tables;
};

class CompactionJob {
 public:
  explicit CompactionJob(std::filesystem::path db_path);

  Status Run(const CompactionInput& input, CompactionOutput* output);

 private:
  std::filesystem::path db_path_;
};

}  // namespace stratakv
