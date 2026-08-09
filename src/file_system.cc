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

Status FileSystem::OpenWritableFile(const std::filesystem::path& path,
                                    bool append) {
  const int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return ErrnoStatus("failed to open for writing", path);
  }
  if (::close(fd) != 0) {
    return ErrnoStatus("failed to close after open", path);
  }
  return Status::OK();
}

Status FileSystem::AppendFile(const std::filesystem::path& path,
                              std::string_view data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  if (fd < 0) {
    return ErrnoStatus("failed to open for append", path);
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t result =
        ::write(fd, data.data() + written, data.size() - written);
    if (result < 0) {
      const Status status = ErrnoStatus("failed to append", path);
      ::close(fd);
      return status;
    }
    written += static_cast<std::size_t>(result);
  }
  if (::close(fd) != 0) {
    return ErrnoStatus("failed to close after append", path);
  }
  return Status::OK();
}

Status FileSystem::ReadFile(const std::filesystem::path& path,
                            std::uint64_t offset, std::size_t size,
                            std::string* data) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return ErrnoStatus("failed to open for reading", path);
  }
  data->assign(size, '\0');
  std::size_t read = 0;
  while (read < size) {
    const ssize_t result = ::pread(fd, data->data() + read, size - read,
                                   static_cast<off_t>(offset + read));
    if (result < 0) {
      const Status status = ErrnoStatus("failed to read", path);
      ::close(fd);
      return status;
    }
    if (result == 0) {
      break;
    }
    read += static_cast<std::size_t>(result);
  }
  data->resize(read);
  if (::close(fd) != 0) {
    return ErrnoStatus("failed to close after read", path);
  }
  return Status::OK();
}

std::shared_ptr<FileSystem> DefaultFileSystem() {
  static std::shared_ptr<FileSystem> instance =
      std::make_shared<PosixFileSystem>();
  return instance;
}

}  // namespace stratakv
