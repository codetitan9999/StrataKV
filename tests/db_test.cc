#include "stratakv/db.h"
#include "stratakv/file_system.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("stratakv-test-" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class TestRunner {
 public:
  void Expect(bool condition, const std::string& message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
  }

  void ExpectOk(const stratakv::Status& status, const std::string& context) {
    Expect(status.ok(), context + ": " + status.ToString());
  }

  int Finish() const {
    if (failures_ == 0) {
      std::cout << "All tests passed\n";
      return 0;
    }

    std::cerr << failures_ << " test expectation(s) failed\n";
    return 1;
  }

 private:
  int failures_ = 0;
};

class FailingRenameFileSystem final : public stratakv::FileSystem {
 public:
  stratakv::Status SyncFile(const std::filesystem::path& path) override {
    if (path.parent_path().filename() == "sst") {
      ++file_syncs;
    }
    return delegate_->SyncFile(path);
  }
  stratakv::Status Rename(const std::filesystem::path&,
                          const std::filesystem::path&) override {
    return stratakv::Status::IOError("injected SSTable rename failure");
  }
  stratakv::Status SyncDirectory(const std::filesystem::path& path) override {
    return delegate_->SyncDirectory(path);
  }
  stratakv::Status Remove(const std::filesystem::path& path) override {
    return delegate_->Remove(path);
  }

  int file_syncs = 0;

 private:
  std::shared_ptr<stratakv::FileSystem> delegate_ =
      stratakv::DefaultFileSystem();
};

class FailingWalRotationFileSystem final : public stratakv::FileSystem {
 public:
  stratakv::Status SyncFile(const std::filesystem::path& path) override {
    return delegate_->SyncFile(path);
  }
  stratakv::Status Rename(const std::filesystem::path& from,
                          const std::filesystem::path& to) override {
    const stratakv::Status status = delegate_->Rename(from, to);
    if (status.ok() && from.filename() == "current.log" &&
        to.filename() == "previous.log") {
      fail_next_wal_directory_sync_ = true;
    }
    return status;
  }
  stratakv::Status SyncDirectory(
      const std::filesystem::path& path) override {
    if (fail_next_wal_directory_sync_ && path.filename() == "wal") {
      fail_next_wal_directory_sync_ = false;
      return stratakv::Status::IOError(
          "injected WAL directory sync failure");
    }
    return delegate_->SyncDirectory(path);
  }
  stratakv::Status Remove(const std::filesystem::path& path) override {
    return delegate_->Remove(path);
  }

 private:
  bool fail_next_wal_directory_sync_ = false;
  std::shared_ptr<stratakv::FileSystem> delegate_ =
      stratakv::DefaultFileSystem();
};

std::unique_ptr<stratakv::DB> OpenOrFail(TestRunner* runner,
                                         const std::filesystem::path& path) {
  auto [db, status] = stratakv::DB::Open(stratakv::Options{}, path);
  runner->ExpectOk(status, "open database");
  return std::move(db);
}

std::unique_ptr<stratakv::DB> OpenOrFail(TestRunner* runner,
                                         const std::filesystem::path& path,
                                         const stratakv::Options& options) {
  auto [db, status] = stratakv::DB::Open(options, path);
  runner->ExpectOk(status, "open database");
  return std::move(db);
}

int CountSSTables(const std::filesystem::path& db_path) {
  const auto table_dir = db_path / "sst";
  if (!std::filesystem::exists(table_dir)) {
    return 0;
  }

  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(table_dir)) {
    if (entry.path().extension() == ".sst") {
      ++count;
    }
  }
  return count;
}

void ReportsSSTableInstallFailureBeforeManifestEdit(TestRunner* runner) {
  TempDir dir;
  auto file_system = std::make_shared<FailingRenameFileSystem>();
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 0;
  options.file_system = file_system;
  auto db = OpenOrFail(runner, dir.path(), options);

  const stratakv::Status status =
      db->Put(stratakv::WriteOptions{}, "key", "value");
  runner->Expect(!status.ok(), "SSTable rename failure should fail the write");
  runner->Expect(file_system->file_syncs == 1,
                 "SSTable temporary file should be synced before rename");
  runner->Expect(CountSSTables(dir.path()) == 0,
                 "failed installation should not expose an SSTable");

  db.reset();
  runner->Expect(std::filesystem::file_size(dir.path() / "MANIFEST") == 0,
                 "manifest should not reference an uninstalled SSTable");
}

