#include "memtable.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace stratakv {
namespace {

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(std::vector<std::pair<std::string, std::string>> rows)
      : rows_(std::move(rows)) {}

  bool Valid() const override { return index_ < rows_.size(); }

  void SeekToFirst() override { index_ = 0; }

  void Seek(std::string_view target) override {
    const auto it = std::lower_bound(
        rows_.begin(), rows_.end(), target,
        [](const auto& row, std::string_view key) { return row.first < key; });
    index_ = static_cast<std::size_t>(it - rows_.begin());
  }

  void Next() override {
    if (Valid()) {
      ++index_;
    }
  }

  std::string_view key() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].first;
  }

  std::string_view value() const override {
    if (!Valid()) {
      return {};
    }
    return rows_[index_].second;
  }

  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::pair<std::string, std::string>> rows_;
  std::size_t index_ = 0;
};

}  // namespace

std::size_t MemTable::EntrySize(std::string_view key, const Entry& entry) {
  return key.size() + entry.value.size() + sizeof(entry.sequence) +
         sizeof(entry.type);
}

Status MemTable::Put(std::uint64_t sequence, std::string_view key,
                     std::string_view value) {
  Entry next{ValueType::kValue, std::string(value), sequence};
  const std::string stored_key(key);

  const auto existing = entries_.find(stored_key);
  if (existing != entries_.end()) {
    approximate_memory_usage_ -= EntrySize(existing->first, existing->second);
  }

  approximate_memory_usage_ += EntrySize(stored_key, next);
  entries_[stored_key] = std::move(next);
  return Status::OK();
}

Status MemTable::Delete(std::uint64_t sequence, std::string_view key) {
  Entry next{ValueType::kDeletion, std::string(), sequence};
  const std::string stored_key(key);

  const auto existing = entries_.find(stored_key);
  if (existing != entries_.end()) {
    approximate_memory_usage_ -= EntrySize(existing->first, existing->second);
  }

  approximate_memory_usage_ += EntrySize(stored_key, next);
  entries_[stored_key] = std::move(next);
  return Status::OK();
}

Status MemTable::Apply(const LogRecord& record) {
  switch (record.type) {
    case RecordType::kPut:
      return Put(record.sequence, record.key, record.value);
    case RecordType::kDelete:
      return Delete(record.sequence, record.key);
  }

  return Status::Corruption("unknown WAL record type");
}

std::pair<std::string, Status> MemTable::Get(std::string_view key) const {
  const auto it = entries_.find(std::string(key));
  if (it == entries_.end() || it->second.type == ValueType::kDeletion) {
    return {"", Status::NotFound("key not found")};
  }

  return {it->second.value, Status::OK()};
}

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  std::vector<std::pair<std::string, std::string>> rows;
  rows.reserve(entries_.size());

  for (const auto& [key, entry] : entries_) {
    if (entry.type == ValueType::kValue) {
      rows.emplace_back(key, entry.value);
    }
  }

  return std::make_unique<MemTableIterator>(std::move(rows));
}

std::size_t MemTable::ApproximateMemoryUsage() const {
  return approximate_memory_usage_;
}

}  // namespace stratakv
