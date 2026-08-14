#pragma once

#include <filesystem>
#include <vector>

#include "record.h"
#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

struct CompactionInput {
  std::vector<const SSTableReader*> tables;
  std::size_t max_output_file_size = 0;
};

struct CompactionOutput {
  std::vector<std::vector<TableEntry>> files;
};

class CompactionJob {
 public:
  explicit CompactionJob(std::filesystem::path db_path);

  Status Run(const CompactionInput& input, CompactionOutput* output);

 private:
  std::filesystem::path db_path_;
};

}  // namespace stratakv
