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

std::string BloomFilter(const std::vector<std::string>& keys) {
  std::string bits(8, '\0');
  for (const auto& key : keys) {
    std::uint32_t hash = Checksum(key);
    const std::uint32_t delta = (hash >> 17) | (hash << 15);
    for (int probe = 0; probe < 7; ++probe) {
      const std::size_t bit = hash % 64;
      bits[bit / 8] |= static_cast<char>(1U << (bit % 8));
      hash += delta;
    }
  }
  std::string filter;
  AppendFixed(&filter, 1, 4);
  AppendFixed(&filter, 7, 4);
  AppendFixed(&filter, 64, 8);
  filter.append(bits);
  return filter;
}

std::uint64_t ReadFixed(const std::string& input, std::size_t offset,
                        std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(input[offset + i]))
             << (8 * i);
  }
  return value;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  const auto size = stream.tellg();
  std::string contents(static_cast<std::size_t>(size), '\0');
  stream.seekg(0);
  stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  return contents;
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

  constexpr std::streamoff kFooterSize = 56;
  std::fstream stream(table_path, std::ios::binary | std::ios::in |
                                      std::ios::out | std::ios::ate);
  const std::streamoff file_size = static_cast<std::streamoff>(stream.tellg());
  std::string footer(static_cast<std::size_t>(kFooterSize), '\0');
  stream.seekg(file_size - kFooterSize);
  stream.read(footer.data(), kFooterSize);
  const std::streamoff index_byte = static_cast<std::streamoff>(
      ReadFixed(footer, 28, 8) - 1);
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

void WritesGoldenPrefixCompressedBlock(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path, 4096);
  runner->ExpectOk(builder.Add("alpine", "one"), "add golden first key");
  runner->ExpectOk(builder.Add("alps", "two"), "add golden second key");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish golden table");

  std::string data;
  AppendFixed(&data, 0, 4);
  AppendFixed(&data, 6, 4);
  AppendFixed(&data, 3, 4);
  AppendFixed(&data, 1, 1);
  data.append("alpineone");
  AppendFixed(&data, 3, 4);
  AppendFixed(&data, 1, 4);
  AppendFixed(&data, 3, 4);
  AppendFixed(&data, 1, 1);
  data.append("stwo");
  AppendFixed(&data, 0, 4);
  AppendFixed(&data, 1, 4);

  std::string expected = data;
  AppendFixed(&expected, Checksum(data), 4);
  const std::uint64_t index_offset = expected.size();
  std::string index;
  AppendFixed(&index, 4, 4);
  index.append("alps");
  AppendFixed(&index, 0, 8);
  AppendFixed(&index, index_offset, 8);
  AppendFixed(&index, 2, 8);
  expected.append(index);
  const std::uint64_t filter_offset = expected.size();
  const std::string filter = BloomFilter({"alpine", "alps"});
  expected.append(filter);
  AppendFixed(&expected, 2, 8);
  AppendFixed(&expected, index_offset, 8);
  AppendFixed(&expected, index.size(), 8);
  AppendFixed(&expected, Checksum(index), 4);
  AppendFixed(&expected, filter_offset, 8);
  AppendFixed(&expected, filter.size(), 8);
  AppendFixed(&expected, Checksum(filter), 4);
  expected.append("STKV0004");

  runner->Expect(ReadFile(table_path) == expected,
                 "prefix-compressed encoding matches golden bytes");
}

void DetectsCorruptRestartArray(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("prefix-000", "one"), "add restart key");
  runner->ExpectOk(builder.Add("prefix-001", "two"), "add compressed key");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish restart table");

  std::string file = ReadFile(table_path);
  constexpr std::size_t kFooterSize = 56;
  const std::size_t index_offset = static_cast<std::size_t>(
      ReadFixed(file, file.size() - kFooterSize + 8, 8));
  const std::size_t restart_offset = index_offset - 4 - 4 - 4;
  file[restart_offset] = 1;
  const std::string data = file.substr(0, index_offset - 4);
  const std::uint32_t checksum = Checksum(data);
  for (std::size_t i = 0; i < 4; ++i) {
    file[index_offset - 4 + i] =
        static_cast<char>((checksum >> (8 * i)) & 0xffU);
  }
  std::ofstream stream(table_path, std::ios::binary | std::ios::trunc);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  (void)reader;
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "misaligned restart array is rejected after checksum");
}

