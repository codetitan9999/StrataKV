#include "sstable.h"

#include <chrono>
#include <cstdint>
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

void AppendFixed(std::string* out, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xffU));
  }
}

std::uint32_t Checksum(const std::string& payload) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char byte : payload) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

void AppendLegacyPut(std::string* data, const std::string& key,
                     const std::string& value) {
  AppendFixed(data, 1, 1);
  AppendFixed(data, key.size(), 4);
  AppendFixed(data, value.size(), 4);
  data->append(key);
  data->append(value);
}

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

void ReadsAcrossMultipleBlocks(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  stratakv::SSTableBuilder builder(table_path, 32);
  for (int i = 0; i < 20; ++i) {
    const std::string key = "key-" + std::to_string(100 + i);
    if (i == 9) {
      runner->ExpectOk(builder.AddDeletion(key), "add multi-block tombstone");
    } else {
      runner->ExpectOk(builder.Add(key, "value-" + std::to_string(i)),
                       "add multi-block value");
    }
  }

  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish multi-block table");
  auto [reader, open_status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(open_status, "open multi-block table");
  if (!reader) {
    return;
  }

  for (int i : {0, 4, 8, 10, 15, 19}) {
    const std::string key = "key-" + std::to_string(100 + i);
    auto [value, status] = reader->Get(key);
    runner->ExpectOk(status, "get key across data blocks");
    runner->Expect(value == "value-" + std::to_string(i),
                   "value across data blocks");
  }
  const stratakv::TableLookup deleted = reader->Lookup("key-109");
  runner->Expect(deleted.found && deleted.deleted,
                 "tombstone survives a data-block boundary");

  auto it = reader->NewIterator();
  std::size_t visible_count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++visible_count;
  }
  runner->Expect(visible_count == 19,
                 "iterator visits every live entry across blocks");
}

void DetectsCorruptIndexBlock(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  stratakv::SSTableBuilder builder(table_path, 24);
  runner->ExpectOk(builder.Add("alpha", "one"), "add alpha");
  runner->ExpectOk(builder.Add("beta", "two"), "add beta");
  runner->ExpectOk(builder.Add("gamma", "three"), "add gamma");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish indexed table");

  constexpr std::streamoff kFooterSize = 36;
  std::fstream stream(table_path, std::ios::binary | std::ios::in |
                                      std::ios::out | std::ios::ate);
  const std::streamoff file_size = static_cast<std::streamoff>(stream.tellg());
  const std::streamoff index_byte = file_size - kFooterSize - 1;
  stream.seekg(index_byte);
  char byte = 0;
  stream.read(&byte, 1);
  byte ^= 0x55;
  stream.seekp(index_byte);
  stream.write(&byte, 1);
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  (void)reader;
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "corrupted index block should fail checksum verification");
}

void ReadsLegacySingleBlockTable(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";

  std::string data;
  AppendLegacyPut(&data, "alpha", "one");
  AppendLegacyPut(&data, "beta", "two");
  std::string file = data;
  AppendFixed(&file, 2, 8);
  AppendFixed(&file, data.size(), 8);
  AppendFixed(&file, Checksum(data), 4);
  file.append("STKV0001");
  std::ofstream stream(table_path, std::ios::binary);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(status, "open legacy single-block table");
  if (!reader) {
    return;
  }
  auto [value, get_status] = reader->Get("beta");
  runner->ExpectOk(get_status, "read legacy table value");
  runner->Expect(value == "two", "legacy table value");
  runner->Expect(reader->metadata().entry_count == 2,
                 "legacy table metadata");
}

}  // namespace

int main() {
  TestRunner runner;
  WritesAndReadsSortedTable(&runner);
  RejectsOutOfOrderKeys(&runner);
  DetectsCorruptDataBlock(&runner);
  StoresDeleteMarkers(&runner);
  ReadsAcrossMultipleBlocks(&runner);
  DetectsCorruptIndexBlock(&runner);
  ReadsLegacySingleBlockTable(&runner);
  return runner.Finish();
}
