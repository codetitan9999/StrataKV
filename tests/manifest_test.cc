#include "manifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "stratakv/file_system.h"

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

class RecordingFileSystem final : public stratakv::FileSystem {
 public:
  stratakv::Status SyncFile(const std::filesystem::path& path) override {
    calls.push_back("file:" + path.filename().string());
    return delegate_->SyncFile(path);
  }

  stratakv::Status Rename(const std::filesystem::path& from,
                          const std::filesystem::path& to) override {
    calls.push_back("rename:" + from.filename().string() + ":" +
                    to.filename().string());
    if (fail_rename) {
      return stratakv::Status::IOError("injected rename failure");
    }
    return delegate_->Rename(from, to);
  }

  stratakv::Status SyncDirectory(const std::filesystem::path& path) override {
    calls.push_back("dir:" + path.filename().string());
    if (fail_directory_sync) {
      return stratakv::Status::IOError("injected directory sync failure");
    }
    return delegate_->SyncDirectory(path);
  }

  stratakv::Status Remove(const std::filesystem::path& path) override {
    calls.push_back("remove:" + path.filename().string());
    return delegate_->Remove(path);
  }

  bool fail_rename = false;
  bool fail_directory_sync = false;
  std::vector<std::string> calls;

 private:
  std::shared_ptr<stratakv::FileSystem> delegate_ =
      stratakv::DefaultFileSystem();
};

stratakv::TableMetadata Metadata(std::uint64_t file_number,
                                 std::string smallest_key,
                                 std::string largest_key,
                                 std::uint32_t level = 0) {
  stratakv::TableMetadata metadata;
  metadata.file_number = file_number;
  metadata.level = level;
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
  runner->ExpectOk(reader.Replay([&](const stratakv::ManifestEdit& edit) {
                     runner->Expect(
                         edit.type == stratakv::ManifestEditType::kTableAdded,
                         "manifest edit should be table-added");
                     file_numbers.push_back(edit.file_number);
                     return stratakv::Status::OK();
                   }),
                   "replay manifest");

  runner->Expect(file_numbers == std::vector<std::uint64_t>({1, 2}),
                 "manifest should replay table records in append order");
}

void PersistsTableLevels(TestRunner* runner) {
  TempDir dir;
  const auto manifest_path = dir.path() / "MANIFEST";
  stratakv::ManifestWriter writer(manifest_path);
  runner->ExpectOk(writer.Open(/*append=*/false), "open leveled manifest");
  runner->ExpectOk(writer.AppendTable(Metadata(7, "a", "z", 2)),
                   "append leveled table");
  runner->ExpectOk(writer.Sync(), "sync leveled manifest");

  std::uint32_t replayed_level = 0;
  stratakv::ManifestReader reader(manifest_path);
  runner->ExpectOk(reader.Replay([&](const stratakv::ManifestEdit& edit) {
                     replayed_level = edit.table.level;
                     return stratakv::Status::OK();
                   }),
                   "replay leveled manifest");
  runner->Expect(replayed_level == 2, "manifest should preserve table level");
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

void ReplaysTableDeletions(TestRunner* runner) {
  TempDir dir;
  const auto manifest_path = dir.path() / "MANIFEST";

  stratakv::ManifestWriter writer(manifest_path);
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
  runner->ExpectOk(writer.AppendTable(Metadata(1, "a", "c")),
                   "append first table");
  runner->ExpectOk(writer.AppendTable(Metadata(2, "d", "f")),
                   "append second table");
  runner->ExpectOk(writer.DeleteTable(1), "delete first table");
  runner->ExpectOk(writer.Sync(), "sync manifest");

  std::map<std::uint64_t, bool> active;
  stratakv::ManifestReader reader(manifest_path);
  runner->ExpectOk(reader.Replay([&](const stratakv::ManifestEdit& edit) {
                     if (edit.type == stratakv::ManifestEditType::kTableAdded) {
                       active[edit.file_number] = true;
                     } else {
                       active.erase(edit.file_number);
                     }
                     return stratakv::Status::OK();
                   }),
                   "replay manifest with deletion");

  runner->Expect(active.size() == 1 && active.count(2) == 1,
                 "manifest deletion should remove table one");
}

void RejectsInvalidDeletions(TestRunner* runner) {
  TempDir dir;
  stratakv::ManifestWriter writer(dir.path() / "MANIFEST");
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");

  const stratakv::Status status = writer.DeleteTable(0);
  runner->Expect(status.code() == stratakv::Status::Code::kInvalidArgument,
                 "manifest should reject invalid table deletions");
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
      reader.Replay([](const stratakv::ManifestEdit&) {
        return stratakv::Status::OK();
      });
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "manifest checksum mismatch should be detected");
}