void RecoversWalInterruptedAfterRename(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 0;
  options.file_system = std::make_shared<FailingWalRotationFileSystem>();

  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;
  const stratakv::Status write_status =
      db->Put(stratakv::WriteOptions{.sync = true}, "alpha", "one");
  runner->Expect(!write_status.ok(),
                 "injected WAL rotation failure should fail the write");
  runner->Expect(std::filesystem::exists(dir.path() / "wal" / "previous.log"),
                 "renamed WAL should remain recoverable after interruption");
  runner->Expect(!std::filesystem::exists(dir.path() / "wal" / "current.log"),
                 "interruption should occur before the new WAL is created");
  db.reset();

  stratakv::Options reopen_options;
  reopen_options.write_buffer_size = 1;
  reopen_options.level0_compaction_trigger = 0;
  auto reopened = OpenOrFail(runner, dir.path(), reopen_options);
  if (!reopened) return;
  auto [value, get_status] =
      reopened->Get(stratakv::ReadOptions{}, "alpha");
  runner->ExpectOk(get_status, "get value after interrupted WAL rotation");
  runner->Expect(value == "one",
                 "retired WAL should preserve acknowledged writes");
}

void PutGetDeleteRoundTrip(TestRunner* runner) {
  TempDir dir;
  auto db = OpenOrFail(runner, dir.path());
  if (!db) {
    return;
  }

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "alpha", "one"),
                   "put alpha");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "beta", "two"),
                   "put beta");

  auto [value, get_status] = db->Get(stratakv::ReadOptions{}, "alpha");
  runner->ExpectOk(get_status, "get alpha");
  runner->Expect(value == "one", "alpha should round-trip");

  runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "alpha"),
                   "delete alpha");
  auto [deleted_value, deleted_status] =
      db->Get(stratakv::ReadOptions{}, "alpha");
  (void)deleted_value;
  runner->Expect(deleted_status.code() == stratakv::Status::Code::kNotFound,
                 "deleted alpha should be hidden");
}

void IteratorOrdersLiveKeys(TestRunner* runner) {
  TempDir dir;
  auto db = OpenOrFail(runner, dir.path());
  if (!db) {
    return;
  }

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "c", "3"), "put c");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "1"), "put a");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "b", "2"), "put b");
  runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "b"), "delete b");

  std::vector<std::string> keys;
  auto it = db->NewIterator(stratakv::ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }

  runner->ExpectOk(it->status(), "iterator status");
  runner->Expect(keys == std::vector<std::string>({"a", "c"}),
                 "iterator should return sorted live keys");
}

void ReplaysWalOnReopen(TestRunner* runner) {
  TempDir dir;

  {
    auto db = OpenOrFail(runner, dir.path());
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{.sync = true}, "alpha",
                             "one"),
                     "put alpha before reopen");
    runner->ExpectOk(db->Put(stratakv::WriteOptions{.sync = true}, "beta",
                             "two"),
                     "put beta before reopen");
    runner->ExpectOk(db->Delete(stratakv::WriteOptions{.sync = true}, "alpha"),
                     "delete alpha before reopen");
  }

  auto db = OpenOrFail(runner, dir.path());
  if (!db) {
    return;
  }

  auto [alpha, alpha_status] = db->Get(stratakv::ReadOptions{}, "alpha");
  (void)alpha;
  runner->Expect(alpha_status.code() == stratakv::Status::Code::kNotFound,
                 "recovered delete tombstone should hide alpha");

  auto [beta, beta_status] = db->Get(stratakv::ReadOptions{}, "beta");
  runner->ExpectOk(beta_status, "get beta after WAL replay");
  runner->Expect(beta == "two", "beta should survive reopen");
}

void IgnoresTornWalTailOnReopen(TestRunner* runner) {
  TempDir dir;

  {
    auto db = OpenOrFail(runner, dir.path());
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{.sync = true}, "alpha",
                             "one"),
                     "put alpha before torn WAL tail");
  }

  std::ofstream stream(dir.path() / "wal" / "current.log",
                       std::ios::binary | std::ios::app);
  stream.write("abc", 3);
  stream.close();
  runner->Expect(!stream.fail(), "append torn WAL tail");

  auto db = OpenOrFail(runner, dir.path());
  if (!db) {
    return;
  }

  auto [value, status] = db->Get(stratakv::ReadOptions{}, "alpha");
  runner->ExpectOk(status, "get alpha after torn WAL reopen");
  runner->Expect(value == "one", "complete WAL record should survive");
}

