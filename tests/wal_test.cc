#include "wal.h"
#include "stratakv/file_system.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("stratakv-wal-test-" + std::to_string(now));
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

class RecordingFileSystem final : public stratakv::FileSystem {
 public:
  stratakv::Status SyncFile(const std::filesystem::path& path) override {
    synced_files.push_back(path.filename().string());
    return delegate_->SyncFile(path);
  }
  stratakv::Status Rename(const std::filesystem::path& from,
                          const std::filesystem::path& to) override {
    return delegate_->Rename(from, to);
  }
  stratakv::Status SyncDirectory(
      const std::filesystem::path& path) override {
    return delegate_->SyncDirectory(path);
  }
  stratakv::Status Remove(const std::filesystem::path& path) override {
    return delegate_->Remove(path);
  }

  std::vector<std::string> synced_files;

 private:
  std::shared_ptr<stratakv::FileSystem> delegate_ =
      stratakv::DefaultFileSystem();
};

class FailingWalIOFileSystem final : public stratakv::FileSystem {
 public:
  stratakv::Status SyncFile(const std::filesystem::path& path) override {
    return delegate_->SyncFile(path);
  }
  stratakv::Status Rename(const std::filesystem::path& from,
                          const std::filesystem::path& to) override {
    return delegate_->Rename(from, to);
  }
  stratakv::Status SyncDirectory(const std::filesystem::path& path) override {
    return delegate_->SyncDirectory(path);
  }
  stratakv::Status Remove(const std::filesystem::path& path) override {
    return delegate_->Remove(path);
  }
  stratakv::Status OpenWritableFile(const std::filesystem::path& path,
                                    bool append) override {
    return delegate_->OpenWritableFile(path, append);
  }
  stratakv::Status AppendFile(const std::filesystem::path&,
                              std::string_view) override {
    return stratakv::Status::IOError("injected WAL append failure");
  }
  stratakv::Status ReadFile(const std::filesystem::path& path,
                            std::uint64_t offset, std::size_t size,
                            std::string* data) override {
    if (fail_reads) {
      return stratakv::Status::IOError("injected WAL read failure");
    }
    return delegate_->ReadFile(path, offset, size, data);
  }

  bool fail_reads = false;

 private:
  std::shared_ptr<stratakv::FileSystem> delegate_ =
      stratakv::DefaultFileSystem();
};

void AppendRecord(TestRunner* runner, const std::filesystem::path& path,
                  const stratakv::LogRecord& record, bool append = true) {
  stratakv::WalWriter writer(path);
  runner->ExpectOk(writer.Open(append), "open WAL writer");
  runner->ExpectOk(writer.Append(record), "append WAL record");
  runner->ExpectOk(writer.Sync(), "sync WAL writer");
}

std::vector<stratakv::LogRecord> Replay(const std::filesystem::path& path,
                                        stratakv::Status* status) {
  std::vector<stratakv::LogRecord> records;
  stratakv::WalReader reader(path);
  *status = reader.Replay([&](const stratakv::LogRecord& record) {
    records.push_back(record);
    return stratakv::Status::OK();
  });
  return records;
}

void ReplaysCompleteRecords(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";

  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha",
                                    "one"},
               /*append=*/false);
  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kDelete, 2, "beta",
                                    ""},
               /*append=*/true);

  stratakv::Status status;
  const auto records = Replay(path, &status);
  runner->ExpectOk(status, "replay complete WAL");
  runner->Expect(records.size() == 2, "two complete records should replay");
  if (records.size() != 2) {
    return;
  }
  runner->Expect(records[0].key == "alpha", "first record key");
  runner->Expect(records[0].value == "one", "first record value");
  runner->Expect(records[1].type == stratakv::RecordType::kDelete,
                 "second record type");
  runner->Expect(records[1].key == "beta", "second record key");
}

void IgnoresTornTrailingHeader(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";

  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha",
                                    "one"},
               /*append=*/false);

  std::ofstream stream(path, std::ios::binary | std::ios::app);
  stream.write("abc", 3);
  stream.close();
  runner->Expect(!stream.fail(), "append torn WAL header");

  stratakv::Status status;
  const auto records = Replay(path, &status);
  runner->ExpectOk(status, "replay WAL with torn trailing header");
  runner->Expect(records.size() == 1, "torn header should not replay");
  if (records.size() != 1) {
    return;
  }
  runner->Expect(records[0].key == "alpha", "complete record survives");
}

