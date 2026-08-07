#include "stratakv/file_system.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <system_error>

namespace stratakv {
namespace {

Status ErrnoStatus(const char* operation, const std::filesystem::path& path) {
  return Status::IOError(std::string(operation) + " " + path.string() +
                         ": " + std::strerror(errno));
}

class PosixFileSystem final : public FileSystem {
 public:
  Status SyncFile(const std::filesystem::path& path) override {
    return SyncPath(path, /*directory=*/false);
  }

  Status Rename(const std::filesystem::path& from,
                const std::filesystem::path& to) override {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) {
      return Status::IOError("failed to rename " + from.string() + " to " +
                             to.string() + ": " + ec.message());
    }
    return Status::OK();
  }

  Status SyncDirectory(const std::filesystem::path& path) override {
    return SyncPath(path, /*directory=*/true);
  }

  Status Remove(const std::filesystem::path& path) override {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      return Status::IOError("failed to remove " + path.string() + ": " +
                             ec.message());
    }
    return Status::OK();
  }

 private:
  Status SyncPath(const std::filesystem::path& path, bool directory) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    if (directory) {
      flags |= O_DIRECTORY;
    }
#else
    (void)directory;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
      return ErrnoStatus("failed to open for sync", path);
    }
    if (::fsync(fd) != 0) {
      const Status status = ErrnoStatus("failed to sync", path);
      ::close(fd);
      return status;
    }
    if (::close(fd) != 0) {
      return ErrnoStatus("failed to close after sync", path);
    }
    return Status::OK();
  }
};

}  // namespace

std::shared_ptr<FileSystem> DefaultFileSystem() {
  static std::shared_ptr<FileSystem> instance =
      std::make_shared<PosixFileSystem>();
  return instance;
}

}  // namespace stratakv
