#pragma once

#include <filesystem>
#include <fstream>
#include <functional>

#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

class ManifestWriter {
 public:
  explicit ManifestWriter(std::filesystem::path path);

  Status Open(bool append);
  Status AppendTable(const TableMetadata& metadata);
  Status Sync();

 private:
  std::filesystem::path path_;
  std::ofstream stream_;
};

class ManifestReader {
 public:
  explicit ManifestReader(std::filesystem::path path);

  Status Replay(
      const std::function<Status(const TableMetadata&)>& apply_table) const;

 private:
  std::filesystem::path path_;
};

}  // namespace stratakv
