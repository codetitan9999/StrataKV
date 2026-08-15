#include "stratakv/db.h"
#include "stratakv/file_system.h"

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

#include "compaction.h"
#include "manifest.h"
#include "merge_iterator.h"
#include "memtable.h"
#include "record.h"
#include "sstable.h"
#include "version_set.h"
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

}  // namespace

class DBImpl final : public DB {
 public:
  DBImpl(Options options, std::filesystem::path path)
      : options_(options),
        db_path_(std::move(path)),
        file_system_(options.file_system ? options.file_system
                                         : DefaultFileSystem()),
        block_cache_(std::make_shared<BlockCache>(options.block_cache_size)) {}

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

    manifest_ = std::make_unique<ManifestWriter>(manifest_path_, file_system_);
    Status manifest_status = manifest_->Open(/*append=*/true);
    if (!manifest_status.ok()) {
      return manifest_status;
    }

    wal_path_ = wal_dir_ / "current.log";
    retired_wal_path_ = wal_dir_ / "previous.log";
    if (std::filesystem::exists(retired_wal_path_, ec)) {
      Status recover_status = Recover(retired_wal_path_);
      if (!recover_status.ok()) {
        return recover_status;
      }
    } else if (ec) {
      return Status::IOError("failed to inspect retired WAL path " +
                             retired_wal_path_.string() + ": " + ec.message());
    }
    const bool wal_exists = std::filesystem::exists(wal_path_, ec);
    if (wal_exists) {
      Status recover_status = Recover(wal_path_);
      if (!recover_status.ok()) {
        return recover_status;
      }
    } else if (ec) {
      return Status::IOError("failed to inspect WAL path " +
                             wal_path_.string() + ": " + ec.message());
    }

    wal_ = std::make_unique<WalWriter>(wal_path_, file_system_,
                                       options_.max_wal_record_size);
    Status wal_status = wal_->Open(/*append=*/true);
    if (!wal_status.ok() || wal_exists) {
      return wal_status;
    }
    wal_status = wal_->Sync();
    if (!wal_status.ok()) {
      return wal_status;
    }
    return file_system_->SyncDirectory(wal_dir_);
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
      const TableMetadata& metadata = it->reader->metadata();
      if (key < metadata.smallest_key || key > metadata.largest_key) {
        continue;
      }

