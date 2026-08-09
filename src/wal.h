#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "record.h"
#include "stratakv/file_system.h"
#include "stratakv/status.h"

namespace stratakv {

class WalWriter {
 public:
  explicit WalWriter(
      std::filesystem::path path,
      std::shared_ptr<FileSystem> file_system = DefaultFileSystem(),
      std::size_t max_record_size = 64 * 1024 * 1024);
  ~WalWriter();

  Status Open(bool append);
  Status Append(const LogRecord& record);
  Status Sync();
  Status Close();

 private:
  std::filesystem::path path_;
  std::shared_ptr<FileSystem> file_system_;
  std::size_t max_record_size_;
  bool open_ = false;
};

class WalReader {
 public:
  explicit WalReader(
      std::filesystem::path path,
      std::shared_ptr<FileSystem> file_system = DefaultFileSystem(),
      std::size_t max_record_size = 64 * 1024 * 1024);

  Status Replay(const std::function<Status(const LogRecord&)>& apply) const;

 private:
  std::filesystem::path path_;
  std::shared_ptr<FileSystem> file_system_;
  std::size_t max_record_size_;
};

}  // namespace stratakv
