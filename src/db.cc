#include "stratakv/db.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "manifest.h"
#include "memtable.h"
#include "record.h"
#include "sstable.h"
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

std::filesystem::path TablePath(const std::filesystem::path& table_dir,
                                std::uint64_t file_number) {
  std::ostringstream name;
  name << std::setw(6) << std::setfill('0') << file_number << ".sst";
  return table_dir / name.str();
}

class MaterializedIterator final : public Iterator {
 public:
  explicit MaterializedIterator(
      std::vector<std::pair<std::string, std::string>> rows)
      : rows_(std::move(rows)) {}

  bool Valid() const override { return index_ < rows_.size(); }

  void SeekToFirst() override { index_ = 0; }

  void Seek(std::string_view target) override {
    const auto it = std::lower_bound(
        rows_.begin(), rows_.end(), target,
        [](const auto& row, std::string_view key) { return row.first < key; });
    index_ = static_cast<std::size_t>(it - rows_.begin());
  }

  void Next() override {
    if (Valid()) {
      ++index_;
    }
  }

  std::string_view key() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].first;
  }

  std::string_view value() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].second;
  }

  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::pair<std::string, std::string>> rows_;
  std::size_t index_ = 0;
};

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
    manifest_path_ = db_path_ / "MANIFEST";

    dir_status = EnsureDirectory(wal_dir_);
    if (!dir_status.ok()) {
      return dir_status;
    }

    dir_status = EnsureDirectory(table_dir_);
    if (!dir_status.ok()) {
      return dir_status;
    }

    Status table_status = LoadTables();
    if (!table_status.ok()) {
      return table_status;
    }

    manifest_ = std::make_unique<ManifestWriter>(manifest_path_);
    Status manifest_status = manifest_->Open(/*append=*/true);
    if (!manifest_status.ok()) {
      return manifest_status;
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

    status = memtable_.Apply(record);
    if (!status.ok()) {
      return status;
    }

    return MaybeFlushMemTable();
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

    status = memtable_.Apply(record);
    if (!status.ok()) {
      return status;
    }

    return MaybeFlushMemTable();
  }

  std::pair<std::string, Status> Get(const ReadOptions& read_options,
                                     std::string_view key) const override {
    (void)read_options;

    Status key_status = ValidateKey(key);
    if (!key_status.ok()) {
      return {"", key_status};
    }

    std::lock_guard<std::mutex> lock(mu_);
    const MemTableLookup memtable_lookup = memtable_.Lookup(key);
    if (memtable_lookup.found) {
      if (memtable_lookup.deleted) {
        return {"", Status::NotFound("key not found")};
      }
      return {memtable_lookup.value, Status::OK()};
    }

    for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {
      const TableMetadata& metadata = (*it)->metadata();
      if (key < metadata.smallest_key || key > metadata.largest_key) {
        continue;
      }

      const TableLookup table_lookup = (*it)->Lookup(key);
      if (!table_lookup.found) {
        continue;
      }
      if (table_lookup.deleted) {
        return {"", Status::NotFound("key not found")};
      }
      return {table_lookup.value, Status::OK()};
    }

    return {"", Status::NotFound("key not found")};
  }

  std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& read_options) const override {
    (void)read_options;

    std::lock_guard<std::mutex> lock(mu_);

    std::map<std::string, std::string> visible;
    for (const auto& table : tables_) {
      for (const TableEntry& entry : table->entries()) {
        if (entry.type == RecordType::kDelete) {
          visible.erase(entry.key);
        } else {
          visible[entry.key] = entry.value;
        }
      }
    }

    for (const MemTableEntry& entry : memtable_.Snapshot()) {
      if (entry.type == RecordType::kDelete) {
        visible.erase(entry.key);
      } else {
        visible[entry.key] = entry.value;
      }
    }

    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(visible.size());
    for (const auto& [visible_key, visible_value] : visible) {
      rows.emplace_back(visible_key, visible_value);
    }

    return std::make_unique<MaterializedIterator>(std::move(rows));
  }

 private:
  Status LoadTables() {
    ManifestReader reader(manifest_path_);
    return reader.Replay([this](const TableMetadata& manifest_metadata) {
      const std::filesystem::path path =
          TablePath(table_dir_, manifest_metadata.file_number);
      auto [table_reader, status] = SSTableReader::Open(path);
      if (!status.ok()) {
        return status;
      }

      const TableMetadata& table_metadata = table_reader->metadata();
      if (table_metadata.entry_count != manifest_metadata.entry_count ||
          table_metadata.file_size_bytes != manifest_metadata.file_size_bytes ||
          table_metadata.smallest_key != manifest_metadata.smallest_key ||
          table_metadata.largest_key != manifest_metadata.largest_key) {
        return Status::Corruption("manifest metadata does not match SSTable: " +
                                  path.string());
      }

      next_file_number_ =
          std::max(next_file_number_, manifest_metadata.file_number + 1);
      tables_.push_back(std::move(table_reader));
      return Status::OK();
    });
  }

  Status Recover() {
    WalReader reader(wal_path_);
    return reader.Replay([this](const LogRecord& record) {
      last_sequence_ = std::max(last_sequence_, record.sequence);
      return memtable_.Apply(record);
    });
  }

  Status MaybeFlushMemTable() {
    if (memtable_.empty() ||
        memtable_.ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }

    return FlushMemTable();
  }

  Status FlushMemTable() {
    const std::vector<MemTableEntry> snapshot = memtable_.Snapshot();
    if (snapshot.empty()) {
      return Status::OK();
    }

    const std::uint64_t file_number = next_file_number_++;
    const std::filesystem::path final_path = TablePath(table_dir_, file_number);
    const std::filesystem::path temporary_path = final_path.string() + ".tmp";

    SSTableBuilder builder(temporary_path);
    for (const MemTableEntry& entry : snapshot) {
      Status add_status =
          entry.type == RecordType::kDelete
              ? builder.AddDeletion(entry.key)
              : builder.Add(entry.key, entry.value);
      if (!add_status.ok()) {
        return add_status;
      }
    }

    TableMetadata metadata;
    Status finish_status = builder.Finish(&metadata);
    if (!finish_status.ok()) {
      return finish_status;
    }
    metadata.file_number = file_number;
    metadata.file_path = final_path;

    std::error_code ec;
    std::filesystem::rename(temporary_path, final_path, ec);
    if (ec) {
      return Status::IOError("failed to install SSTable " +
                             final_path.string() + ": " + ec.message());
    }

    Status manifest_status = manifest_->AppendTable(metadata);
    if (!manifest_status.ok()) {
      return manifest_status;
    }
    manifest_status = manifest_->Sync();
    if (!manifest_status.ok()) {
      return manifest_status;
    }

    auto [reader, open_status] = SSTableReader::Open(final_path);
    if (!open_status.ok()) {
      return open_status;
    }
    tables_.push_back(std::move(reader));

    Status wal_status = ResetWal();
    if (!wal_status.ok()) {
      return wal_status;
    }

    memtable_.Clear();
    return Status::OK();
  }

  Status ResetWal() {
    if (wal_ != nullptr) {
      Status sync_status = wal_->Sync();
      if (!sync_status.ok()) {
        return sync_status;
      }
      wal_.reset();
    }

    auto next_wal = std::make_unique<WalWriter>(wal_path_);
    Status open_status = next_wal->Open(/*append=*/false);
    if (!open_status.ok()) {
      return open_status;
    }

    wal_ = std::move(next_wal);
    return Status::OK();
  }

  Options options_;
  std::filesystem::path db_path_;
  std::filesystem::path wal_dir_;
  std::filesystem::path table_dir_;
  std::filesystem::path manifest_path_;
  std::filesystem::path wal_path_;

  mutable std::mutex mu_;
  MemTable memtable_;
  std::vector<std::unique_ptr<SSTableReader>> tables_;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t next_file_number_ = 1;
  std::unique_ptr<ManifestWriter> manifest_;
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
