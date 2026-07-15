#include "sstable.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("stratakv-sstable-test-" + std::to_string(now));
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
      std::cout << "All SSTable tests passed\n";
      return 0;
    }

    std::cerr << failures_ << " SSTable test expectation(s) failed\n";
    return 1;
  }

 private:
  int failures_ = 0;
};

void WritesAndReadsSortedTable(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("alpha", "one"), "add alpha");
  runner->ExpectOk(builder.Add("beta", "two"), "add beta");
  runner->ExpectOk(builder.Add("delta", "four"), "add delta");

  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish table");
  runner->Expect(metadata.entry_count == 3, "metadata entry count");
  runner->Expect(metadata.smallest_key == "alpha", "metadata smallest key");
  runner->Expect(metadata.largest_key == "delta", "metadata largest key");
  runner->Expect(metadata.file_size_bytes > 0, "metadata file size");

  auto [reader, open_status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(open_status, "open table");
  if (!reader) {
    return;
  }

  auto [value, get_status] = reader->Get("beta");
  runner->ExpectOk(get_status, "get beta");
  runner->Expect(value == "two", "beta value");

  auto [missing, missing_status] = reader->Get("gamma");
  (void)missing;
  runner->Expect(missing_status.code() == stratakv::Status::Code::kNotFound,
                 "missing key should return NotFound");

  auto it = reader->NewIterator();
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  runner->Expect(keys == std::vector<std::string>({"alpha", "beta", "delta"}),
                 "iterator returns sorted keys");

  it->Seek("carrot");
  runner->Expect(it->Valid(), "seek should land on delta");
  runner->Expect(it->key() == "delta", "seek target after beta");
}

void RejectsOutOfOrderKeys(TestRunner* runner) {
  TempDir dir;
  stratakv::SSTableBuilder builder(dir.path() / "000001.sst");
  runner->ExpectOk(builder.Add("beta", "two"), "add beta");

  const stratakv::Status status = builder.Add("alpha", "one");
  runner->Expect(status.code() == stratakv::Status::Code::kInvalidArgument,
                 "out-of-order keys should be rejected");
}

void DetectsCorruptDataBlock(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("alpha", "one"), "add alpha");
  runner->ExpectOk(builder.Add("beta", "two"), "add beta");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish table");

  std::fstream stream(table_path, std::ios::binary | std::ios::in |
                                      std::ios::out);
  char first_byte = 0;
  stream.read(&first_byte, 1);
  first_byte ^= 0x7f;
  stream.seekp(0);
  stream.write(&first_byte, 1);
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  (void)reader;
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "corrupted data block should fail checksum verification");
}

void StoresDeleteMarkers(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("alpha", "one"), "add alpha");
  runner->ExpectOk(builder.AddDeletion("beta"), "add beta tombstone");
  runner->ExpectOk(builder.Add("gamma", "three"), "add gamma");

  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish tombstone table");

  auto [reader, open_status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(open_status, "open tombstone table");
  if (!reader) {
    return;
  }

  const stratakv::TableLookup lookup = reader->Lookup("beta");
  runner->Expect(lookup.found, "tombstone lookup should find beta");
  runner->Expect(lookup.deleted, "tombstone lookup should mark beta deleted");

  auto [value, status] = reader->Get("beta");
  (void)value;
  runner->Expect(status.code() == stratakv::Status::Code::kNotFound,
                 "public Get should hide tombstones");

  std::vector<std::string> keys;
  auto it = reader->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  runner->Expect(keys == std::vector<std::string>({"alpha", "gamma"}),
                 "table iterator should skip tombstones");
}

}  // namespace

int main() {
  TestRunner runner;
  WritesAndReadsSortedTable(&runner);
  RejectsOutOfOrderKeys(&runner);
  DetectsCorruptDataBlock(&runner);
  StoresDeleteMarkers(&runner);
  return runner.Finish();
}