void FlushesMemTableToSSTable(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;

  {
    auto db = OpenOrFail(runner, dir.path(), options);
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "alpha", "one"),
                     "put alpha for flush");
    runner->Expect(CountSSTables(dir.path()) == 1,
                   "put should flush one SSTable");
    runner->Expect(std::filesystem::exists(dir.path() / "MANIFEST"),
                   "flush should create a manifest");

    auto [value, status] = db->Get(stratakv::ReadOptions{}, "alpha");
    runner->ExpectOk(status, "get alpha from flushed table");
    runner->Expect(value == "one", "flushed alpha value");
  }

  auto reopened = OpenOrFail(runner, dir.path(), options);
  if (!reopened) {
    return;
  }

  auto [value, status] = reopened->Get(stratakv::ReadOptions{}, "alpha");
  runner->ExpectOk(status, "get alpha after table reopen");
  runner->Expect(value == "one", "alpha should survive table reopen");
}

void FlushedTombstoneHidesOlderTableValue(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;

  {
    auto db = OpenOrFail(runner, dir.path(), options);
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "alpha", "one"),
                     "put alpha before tombstone");
    runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "alpha"),
                     "delete alpha tombstone");
    runner->Expect(CountSSTables(dir.path()) == 2,
                   "put and delete should each flush an SSTable");

    auto [value, status] = db->Get(stratakv::ReadOptions{}, "alpha");
    (void)value;
    runner->Expect(status.code() == stratakv::Status::Code::kNotFound,
                   "flushed tombstone should hide alpha");
  }

  auto reopened = OpenOrFail(runner, dir.path(), options);
  if (!reopened) {
    return;
  }

  auto [value, status] = reopened->Get(stratakv::ReadOptions{}, "alpha");
  (void)value;
  runner->Expect(status.code() == stratakv::Status::Code::kNotFound,
                 "reopened tombstone should hide alpha");
}

void IteratorMergesFlushedTables(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;

  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) {
    return;
  }

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "c", "3"), "put c");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "1"), "put a");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "b", "2"), "put b");
  runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "b"), "delete b");

  std::vector<std::string> keys;
  auto it = db->NewIterator(stratakv::ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }

  runner->ExpectOk(it->status(), "flushed iterator status");
  runner->Expect(keys == std::vector<std::string>({"a", "c"}),
                 "iterator should merge flushed tables and tombstones");
}

void IteratorSeeksAcrossVersions(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 0;
  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "old-a"),
                   "put old a");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "b", "old-b"),
                   "put old b");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "new-a"),
                   "overwrite a");
  runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "b"), "delete b");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "d", "four"), "put d");

  auto it = db->NewIterator(stratakv::ReadOptions{});
  it->Seek("b");
  runner->Expect(it->Valid(), "seek should find a later live key");
  runner->Expect(it->key() == "d", "seek should skip a tombstoned key");
  it->SeekToFirst();
  runner->Expect(it->Valid() && it->key() == "a" && it->value() == "new-a",
                 "newest table value should win during merge");
  it->Next();
  runner->Expect(it->Valid() && it->key() == "d",
                 "merge should return each visible key once");
  it->Next();
  runner->Expect(!it->Valid(), "merged iterator should reach its end");
  runner->ExpectOk(it->status(), "seek merge iterator status");
}

void IteratorMergesManyTables(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 0;
  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  for (int version = 0; version < 32; ++version) {
    for (int key = 0; key < 8; ++key) {
      std::ostringstream name;
      name << "key-" << std::setw(2) << std::setfill('0') << key;
      runner->ExpectOk(
          db->Put(stratakv::WriteOptions{}, name.str(),
                  "version-" + std::to_string(version)),
          "put high-table-count version");
    }
  }
  runner->Expect(CountSSTables(dir.path()) == 256,
                 "test should create a high table count");

  auto it = db->NewIterator(stratakv::ReadOptions{});
  int visited = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    runner->Expect(it->value() == "version-31",
                   "newest value should win across many tables");
    ++visited;
  }
  runner->ExpectOk(it->status(), "high-table-count iterator status");
  runner->Expect(visited == 8,
                 "high-table-count merge should emit every key once");
}

