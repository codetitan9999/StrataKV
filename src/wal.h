#pragma once

#include <filesystem>
#include <fstream>
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
      std::shared_ptr<FileSystem> file_system = DefaultFileSystem());
  ~WalWriter();

  Status Open(bool append);
  Status Append(const LogRecord& record);
  Status Sync();
  Status Close();

 private:
  std::filesystem::path path_;
  std::shared_ptr<FileSystem> file_system_;
  std::ofstream stream_;
};

class WalReader {
 public:
  explicit WalReader(std::filesystem::path path);

  Status Replay(const std::function<Status(const LogRecord&)>& apply) const;

 private:
  std::filesystem::path path_;
};

}  // namespace stratakv
