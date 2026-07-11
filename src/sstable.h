#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/iterator.h"
#include "stratakv/status.h"

namespace stratakv {

struct TableMetadata {
  std::uint64_t file_number = 0;
  std::filesystem::path file_path;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t entry_count = 0;
  std::uint64_t file_size_bytes = 0;
};

class SSTableBuilder {
 public:
  explicit SSTableBuilder(std::filesystem::path path);

  Status Add(std::string_view key, std::string_view value);
  Status Finish(TableMetadata* metadata);

 private:
  std::filesystem::path path_;
  std::vector<std::pair<std::string, std::string>> entries_;
  std::string last_key_;
  bool has_last_key_ = false;
  bool finished_ = false;
};

class SSTableReader {
 public:
  static std::pair<std::unique_ptr<SSTableReader>, Status> Open(
      std::filesystem::path path);

  [[nodiscard]] std::pair<std::string, Status> Get(std::string_view key) const;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;
  [[nodiscard]] const TableMetadata& metadata() const;

 private:
  SSTableReader(std::filesystem::path path,
                std::vector<std::pair<std::string, std::string>> entries,
                TableMetadata metadata);

  std::filesystem::path path_;
  std::vector<std::pair<std::string, std::string>> entries_;
  TableMetadata metadata_;
};

}  // namespace stratakv
