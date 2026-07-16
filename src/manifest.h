#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>

#include "sstable.h"
#include "stratakv/status.h"

namespace stratakv {

enum class ManifestEditType {
  kTableAdded,
  kTableDeleted,
};

struct ManifestEdit {
  ManifestEditType type = ManifestEditType::kTableAdded;
  TableMetadata table;
  std::uint64_t file_number = 0;
};

class ManifestWriter {
 public:
  explicit ManifestWriter(std::filesystem::path path);

  Status Open(bool append);
  Status AppendTable(const TableMetadata& metadata);
  Status DeleteTable(std::uint64_t file_number);
  Status Sync();

 private:
  std::filesystem::path path_;
  std::ofstream stream_;
};

class ManifestReader {
 public:
  explicit ManifestReader(std::filesystem::path path);

  Status Replay(const std::function<Status(const ManifestEdit&)>& apply) const;

 private:
  std::filesystem::path path_;
};

}  // namespace stratakv
