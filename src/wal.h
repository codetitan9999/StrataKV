#pragma once

#include <filesystem>
#include <fstream>
#include <functional>

#include "record.h"
#include "stratakv/status.h"

namespace stratakv {

class WalWriter {
 public:
  explicit WalWriter(std::filesystem::path path);
  ~WalWriter();

  Status Open(bool append);
  Status Append(const LogRecord& record);
  Status Sync();

 private:
  std::filesystem::path path_;
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
