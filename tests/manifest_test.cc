#include "manifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("stratakv-manifest-test-" + std::to_string(now));
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
      std::cout << "All manifest tests passed\n";
      return 0;
    }

    std::cerr << failures_ << " manifest test expectation(s) failed\n";
    return 1;
  }

 private:
  int failures_ = 0;
};

stratakv::TableMetadata Metadata(std::uint64_t file_number,
                                 std::string smallest_key,
                                 std::string largest_key) {
  stratakv::TableMetadata metadata;
  metadata.file_number = file_number;
  metadata.file_path = std::to_string(file_number) + ".sst";
  metadata.smallest_key = std::move(smallest_key);
  metadata.largest_key = std::move(largest_key);
  metadata.entry_count = 3;
  metadata.file_size_bytes = 128;
  return metadata;
}

void ReplaysAppendedTables(TestRunner* runner) {
  TempDir dir;
  const auto manifest_path = dir.path() / "MANIFEST";

  stratakv::ManifestWriter writer(manifest_path);
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
  runner->ExpectOk(writer.AppendTable(Metadata(1, "a", "c")),
                   "append first table");
  runner->ExpectOk(writer.AppendTable(Metadata(2, "d", "f")),
                   "append second table");
  runner->ExpectOk(writer.Sync(), "sync manifest");

  std::vector<std::uint64_t> file_numbers;
  stratakv::ManifestReader reader(manifest_path);
  runner->ExpectOk(reader.Replay([&](const stratakv::TableMetadata& metadata) {
                     file_numbers.push_back(metadata.file_number);
                     return stratakv::Status::OK();
                   }),
                   "replay manifest");

  runner->Expect(file_numbers == std::vector<std::uint64_t>({1, 2}),
                 "manifest should replay table records in append order");
}

void RejectsInvalidMetadata(TestRunner* runner) {
  TempDir dir;
  stratakv::ManifestWriter writer(dir.path() / "MANIFEST");
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");

  stratakv::TableMetadata metadata = Metadata(0, "a", "c");
  const stratakv::Status status = writer.AppendTable(metadata);
  runner->Expect(status.code() == stratakv::Status::Code::kInvalidArgument,
                 "manifest should reject invalid file numbers");
}

void DetectsChecksumMismatch(TestRunner* runner) {
  TempDir dir;
  const auto manifest_path = dir.path() / "MANIFEST";

  {
    stratakv::ManifestWriter writer(manifest_path);
    runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
    runner->ExpectOk(writer.AppendTable(Metadata(1, "a", "c")),
                     "append table");
    runner->ExpectOk(writer.Sync(), "sync manifest");
  }

  std::fstream stream(manifest_path,
                      std::ios::binary | std::ios::in | std::ios::out);
  char byte = 0;
  stream.seekg(8);
  stream.read(&byte, 1);
  byte ^= 0x7f;
  stream.seekp(8);
  stream.write(&byte, 1);
  stream.close();

  stratakv::ManifestReader reader(manifest_path);
  const stratakv::Status status =
      reader.Replay([](const stratakv::TableMetadata&) {
        return stratakv::Status::OK();
      });
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "manifest checksum mismatch should be detected");
}

}  // namespace

int main() {
  TestRunner runner;
  ReplaysAppendedTables(&runner);
  RejectsInvalidMetadata(&runner);
  DetectsChecksumMismatch(&runner);
  return runner.Finish();
}