void IteratorHonorsBoundsAndPrefix(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 0;
  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  for (const std::string& key : {"aa", "app-1", "app-2", "app-3",
                                 "banana", "carrot"}) {
    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, key, key),
                     "put bounded scan key");
  }
  runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "app-2"),
                   "delete bounded scan key");

  stratakv::ReadOptions range;
  range.lower_bound = "app-1";
  range.upper_bound = "banana";
  std::vector<std::string> keys;
  auto it = db->NewIterator(range);
  for (it->SeekToFirst(); it->Valid(); it->Next()) keys.emplace_back(it->key());
  runner->ExpectOk(it->status(), "bounded iterator status");
  runner->Expect(keys == std::vector<std::string>({"app-1", "app-3"}),
                 "range scan should use inclusive/exclusive bounds");

  stratakv::ReadOptions prefix;
  prefix.prefix = "app-";
  prefix.lower_bound = "app-2";
  keys.clear();
  it = db->NewIterator(prefix);
  it->Seek("aa");
  for (; it->Valid(); it->Next()) keys.emplace_back(it->key());
  runner->ExpectOk(it->status(), "prefix iterator status");
  runner->Expect(keys == std::vector<std::string>({"app-3"}),
                 "prefix scan should intersect its lower bound and tombstones");

  prefix.upper_bound = "app-3";
  it = db->NewIterator(prefix);
  it->SeekToFirst();
  runner->Expect(!it->Valid(), "exclusive upper bound should end prefix scan");
  runner->ExpectOk(it->status(), "empty bounded prefix status");
}

void IteratorReportsDeferredBlockErrors(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 6000;
  options.level0_compaction_trigger = 0;
  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  const std::string value(200, 'x');
  for (int i = 0; i < 30; ++i) {
    std::ostringstream key;
    key << "key-" << std::setw(3) << std::setfill('0') << i;
    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, key.str(), value),
                     "put scan error entry");
  }
  runner->Expect(CountSSTables(dir.path()) == 1,
                 "scan error setup should flush one multi-block table");

  auto it = db->NewIterator(stratakv::ReadOptions{});
  std::error_code ec;
  std::filesystem::remove(dir.path() / "sst" / "000001.sst", ec);
  runner->Expect(!ec, "remove table before deferred scan read");
  int visited = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++visited;
  runner->Expect(visited > 0 && visited < 30,
                 "scan should consume cached data before the missing block");
  runner->Expect(it->status().code() == stratakv::Status::Code::kIOError,
                 "scan should expose deferred block I/O failure");
}

void MissingManifestTableFailsOpen(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;

  {
    auto db = OpenOrFail(runner, dir.path(), options);
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "alpha", "one"),
                     "put alpha before hiding table");
  }

  std::error_code ec;
  std::filesystem::rename(dir.path() / "sst" / "000001.sst",
                          dir.path() / "sst" / "000001.hidden", ec);
  runner->Expect(!ec, "hide manifest-listed SSTable");

  auto [db, status] = stratakv::DB::Open(options, dir.path());
  (void)db;
  runner->Expect(!status.ok(), "missing manifest-listed table should fail open");
}

void CompactsFlushedTables(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 3;

  {
    auto db = OpenOrFail(runner, dir.path(), options);
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "c", "3"), "put c");
    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "1"), "put a");
    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "b", "2"), "put b");

    runner->Expect(CountSSTables(dir.path()) == 1,
                   "compaction should replace three tables with one");

    auto [value, status] = db->Get(stratakv::ReadOptions{}, "a");
    runner->ExpectOk(status, "get compacted a");
    runner->Expect(value == "1", "compacted a value");
  }

  auto reopened = OpenOrFail(runner, dir.path(), options);
  if (!reopened) {
    return;
  }

  std::vector<std::string> keys;
  auto it = reopened->NewIterator(stratakv::ReadOptions{});
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }

  runner->Expect(keys == std::vector<std::string>({"a", "b", "c"}),
                 "compacted table should reopen with sorted keys");
}

