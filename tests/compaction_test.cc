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

std::shared_ptr<stratakv::SSTableReader> BuildTableOrNull(
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

  std::vector<std::vector<stratakv::TableEntry>> output;
  std::vector<stratakv::TableEntry> current;
  stratakv::CompactionJob job(dir.path());
  runner->ExpectOk(
      job.Run(input,
              {[&](const auto& entry) {
                 current.push_back(entry);
                 return stratakv::Status::OK();
               },
               [&] {
                 output.push_back(std::move(current));
                 current.clear();
                 return stratakv::Status::OK();
               }}),
      "run compaction job");

  std::vector<std::string> keys;
  for (const auto& file : output) {
    for (const stratakv::TableEntry& entry : file) keys.push_back(entry.key);
  }

  runner->Expect(keys == std::vector<std::string>({"beta", "gamma"}),
                 "compaction should keep only visible keys");
}

void SplitsOutputsAtConfiguredSize(TestRunner* runner) {
  TempDir dir;
  auto table = BuildTableOrNull(
      runner, dir.path() / "000001.sst",
      {{stratakv::RecordType::kPut, "alpha", "11111"},
       {stratakv::RecordType::kPut, "bravo", "22222"},
       {stratakv::RecordType::kPut, "charlie", "33333"}});
  stratakv::CompactionInput input;
  input.tables = {table.get()};
  input.max_output_file_size = 30;
  std::vector<std::vector<stratakv::TableEntry>> output;
  std::vector<stratakv::TableEntry> current;
  stratakv::CompactionJob job(dir.path());
  runner->ExpectOk(
      job.Run(input,
              {[&](const auto& entry) {
                 current.push_back(entry);
                 return stratakv::Status::OK();
               },
               [&] {
                 output.push_back(std::move(current));
                 current.clear();
                 return stratakv::Status::OK();
               }}),
      "run split compaction");
  runner->Expect(output.size() == 3,
                 "compaction should emit size-limited files");
}

void RetainsTombstonesForDeeperLevels(TestRunner* runner) {
  TempDir dir;
  auto table = BuildTableOrNull(
      runner, dir.path() / "000001.sst",
      {{stratakv::RecordType::kDelete, "alpha", ""}});
  stratakv::CompactionInput input;
  input.tables = {table.get()};
  input.drop_tombstones = false;
  std::vector<std::vector<stratakv::TableEntry>> output;
  std::vector<stratakv::TableEntry> current;
  stratakv::CompactionJob job(dir.path());
  runner->ExpectOk(
      job.Run(input,
              {[&](const auto& entry) {
                 current.push_back(entry);
                 return stratakv::Status::OK();
               },
               [&] {
                 output.push_back(std::move(current));
                 current.clear();
                 return stratakv::Status::OK();
               }}),
      "run tombstone compaction");
  runner->Expect(output.size() == 1 && output[0].size() == 1 &&
                     output[0][0].type == stratakv::RecordType::kDelete,
                 "compaction should retain tombstone when requested");
}

void RejectsNullOutput(TestRunner* runner) {
  stratakv::CompactionJob job(std::filesystem::temp_directory_path());
  const stratakv::Status status =
      job.Run(stratakv::CompactionInput{}, {});
  runner->Expect(status.code() == stratakv::Status::Code::kInvalidArgument,
                 "compaction should reject null output");
}

void StopsStreamingWhenOutputFails(TestRunner* runner) {
  TempDir dir;
  auto table = BuildTableOrNull(
      runner, dir.path() / "000001.sst",
      {{stratakv::RecordType::kPut, "alpha", "11111"},
       {stratakv::RecordType::kPut, "bravo", "22222"},
       {stratakv::RecordType::kPut, "charlie", "33333"}});
  stratakv::CompactionInput input;
  input.tables = {table.get()};
  input.max_output_file_size = 30;
  int calls = 0;
  stratakv::CompactionJob job(dir.path());
  const stratakv::Status status = job.Run(
      input, {[](const auto&) { return stratakv::Status::OK(); },
              [&] {
                ++calls;
                return stratakv::Status::IOError("injected output failure");
              }});
  runner->Expect(status.code() == stratakv::Status::Code::kIOError,
                 "compaction should propagate output failure");
  runner->Expect(calls == 1,
                 "compaction should stop after the first failed output");
}

void StopsStreamingWhenEntrySinkFails(TestRunner* runner) {
  TempDir dir;
  auto table = BuildTableOrNull(
      runner, dir.path() / "000001.sst",
      {{stratakv::RecordType::kPut, "alpha", "one"},
       {stratakv::RecordType::kPut, "bravo", "two"},
       {stratakv::RecordType::kPut, "charlie", "three"}});
  stratakv::CompactionInput input;
  input.tables = {table.get()};
  int entries = 0;
  int finishes = 0;
  stratakv::CompactionJob job(dir.path());
  const stratakv::Status status = job.Run(
      input,
      {[&](const auto&) {
         ++entries;
         return entries == 2
                    ? stratakv::Status::IOError("injected entry failure")
                    : stratakv::Status::OK();
       },
       [&] {
         ++finishes;
         return stratakv::Status::OK();
       }});
  runner->Expect(status.code() == stratakv::Status::Code::kIOError,
                 "compaction should propagate entry sink failure");
  runner->Expect(entries == 2 && finishes == 0,
                 "compaction should stop without finishing a failed output");
}

}  // namespace

int main() {
  TestRunner runner;
  MergesNewestEntriesAndDropsCoveredTombstones(&runner);
  SplitsOutputsAtConfiguredSize(&runner);
  RetainsTombstonesForDeeperLevels(&runner);
  RejectsNullOutput(&runner);
  StopsStreamingWhenOutputFails(&runner);
  StopsStreamingWhenEntrySinkFails(&runner);
  return runner.Finish();
}
