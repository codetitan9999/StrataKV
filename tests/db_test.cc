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

}  // namespace

int main() {
  TestRunner runner;
  PutGetDeleteRoundTrip(&runner);
  IteratorOrdersLiveKeys(&runner);
  ReplaysWalOnReopen(&runner);
  return runner.Finish();
}