void ReadsLegacyIndexedTable(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  std::string data;
  AppendLegacyPut(&data, "alpha", "one");
  AppendLegacyPut(&data, "beta", "two");
  std::string file = data;
  AppendFixed(&file, Checksum(data), 4);
  const std::uint64_t index_offset = file.size();
  std::string index;
  AppendFixed(&index, 4, 4);
  index.append("beta");
  AppendFixed(&index, 0, 8);
  AppendFixed(&index, index_offset, 8);
  AppendFixed(&index, 2, 8);
  file.append(index);
  AppendFixed(&file, 2, 8);
  AppendFixed(&file, index_offset, 8);
  AppendFixed(&file, index.size(), 8);
  AppendFixed(&file, Checksum(index), 4);
  file.append("STKV0002");
  std::ofstream stream(table_path, std::ios::binary);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(status, "open legacy indexed table");
  if (!reader) return;
  auto [value, get_status] = reader->Get("beta");
  runner->ExpectOk(get_status, "read legacy indexed value");
  runner->Expect(value == "two", "legacy indexed table value");
}

void ReadsLegacyCompressedTable(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("alpha", "one"), "add legacy compressed key");
  runner->ExpectOk(builder.Add("beta", "two"), "add legacy compressed value");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish source compressed table");

  std::string file = ReadFile(table_path);
  constexpr std::size_t kFilterFooterSize = 56;
  const std::size_t footer = file.size() - kFilterFooterSize;
  const std::size_t filter_offset =
      static_cast<std::size_t>(ReadFixed(file, footer + 28, 8));
  const std::string legacy_footer = file.substr(footer, 28);
  file.resize(filter_offset);
  file.append(legacy_footer);
  file.append("STKV0003");
  std::ofstream stream(table_path, std::ios::binary | std::ios::trunc);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(status, "open legacy compressed table");
  if (!reader) return;
  auto [value, get_status] = reader->Get("beta");
  runner->ExpectOk(get_status, "read legacy compressed value");
  runner->Expect(value == "two", "legacy compressed table value");
}

void CompressesSharedKeyPrefixes(TestRunner* runner) {
  TempDir dir;
  stratakv::SSTableBuilder builder(dir.path() / "000001.sst", 1 << 20);
  std::size_t uncompressed_entries_size = 0;
  for (int i = 1000; i < 1100; ++i) {
    const std::string key = "customer/account/region/" + std::to_string(i);
    runner->ExpectOk(builder.Add(key, "value"), "add compressible key");
    uncompressed_entries_size += 9 + key.size() + 5;
  }
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish compressible table");
  runner->Expect(metadata.file_size_bytes < uncompressed_entries_size,
                 "prefix compression reduces total table bytes");
}

void UsesRestartIndexForPointLookups(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path, 4096);
  for (int i = 0; i < 80; ++i) {
    const std::string key = "tenant/orders/2026/" + std::to_string(1000 + i);
    if (i == 47) {
      runner->ExpectOk(builder.AddDeletion(key), "add restart lookup tombstone");
    } else {
      runner->ExpectOk(builder.Add(key, "value-" + std::to_string(i)),
                       "add restart lookup value");
    }
  }
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish restart lookup table");
  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(status, "open restart lookup table");
  if (!reader) return;

  for (int i : {0, 15, 16, 31, 32, 46, 48, 63, 64, 79}) {
    auto [value, get_status] =
        reader->Get("tenant/orders/2026/" + std::to_string(1000 + i));
    runner->ExpectOk(get_status, "lookup across restart boundaries");
    runner->Expect(value == "value-" + std::to_string(i),
                   "restart lookup returns exact value");
  }
  const auto deleted = reader->Lookup("tenant/orders/2026/1047");
  runner->Expect(deleted.status.ok() && deleted.found && deleted.deleted,
                 "restart lookup returns tombstone");
  runner->Expect(!reader->Lookup("tenant/orders/2026/10315").found,
                 "restart lookup rejects key between entries");
}