void IgnoresTornTrailingPayload(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";

  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha",
                                    "one"},
               /*append=*/false);
  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 2, "beta",
                                    "two"},
               /*append=*/true);

  const auto size_before_truncation = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size_before_truncation - 2);

  stratakv::Status status;
  const auto records = Replay(path, &status);
  runner->ExpectOk(status, "replay WAL with torn trailing payload");
  runner->Expect(records.size() == 1, "torn payload should not replay");
  if (records.size() != 1) {
    return;
  }
  runner->Expect(records[0].key == "alpha", "complete prefix survives");
}

void DetectsChecksumMismatch(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";

  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha",
                                    "one"},
               /*append=*/false);

  std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
  char byte = 0;
  stream.seekg(8);
  stream.get(byte);
  stream.seekp(8);
  stream.put(static_cast<char>(byte ^ 0x01));
  stream.close();
  runner->Expect(!stream.fail(), "flip WAL payload byte");

  stratakv::Status status;
  const auto records = Replay(path, &status);
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "checksum mismatch should be corruption");
  runner->Expect(records.empty(), "corrupt record should not replay");
}

void SyncsWalFile(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";
  auto file_system = std::make_shared<RecordingFileSystem>();
  stratakv::WalWriter writer(path, file_system);
  runner->ExpectOk(writer.Open(/*append=*/false), "open synced WAL");
  runner->ExpectOk(
      writer.Append(stratakv::LogRecord{stratakv::RecordType::kPut, 1,
                                        "alpha", "one"}),
      "append synced WAL record");
  runner->ExpectOk(writer.Sync(), "durably sync WAL");
  runner->Expect(file_system->synced_files ==
                     std::vector<std::string>({"current.log"}),
                 "WAL sync should reach the filesystem durability boundary");
}

void RejectsOversizedRecordsBeforeRecoveryAllocation(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  const char header[] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  stream.write(header, sizeof(header));
  stream.close();

  std::vector<stratakv::LogRecord> records;
  stratakv::WalReader reader(path, stratakv::DefaultFileSystem(), 128);
  const stratakv::Status status = reader.Replay(
      [&](const stratakv::LogRecord& record) {
        records.push_back(record);
        return stratakv::Status::OK();
      });
  runner->Expect(status.code() == stratakv::Status::Code::kCorruption,
                 "oversized recovery record should be corruption");
  runner->Expect(records.empty(), "oversized record should not replay");

  stratakv::WalWriter writer(path, stratakv::DefaultFileSystem(), 20);
  runner->ExpectOk(writer.Open(/*append=*/false), "open bounded WAL writer");
  const stratakv::Status write_status = writer.Append(
      stratakv::LogRecord{stratakv::RecordType::kPut, 1, "key", "value"});
  runner->Expect(write_status.code() ==
                     stratakv::Status::Code::kInvalidArgument,
                 "oversized write should be rejected consistently");
}

void PropagatesInjectedWalIOFailures(TestRunner* runner) {
  TempDir dir;
  const auto path = dir.path() / "current.log";
  auto file_system = std::make_shared<FailingWalIOFileSystem>();
  stratakv::WalWriter writer(path, file_system);
  runner->ExpectOk(writer.Open(/*append=*/false), "open fault-injected WAL");
  const stratakv::Status append_status = writer.Append(
      stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha", "one"});
  runner->Expect(append_status.code() == stratakv::Status::Code::kIOError,
                 "injected append failure should propagate");

  AppendRecord(runner, path,
               stratakv::LogRecord{stratakv::RecordType::kPut, 1, "alpha",
                                    "one"},
               /*append=*/false);
  file_system->fail_reads = true;
  stratakv::WalReader reader(path, file_system);
  const stratakv::Status read_status = reader.Replay(
      [](const stratakv::LogRecord&) { return stratakv::Status::OK(); });
  runner->Expect(read_status.code() == stratakv::Status::Code::kIOError,
                 "injected read failure should propagate");
}

}  // namespace

int main() {
  TestRunner runner;
  ReplaysCompleteRecords(&runner);
  IgnoresTornTrailingHeader(&runner);
  IgnoresTornTrailingPayload(&runner);
  DetectsChecksumMismatch(&runner);
  SyncsWalFile(&runner);
  RejectsOversizedRecordsBeforeRecoveryAllocation(&runner);
  PropagatesInjectedWalIOFailures(&runner);
  return runner.Finish();
}
