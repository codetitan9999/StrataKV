#include "stratakv/db.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
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

}  // namespace

int main() {
  TestRunner runner;
  PutGetDeleteRoundTrip(&runner);
  IteratorOrdersLiveKeys(&runner);
  ReplaysWalOnReopen(&runner);
  FlushesMemTableToSSTable(&runner);
  FlushedTombstoneHidesOlderTableValue(&runner);
  IteratorMergesFlushedTables(&runner);
  MissingManifestTableFailsOpen(&runner);
  CompactsFlushedTables(&runner);
  CompactionDropsCoveredTombstones(&runner);
  return runner.Finish();
}