void DetectsCorruptionInSearchedRestartKey(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path, 512);
  for (int i = 0; i < 96; ++i) {
    runner->ExpectOk(builder.Add("key-" + std::to_string(1000 + i), "value"),
                     "add restart corruption value");
  }
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish restart corruption table");
  auto [reader, open_status] = stratakv::SSTableReader::Open(table_path);
  runner->ExpectOk(open_status, "open restart corruption table");
  if (!reader) return;

  std::string file = ReadFile(table_path);
  constexpr std::size_t kFooterSize = 56;
  const std::size_t index_offset = static_cast<std::size_t>(
      ReadFixed(file, file.size() - kFooterSize + 8, 8));
  std::size_t index_cursor = index_offset;
  const std::size_t first_key_size =
      static_cast<std::size_t>(ReadFixed(file, index_cursor, 4));
  index_cursor += 4 + first_key_size + 8 + 8 + 8;
  const std::size_t second_key_size =
      static_cast<std::size_t>(ReadFixed(file, index_cursor, 4));
  const std::string lookup_key = file.substr(index_cursor + 4, second_key_size);
  index_cursor += 4 + second_key_size;
  const std::size_t block_offset =
      static_cast<std::size_t>(ReadFixed(file, index_cursor, 8));
  const std::size_t block_size =
      static_cast<std::size_t>(ReadFixed(file, index_cursor + 8, 8));
  const std::size_t data_end = block_offset + block_size - 4;
  const std::size_t restart_count =
      static_cast<std::size_t>(ReadFixed(file, data_end - 4, 4));
  const std::size_t restart_array = data_end - 4 - restart_count * 4;
  runner->Expect(restart_count >= 2,
                 "second block contains multiple restart points");
  if (restart_count < 2) return;
  const std::size_t corrupt_restart = block_offset +
      static_cast<std::size_t>(ReadFixed(file, restart_array + 4, 4));
  file[corrupt_restart] = 1;  // Restart entries must have zero shared bytes.
  const std::string block_data =
      file.substr(block_offset, block_size - sizeof(std::uint32_t));
  const std::uint32_t checksum = Checksum(block_data);
  for (std::size_t i = 0; i < 4; ++i) {
    file[block_offset + block_size - 4 + i] =
        static_cast<char>((checksum >> (8 * i)) & 0xffU);
  }
  std::ofstream stream(table_path, std::ios::binary | std::ios::trunc);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  const auto lookup = reader->Lookup(lookup_key);
  runner->Expect(lookup.status.code() == stratakv::Status::Code::kCorruption,
                 "point lookup validates searched restart keys");
}

void BloomFilterSkipsAbsentDataBlocks(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path, 64);
  for (int i = 0; i < 100; ++i) {
    const std::string key = "key-" + std::to_string(1000 + i * 2);
    runner->ExpectOk(builder.Add(key, "value"), "add Bloom filter key");
  }
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish Bloom filter table");
  auto cache = std::make_shared<stratakv::BlockCache>(1 << 20);
  auto [reader, status] = stratakv::SSTableReader::Open(table_path, cache);
  runner->ExpectOk(status, "open Bloom filter table");
  if (!reader) return;

  const auto misses_before = cache->stats().misses;
  const auto missing = reader->Lookup("key-1197");
  runner->Expect(missing.status.ok() && !missing.found,
                 "Bloom filter rejects an absent in-range key");
  runner->Expect(cache->stats().misses == misses_before,
                 "negative lookup avoids reading its data block");

  auto [value, get_status] = reader->Get("key-1198");
  runner->ExpectOk(get_status, "Bloom filter preserves present keys");
  runner->Expect(value == "value", "present key survives Bloom filtering");
  runner->Expect(cache->stats().misses == misses_before + 1,
                 "present lookup reads the selected data block");
}