void ReplacesHistoryWithSnapshot(TestRunner* runner) {
  TempDir dir;
  const auto manifest_path = dir.path() / "MANIFEST";

  stratakv::ManifestWriter writer(manifest_path);
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
  runner->ExpectOk(writer.AppendTable(Metadata(1, "a", "c")),
                   "append obsolete table");
  runner->ExpectOk(writer.DeleteTable(1), "delete obsolete table");
  runner->ExpectOk(
      writer.ReplaceWithSnapshot({Metadata(2, "d", "f")}),
      "replace manifest with snapshot");
  runner->ExpectOk(writer.AppendTable(Metadata(3, "g", "i")),
                   "append after snapshot replacement");
  runner->ExpectOk(writer.Sync(), "sync appended manifest edit");

  std::vector<std::uint64_t> file_numbers;
  stratakv::ManifestReader reader(manifest_path);
  runner->ExpectOk(reader.Replay([&](const stratakv::ManifestEdit& edit) {
                     runner->Expect(
                         edit.type == stratakv::ManifestEditType::kTableAdded,
                         "snapshot should contain only table additions");
                     file_numbers.push_back(edit.file_number);
                     return stratakv::Status::OK();
                   }),
                   "replay compacted manifest");

  runner->Expect(file_numbers == std::vector<std::uint64_t>({2, 3}),
                 "snapshot should replace obsolete manifest history");
  runner->Expect(!std::filesystem::exists(manifest_path.string() + ".tmp"),
                 "installed snapshot should not leave a temporary file");
}

void SyncsSnapshotInstallation(TestRunner* runner) {
  TempDir dir;
  auto file_system = std::make_shared<RecordingFileSystem>();
  stratakv::ManifestWriter writer(dir.path() / "MANIFEST", file_system);
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
  runner->ExpectOk(writer.ReplaceWithSnapshot({Metadata(1, "a", "z")}),
                   "durably replace manifest snapshot");
  runner->Expect(
      file_system->calls ==
          std::vector<std::string>({"file:MANIFEST.tmp",
                                    "rename:MANIFEST.tmp:MANIFEST",
                                    "dir:" + dir.path().filename().string()}),
      "snapshot should sync file, rename, then sync parent directory");
}

void PreservesManifestWhenSnapshotRenameFails(TestRunner* runner) {
  TempDir dir;
  auto file_system = std::make_shared<RecordingFileSystem>();
  stratakv::ManifestWriter writer(dir.path() / "MANIFEST", file_system);
  runner->ExpectOk(writer.Open(/*append=*/false), "open manifest writer");
  runner->ExpectOk(writer.AppendTable(Metadata(1, "a", "c")),
                   "append original manifest table");
  runner->ExpectOk(writer.Sync(), "sync original manifest");

  file_system->fail_rename = true;
  const stratakv::Status status =
      writer.ReplaceWithSnapshot({Metadata(2, "d", "f")});
  runner->Expect(!status.ok(), "injected rename failure should be reported");
  runner->Expect(!std::filesystem::exists(dir.path() / "MANIFEST.tmp"),
                 "failed snapshot should clean up its temporary file");

  std::vector<std::uint64_t> files;
  stratakv::ManifestReader reader(dir.path() / "MANIFEST");
  runner->ExpectOk(reader.Replay([&](const stratakv::ManifestEdit& edit) {
                     files.push_back(edit.file_number);
                     return stratakv::Status::OK();
                   }),
                   "replay original manifest after failed rename");
  runner->Expect(files == std::vector<std::uint64_t>({1}),
                 "failed rename should leave old manifest authoritative");
}

}  // namespace

int main() {
  TestRunner runner;
  ReplaysAppendedTables(&runner);
  PersistsTableLevels(&runner);
  RejectsInvalidMetadata(&runner);
  ReplaysTableDeletions(&runner);
  RejectsInvalidDeletions(&runner);
  DetectsChecksumMismatch(&runner);
  ReplacesHistoryWithSnapshot(&runner);
  SyncsSnapshotInstallation(&runner);
  PreservesManifestWhenSnapshotRenameFails(&runner);
  return runner.Finish();
}
