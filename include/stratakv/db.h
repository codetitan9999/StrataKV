#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "stratakv/iterator.h"
#include "stratakv/options.h"
#include "stratakv/status.h"

namespace stratakv {

class DB {
 public:
  virtual ~DB() = default;

  static std::pair<std::unique_ptr<DB>, Status> Open(const Options& options,
                                                     std::filesystem::path path);

  virtual Status Put(const WriteOptions& options, std::string_view key,
                     std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;

  [[nodiscard]] virtual std::pair<std::string, Status> Get(
      const ReadOptions& options, std::string_view key) const = 0;

  [[nodiscard]] virtual std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& options) const = 0;
};

}  // namespace stratakv
