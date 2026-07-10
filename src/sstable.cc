#include "sstable.h"

#include <utility>

namespace stratakv {

SSTableBuilder::SSTableBuilder(std::filesystem::path path)
    : path_(std::move(path)) {}

Status SSTableBuilder::Add(std::string_view key, std::string_view value) {
  (void)key;
  (void)value;
  return Status::NotSupported("SSTable writing starts in Phase 1 milestone 2");
}

Status SSTableBuilder::Finish(TableMetadata* metadata) {
  (void)metadata;
  return Status::NotSupported("SSTable writing starts in Phase 1 milestone 2");
}

std::pair<std::unique_ptr<SSTableReader>, Status> SSTableReader::Open(
    std::filesystem::path path) {
  (void)path;
  return {nullptr,
          Status::NotSupported("SSTable reading starts in Phase 1 milestone 2")};
}

std::pair<std::string, Status> SSTableReader::Get(std::string_view key) const {
  (void)key;
  return {"", Status::NotSupported("SSTable reading is not implemented yet")};
}

std::unique_ptr<Iterator> SSTableReader::NewIterator() const { return nullptr; }

SSTableReader::SSTableReader(std::filesystem::path path)
    : path_(std::move(path)) {}

}  // namespace stratakv
