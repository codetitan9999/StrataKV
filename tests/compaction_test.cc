#include "compaction.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("stratakv-compaction-test-" + std::to_string(now));
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
      std::cout << "All compaction tests passed\n";
      return 0;
    }

    std::cerr << failures_ << " compaction test expectation(s) failed\n";
    return 1;
  }

 private:
  int failures_ = 0;
};

std::unique_ptr<stratakv::SSTableReader> BuildTableOrNull(
    TestRunner* runner, const std::filesystem::path& path,
    const std::vector<stratakv::TableEntry>& entries) {
  stratakv::SSTableBuilder builder(path);
  for (const stratakv::TableEntry& entry : entries) {
    stratakv::Status status =
        entry.type == stratakv::RecordType::kDelete
            ? builder.AddDeletion(entry.key)
            : builder.Add(entry.key, entry.value);
    runner->ExpectOk(status, "add table entry");
  }

  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish table");

  auto [reader, status] = stratakv::SSTableReader::Open(path);
  runner->ExpectOk(status, "open table");
  return std::move(reader);
}

void MergesNewestEntriesAndDropsCoveredTombstones(TestRunner* runner) {
  TempDir dir;

  auto first = BuildTableOrNull(
      runner, dir.path() / "000001.sst",
      {
          {stratakv::RecordType::kPut, "alpha", "one"},
          {stratakv::RecordType::kPut, "beta", "two"},
      });
  auto second = BuildTableOrNull(
      runner, dir.path() / "000002.sst",
      {
          {stratakv::RecordType::kDelete, "alpha", ""},
          {stratakv::RecordType::kPut, "gamma", "three"},
      });

  stratakv::CompactionInput input;
  input.tables = {first.get(), second.get()};

  stratakv::CompactionOutput output;
  stratakv::CompactionJob job(dir.path());
  runner->ExpectOk(job.Run(input, &output), "run compaction job");

  std::vector<std::string> keys;
  for (const stratakv::TableEntry& entry : output.entries) {
    keys.push_back(entry.key);
  }

  runner->Expect(keys == std::vector<std::string>({"beta", "gamma"}),
                 "compaction should keep only visible keys");
}

void RejectsNullOutput(TestRunner* runner) {
  stratakv::CompactionJob job(std::filesystem::temp_directory_path());
  const stratakv::Status status = job.Run(stratakv::CompactionInput{}, nullptr);
  runner->Expect(status.code() == stratakv::Status::Code::kInvalidArgument,
                 "compaction should reject null output");
}

}  // namespace

int main() {
  TestRunner runner;
  MergesNewestEntriesAndDropsCoveredTombstones(&runner);
  RejectsNullOutput(&runner);
  return runner.Finish();
}
