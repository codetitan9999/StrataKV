#include "stratakv/db.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "memtable.h"
#include "record.h"
#include "wal.h"

namespace stratakv {
namespace {

Status EnsureDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return Status::IOError("failed to create directory " + path.string() +
                           ": " + ec.message());
  }
  return Status::OK();
}

Status ValidateKey(std::string_view key) {
  if (key.empty()) {
    return Status::InvalidArgument("keys must not be empty");
  }
  return Status::OK();
}

}  // namespace

class DBImpl final : public DB {
 public:
  DBImpl(Options options, std::filesystem::path path)
      : options_(options), db_path_(std::move(path)) {}

  Status OpenInternal() {
    std::error_code ec;
    const bool exists = std::filesystem::exists(db_path_, ec);
    if (ec) {
      return Status::IOError("failed to inspect database path " +
                             db_path_.string() + ": " + ec.message());
    }

    if (exists && options_.error_if_exists) {
      return Status::InvalidArgument("database already exists: " +
                                     db_path_.string());
    }

    if (!exists && !options_.create_if_missing) {
      return Status::NotFound("database does not exist: " + db_path_.string());
    }

    Status dir_status = EnsureDirectory(db_path_);
    if (!dir_status.ok()) {
      return dir_status;
    }

    wal_dir_ = db_path_ / "wal";
    table_dir_ = db_path_ / "sst";

    dir_status = EnsureDirectory(wal_dir_);
    if (!dir_status.ok()) {
      return dir_status;
    }

    dir_status = EnsureDirectory(table_dir_);
    if (!dir_status.ok()) {
      return dir_status;
    }

    wal_path_ = wal_dir_ / "current.log";
    if (std::filesystem::exists(wal_path_, ec)) {
      Status recover_status = Recover();
      if (!recover_status.ok()) {
        return recover_status;
      }
    } else if (ec) {
      return Status::IOError("failed to inspect WAL path " +
                             wal_path_.string() + ": " + ec.message());
    }

    wal_ = std::make_unique<WalWriter>(wal_path_);
    return wal_->Open(/*append=*/true);
  }

  Status Put(const WriteOptions& write_options, std::string_view key,
             std::string_view value) override {
    Status key_status = ValidateKey(key);
    if (!key_status.ok()) {
      return key_status;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const std::uint64_t sequence = ++last_sequence_;
    LogRecord record{RecordType::kPut, sequence, std::string(key),
                     std::string(value)};

    Status status = wal_->Append(record);
    if (!status.ok()) {
      return status;
    }

    if (write_options.sync || options_.fsync_wal) {
      status = wal_->Sync();
      if (!status.ok()) {
        return status;
      }
    }

    return memtable_.Apply(record);
  }

  Status Delete(const WriteOptions& write_options,
                std::string_view key) override {
    Status key_status = ValidateKey(key);
    if (!key_status.ok()) {
      return key_status;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const std::uint64_t sequence = ++last_sequence_;
    LogRecord record{RecordType::kDelete, sequence, std::string(key), ""};

    Status status = wal_->Append(record);
    if (!status.ok()) {
      return status;
    }

    if (write_options.sync || options_.fsync_wal) {
      status = wal_->Sync();
      if (!status.ok()) {
        return status;
      }
    }

    return memtable_.Apply(record);
  }

  std::pair<std::string, Status> Get(const ReadOptions& read_options,
                                     std::string_view key) const override {
    (void)read_options;

    Status key_status = ValidateKey(key);
    if (!key_status.ok()) {
      return {"", key_status};
    }

    std::lock_guard<std::mutex> lock(mu_);
    return memtable_.Get(key);
  }

  std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& read_options) const override {
    (void)read_options;

    std::lock_guard<std::mutex> lock(mu_);
    return memtable_.NewIterator();
  }

 private:
  Status Recover() {
    WalReader reader(wal_path_);
    return reader.Replay([this](const LogRecord& record) {
      last_sequence_ = std::max(last_sequence_, record.sequence);
      return memtable_.Apply(record);
    });
  }

  Options options_;
  std::filesystem::path db_path_;
  std::filesystem::path wal_dir_;
  std::filesystem::path table_dir_;
  std::filesystem::path wal_path_;

  mutable std::mutex mu_;
  MemTable memtable_;
  std::uint64_t last_sequence_ = 0;
  std::unique_ptr<WalWriter> wal_;
};

std::pair<std::unique_ptr<DB>, Status> DB::Open(const Options& options,
                                                std::filesystem::path path) {
  auto impl = std::make_unique<DBImpl>(options, std::move(path));
  Status status = impl->OpenInternal();
  if (!status.ok()) {
    return {nullptr, status};
  }

  std::unique_ptr<DB> db = std::move(impl);
  return {std::move(db), Status::OK()};
}

}  // namespace stratakv