void CompactionDropsCoveredTombstones(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 3;

  {
    auto db = OpenOrFail(runner, dir.path(), options);
    if (!db) {
      return;
    }

    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "alpha", "one"),
                     "put alpha before compacted delete");
    runner->ExpectOk(db->Delete(stratakv::WriteOptions{}, "alpha"),
                     "delete alpha before compaction");
    runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "beta", "two"),
                     "put beta to trigger compaction");

    runner->Expect(CountSSTables(dir.path()) == 1,
                   "compaction should leave one live table");

    auto [alpha, alpha_status] = db->Get(stratakv::ReadOptions{}, "alpha");
    (void)alpha;
    runner->Expect(alpha_status.code() == stratakv::Status::Code::kNotFound,
                   "compaction should keep alpha deleted");
  }

  auto reopened = OpenOrFail(runner, dir.path(), options);
  if (!reopened) {
    return;
  }

  auto [alpha, alpha_status] =
      reopened->Get(stratakv::ReadOptions{}, "alpha");
  (void)alpha;
  runner->Expect(alpha_status.code() == stratakv::Status::Code::kNotFound,
                 "reopened compacted state should keep alpha deleted");

  auto [beta, beta_status] = reopened->Get(stratakv::ReadOptions{}, "beta");
  runner->ExpectOk(beta_status, "get beta after compacted reopen");
  runner->Expect(beta == "two", "beta should survive compaction");
}

void CompactionSplitsOutputsAndRetainsDisjointLevelFiles(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.level0_compaction_trigger = 3;
  options.max_compaction_output_file_size = 20;

  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "old-a"),
                   "put first a");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "m", "old-m"),
                   "put first m");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "z", "old-z"),
                   "put disjoint z");
  runner->Expect(CountSSTables(dir.path()) == 3,
                 "first compaction should split into three level files");

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "new-a"),
                   "overwrite a");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "n", "new-n"),
                   "put n");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "y", "new-y"),
                   "put y to compact overlapping range");
  runner->Expect(CountSSTables(dir.path()) == 5,
                 "second compaction should retain disjoint z table");

  auto [a, a_status] = db->Get(stratakv::ReadOptions{}, "a");
  runner->ExpectOk(a_status, "get overwritten a");
  runner->Expect(a == "new-a", "newest level-zero value should win");
  auto [z, z_status] = db->Get(stratakv::ReadOptions{}, "z");
  runner->ExpectOk(z_status, "get retained z");
  runner->Expect(z == "old-z", "disjoint level-one table should be retained");

  db.reset();
  db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;
  std::tie(z, z_status) = db->Get(stratakv::ReadOptions{}, "z");
  runner->ExpectOk(z_status, "get retained z after reopen");
  runner->Expect(z == "old-z", "retained table should survive manifest replay");
}

void IteratorSurvivesCompactionCleanup(TestRunner* runner) {
  TempDir dir;
  stratakv::Options options;
  options.write_buffer_size = 1;
  options.block_cache_size = 0;
  options.level0_compaction_trigger = 3;

  auto db = OpenOrFail(runner, dir.path(), options);
  if (!db) return;

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "a", "one"),
                   "put a before iterator snapshot");
  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "b", "two"),
                   "put b before iterator snapshot");
  auto it = db->NewIterator(stratakv::ReadOptions{});

  runner->ExpectOk(db->Put(stratakv::WriteOptions{}, "c", "three"),
                   "put c to trigger compaction");
  runner->Expect(CountSSTables(dir.path()) == 3,
                 "pinned iterator should retain its two obsolete tables");

  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  runner->ExpectOk(it->status(), "iterator status after compaction");
  runner->Expect(keys == std::vector<std::string>({"a", "b"}),
                 "iterator should retain its pre-compaction snapshot");

  it.reset();
  runner->Expect(CountSSTables(dir.path()) == 1,
                 "obsolete tables should be removed after iterator release");
}

}  // namespace

int main() {
  TestRunner runner;
  ReportsSSTableInstallFailureBeforeManifestEdit(&runner);
  RecoversWalInterruptedAfterRename(&runner);
  PutGetDeleteRoundTrip(&runner);
  IteratorOrdersLiveKeys(&runner);
  ReplaysWalOnReopen(&runner);
  IgnoresTornWalTailOnReopen(&runner);
  FlushesMemTableToSSTable(&runner);
  FlushedTombstoneHidesOlderTableValue(&runner);
  IteratorMergesFlushedTables(&runner);
  IteratorSeeksAcrossVersions(&runner);
  IteratorMergesManyTables(&runner);
  IteratorHonorsBoundsAndPrefix(&runner);
  IteratorReportsDeferredBlockErrors(&runner);
  MissingManifestTableFailsOpen(&runner);
  CompactsFlushedTables(&runner);
  CompactionDropsCoveredTombstones(&runner);
  CompactionSplitsOutputsAndRetainsDisjointLevelFiles(&runner);
  IteratorSurvivesCompactionCleanup(&runner);
  return runner.Finish();
}