void DetectsCorruptBloomFilter(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path);
  runner->ExpectOk(builder.Add("alpha", "one"), "add filtered key");
  runner->ExpectOk(builder.Add("beta", "two"), "add second filtered key");
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish filtered table");

  std::string file = ReadFile(table_path);
  constexpr std::size_t kFooterSize = 56;
  const std::size_t footer = file.size() - kFooterSize;
  const std::size_t filter_offset =
      static_cast<std::size_t>(ReadFixed(file, footer + 28, 8));
  runner->Expect(filter_offset < footer, "filter block precedes footer");
  if (filter_offset >= footer) return;
  file[filter_offset] ^= 0x01;
  std::ofstream stream(table_path, std::ios::binary | std::ios::trunc);
  stream.write(file.data(), static_cast<std::streamsize>(file.size()));
  stream.close();

  auto [reader, status] = stratakv::SSTableReader::Open(table_path);
  (void)reader;
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "corrupted Bloom filter fails checksum verification");
}

void LazilyReadsAndCachesDataBlocks(TestRunner* runner) {
  TempDir dir;
  const auto table_path = dir.path() / "000001.sst";
  stratakv::SSTableBuilder builder(table_path, 24);
  for (int i = 0; i < 8; ++i) {
    runner->ExpectOk(builder.Add("key-" + std::to_string(i),
                                 "value-" + std::to_string(i)),
                     "add cache test entry");
  }
  stratakv::TableMetadata metadata;
  runner->ExpectOk(builder.Finish(&metadata), "finish cache test table");

  auto cache = std::make_shared<stratakv::BlockCache>(64);
  auto [reader, status] = stratakv::SSTableReader::Open(table_path, cache);
  runner->ExpectOk(status, "open lazy table");
  if (!reader) return;
  const auto after_first_open = cache->stats();
  runner->Expect(after_first_open.misses == 1,
                 "first reader records a shared-cache miss");
  auto [second_reader, second_status] =
      stratakv::SSTableReader::Open(table_path, cache);
  runner->ExpectOk(second_status, "open second reader with shared cache");
  runner->Expect(cache->stats().hits == 1,
                 "second reader reuses the cached block");
  std::error_code ec;
  std::filesystem::remove(table_path, ec);
  runner->Expect(!ec, "remove table after opening index");

  auto [first_value, first_status] = reader->Get("key-0");
  runner->ExpectOk(first_status, "cached first block remains readable");
  runner->Expect(first_value == "value-0", "cached first block value");
  auto [second_value, second_get_status] = second_reader->Get("key-0");
  runner->ExpectOk(second_get_status, "shared cached block remains readable");
  runner->Expect(second_value == "value-0", "shared cached block value");
  auto [later_value, later_status] = reader->Get("key-7");
  (void)later_value;
  runner->Expect(later_status.code() == stratakv::Status::Code::kIOError,
                 "uncached block is read lazily from the table file");
}

void EnforcesSharedCacheBudget(TestRunner* runner) {
  stratakv::BlockCache cache(64);
  auto first = std::make_shared<const std::string>(40, 'a');
  auto second = std::make_shared<const std::string>(40, 'b');
  cache.Insert("000001.sst", 0, 40, first);
  cache.Insert("000002.sst", 0, 40, second);
  const auto stats = cache.stats();
  runner->Expect(stats.evictions == 1, "shared cache evicts across tables");
  runner->Expect(stats.usage_bytes == 40, "shared cache respects byte budget");
  runner->Expect(stats.capacity_bytes == 64,
                 "shared cache reports configured capacity");
  runner->Expect(cache.Lookup("000001.sst", 0, 40) == nullptr,
                 "least-recently-used block was evicted");
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
  WritesGoldenPrefixCompressedBlock(&runner);
  DetectsCorruptRestartArray(&runner);
  ReadsLegacyIndexedTable(&runner);
  ReadsLegacyCompressedTable(&runner);
  CompressesSharedKeyPrefixes(&runner);
  UsesRestartIndexForPointLookups(&runner);
  DetectsCorruptionInSearchedRestartKey(&runner);
  BloomFilterSkipsAbsentDataBlocks(&runner);
  DetectsCorruptBloomFilter(&runner);
  LazilyReadsAndCachesDataBlocks(&runner);
  EnforcesSharedCacheBudget(&runner);
  return runner.Finish();
}
