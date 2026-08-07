#pragma once

#include <filesystem>
#include <memory>

#include "stratakv/status.h"

namespace stratakv {

// Narrow interface for durability-critical filesystem operations. Tests can
// inject failures at installation boundaries without replacing file I/O.
class FileSystem {
 public:
  virtual ~FileSystem() = default;

  virtual Status SyncFile(const std::filesystem::path& path) = 0;
  virtual Status Rename(const std::filesystem::path& from,
                        const std::filesystem::path& to) = 0;
  virtual Status SyncDirectory(const std::filesystem::path& path) = 0;
  virtual Status Remove(const std::filesystem::path& path) = 0;
};

std::shared_ptr<FileSystem> DefaultFileSystem();

}  // namespace stratakv
