#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

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

  // WAL I/O lives behind the same fault-injection boundary as durability
  // operations. The default implementations use descriptor-based positional
  // I/O and may be selectively overridden by tests.
  virtual Status OpenWritableFile(const std::filesystem::path& path,
                                  bool append);
  virtual Status AppendFile(const std::filesystem::path& path,
                            std::string_view data);
  virtual Status ReadFile(const std::filesystem::path& path,
                          std::uint64_t offset, std::size_t size,
                          std::string* data);
};

std::shared_ptr<FileSystem> DefaultFileSystem();

}  // namespace stratakv
