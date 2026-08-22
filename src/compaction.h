#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "record.h"
#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

struct CompactionInput {
  std::vector<const SSTableReader*> tables;
  std::size_t max_output_file_size = 0;
  bool drop_tombstones = true;
};

struct CompactionOutputSink {
  std::function<Status(const TableEntry& entry)> add;
  std::function<Status()> finish;
};

class CompactionJob {
 public:
  explicit CompactionJob(std::filesystem::path db_path);

  Status Run(const CompactionInput& input, const CompactionOutputSink& sink);

 private:
  std::filesystem::path db_path_;
};

}  // namespace stratakv