      const TableLookup table_lookup = it->reader->Lookup(key);
      if (!table_lookup.status.ok()) {
        return {"", table_lookup.status};
      }
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
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<MergeIteratorChild> children;
    children.reserve(tables_.size() + 1);
    std::size_t priority = 0;
    for (const auto& table : tables_) {
      children.push_back({table.reader->NewEntryIterator(), priority++});
    }
    children.push_back({memtable_.NewEntryIterator(), priority});
    return NewMergingIterator(std::move(children), read_options);
  }

  BlockCacheStats GetBlockCacheStats() const override {
    return block_cache_->stats();
  }

 private:
  struct TableState {
    std::uint64_t file_number = 0;
    std::uint32_t level = 0;
    std::shared_ptr<SSTableReader> reader;
  };

  Status LoadTables() {
    std::map<std::uint64_t, TableMetadata> active_tables;
    ManifestReader manifest_reader(manifest_path_);
    Status replay_status =
        manifest_reader.Replay([&](const ManifestEdit& edit) {
          if (edit.type == ManifestEditType::kTableDeleted) {
            active_tables.erase(edit.file_number);
            next_file_number_ =
                std::max(next_file_number_, edit.file_number + 1);
            return Status::OK();
          }

          active_tables[edit.file_number] = edit.table;
          next_file_number_ = std::max(next_file_number_, edit.file_number + 1);
          return Status::OK();
        });
    if (!replay_status.ok()) {
      return replay_status;
    }

    for (const auto& [file_number, manifest_metadata] : active_tables) {
      Status version_status = version_set_.AddTable(manifest_metadata);
      if (!version_status.ok()) {
        return version_status;
      }
      const std::filesystem::path path = TablePath(table_dir_, file_number);
      auto [table_reader, status] =
          SSTableReader::Open(path, block_cache_);
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

      tables_.push_back(TableState{file_number, manifest_metadata.level,
                                   std::move(table_reader)});
    }

    SortTablesForReads();

    return Status::OK();
  }

  Status Recover(const std::filesystem::path& path) {
    WalReader reader(path, file_system_, options_.max_wal_record_size);
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
    metadata.level = 0;
    metadata.file_path = final_path;

    Status install_status = file_system_->SyncFile(temporary_path);
    if (!install_status.ok()) {
      return install_status;
    }
    install_status = file_system_->Rename(temporary_path, final_path);
    if (!install_status.ok()) {
      return install_status;
    }
    install_status = file_system_->SyncDirectory(table_dir_);
    if (!install_status.ok()) {
      return install_status;
    }

    Status manifest_status = manifest_->AppendTable(metadata);
    if (!manifest_status.ok()) {
      return manifest_status;
    }
    manifest_status = manifest_->Sync();
    if (!manifest_status.ok()) {
      return manifest_status;
    }

    auto [reader, open_status] =
        SSTableReader::Open(final_path, block_cache_);
    if (!open_status.ok()) {
      return open_status;
    }
    Status version_status = version_set_.AddTable(metadata);
    if (!version_status.ok()) {
      return version_status;
    }
    tables_.push_back(TableState{file_number, 0, std::move(reader)});
    SortTablesForReads();

    Status wal_status = ResetWal();
    if (!wal_status.ok()) {
      return wal_status;
    }

    memtable_.Clear();
    return MaybeCompactTables();
  }

  Status MaybeCompactTables() {
    while (true) {
      VersionSet::CompactionSelection selection;
      if (options_.level0_compaction_trigger > 0 &&
          version_set_.LevelTableCount(0) >= options_.level0_compaction_trigger) {
        selection = version_set_.PickLevel0Compaction(
            options_.level0_compaction_trigger);
      } else if (options_.level1_compaction_trigger_bytes > 0 &&
                 version_set_.LevelSizeBytes(1) >
                     options_.level1_compaction_trigger_bytes) {
        selection = version_set_.PickLevelCompaction(1);
      } else {
        return Status::OK();
      }
      if (selection.inputs.empty()) return Status::OK();
      Status status = CompactTables(selection);
      if (!status.ok()) return status;
    }
  }

  Status CompactTables(const VersionSet::CompactionSelection& selection) {
    CompactionInput input;
    input.max_output_file_size = options_.max_compaction_output_file_size;
    input.drop_tombstones = selection.drop_tombstones;
    input.tables.reserve(selection.inputs.size());
    for (const TableMetadata& metadata : selection.inputs) {
      const auto it = std::find_if(
          tables_.begin(), tables_.end(), [&](const TableState& table) {
            return table.file_number == metadata.file_number;
          });
      if (it == tables_.end()) {
        return Status::Corruption("selected compaction table is not open");
      }
      input.tables.push_back(it->reader.get());
    }

    CompactionOutput output;
    CompactionJob job(db_path_);
    Status compaction_status = job.Run(input, &output);
    if (!compaction_status.ok()) {
      return compaction_status;
    }

    std::vector<TableState> next_tables;
    for (const TableState& table : tables_) {
      const bool selected = std::any_of(
          selection.inputs.begin(), selection.inputs.end(),
          [&](const TableMetadata& metadata) {
            return metadata.file_number == table.file_number;
          });
      if (!selected) next_tables.push_back(table);
    }

    for (const auto& output_entries : output.files) {
      const std::uint64_t file_number = next_file_number_++;
      const std::filesystem::path final_path =
          TablePath(table_dir_, file_number);
      const std::filesystem::path temporary_path =
          final_path.string() + ".tmp";

      SSTableBuilder builder(temporary_path);
      for (const TableEntry& entry : output_entries) {
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
      metadata.level = selection.output_level;
      metadata.file_path = final_path;

      Status install_status = file_system_->SyncFile(temporary_path);
      if (!install_status.ok()) {
        return install_status;
      }
      install_status = file_system_->Rename(temporary_path, final_path);
      if (!install_status.ok()) {
        return install_status;
      }
      install_status = file_system_->SyncDirectory(table_dir_);
      if (!install_status.ok()) {
        return install_status;
      }

      auto [reader, open_status] =
          SSTableReader::Open(final_path, block_cache_);
      if (!open_status.ok()) {
        return open_status;
      }

      next_tables.push_back(
          TableState{file_number, selection.output_level, std::move(reader)});
    }

    std::vector<TableMetadata> manifest_snapshot;
    manifest_snapshot.reserve(next_tables.size());
    for (const TableState& table : next_tables) {
      manifest_snapshot.push_back(table.reader->metadata());
      manifest_snapshot.back().file_number = table.file_number;
      manifest_snapshot.back().level = table.level;
    }
    Status manifest_status =
        manifest_->ReplaceWithSnapshot(manifest_snapshot);
    if (!manifest_status.ok()) {
      return manifest_status;
    }

    for (TableState& table : tables_) {
      const bool selected = std::any_of(
          selection.inputs.begin(), selection.inputs.end(),
          [&](const TableMetadata& metadata) {
            return metadata.file_number == table.file_number;
          });
      if (selected) table.reader->MarkObsolete();
    }

    tables_ = std::move(next_tables);
    SortTablesForReads();
    version_set_ = VersionSet();
    for (const TableState& table : tables_) {
      TableMetadata metadata = table.reader->metadata();
      metadata.file_number = table.file_number;
      metadata.level = table.level;
      Status version_status = version_set_.AddTable(std::move(metadata));
      if (!version_status.ok()) {
        return version_status;
      }
    }
    return Status::OK();
  }

  void SortTablesForReads() {
    std::sort(tables_.begin(), tables_.end(),
              [](const TableState& left, const TableState& right) {
                if (left.level != right.level) return left.level > right.level;
                if (left.level == 0) {
                  return left.file_number < right.file_number;
                }
                return left.reader->metadata().smallest_key <
                       right.reader->metadata().smallest_key;
              });
  }

  Status ResetWal() {
    if (wal_ != nullptr) {
      Status sync_status = wal_->Sync();
      if (!sync_status.ok()) {
        return sync_status;
      }
      Status close_status = wal_->Close();
      if (!close_status.ok()) {
        return close_status;
      }
      wal_.reset();
    }

    Status rotate_status = file_system_->Rename(wal_path_, retired_wal_path_);
    if (!rotate_status.ok()) {
      return rotate_status;
    }
    rotate_status = file_system_->SyncDirectory(wal_dir_);
    if (!rotate_status.ok()) {
      return rotate_status;
    }

    auto next_wal = std::make_unique<WalWriter>(
        wal_path_, file_system_, options_.max_wal_record_size);
    Status open_status = next_wal->Open(/*append=*/false);
    if (!open_status.ok()) {
      return open_status;
    }
    Status sync_status = next_wal->Sync();
    if (!sync_status.ok()) {
      return sync_status;
    }
    sync_status = file_system_->SyncDirectory(wal_dir_);
    if (!sync_status.ok()) {
      return sync_status;
    }

    wal_ = std::move(next_wal);
    Status remove_status = file_system_->Remove(retired_wal_path_);
    if (!remove_status.ok()) {
      return remove_status;
    }
    return file_system_->SyncDirectory(wal_dir_);
  }

  Options options_;
  std::filesystem::path db_path_;
  std::filesystem::path wal_dir_;
  std::filesystem::path table_dir_;
  std::filesystem::path manifest_path_;
  std::filesystem::path wal_path_;
  std::filesystem::path retired_wal_path_;

  mutable std::mutex mu_;
  MemTable memtable_;
  std::vector<TableState> tables_;
  VersionSet version_set_;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t next_file_number_ = 1;
  std::unique_ptr<ManifestWriter> manifest_;
  std::unique_ptr<WalWriter> wal_;
  std::shared_ptr<FileSystem> file_system_;
  std::shared_ptr<BlockCache> block_cache_;
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
