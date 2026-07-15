#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "record.h"
#include "stratakv/iterator.h"
#include "stratakv/status.h"

namespace stratakv {

struct MemTableEntry {
  RecordType type = RecordType::kPut;
  std::uint64_t sequence = 0;
  std::string key;
  std::string value;
};

struct MemTableLookup {
  bool found = false;
  bool deleted = false;
  std::string value;
};

class MemTable {
 public:
  Status Put(std::uint64_t sequence, std::string_view key,
             std::string_view value);
  Status Delete(std::uint64_t sequence, std::string_view key);
  Status Apply(const LogRecord& record);

  [[nodiscard]] MemTableLookup Lookup(std::string_view key) const;
  [[nodiscard]] std::pair<std::string, Status> Get(std::string_view key) const;
  [[nodiscard]] std::vector<MemTableEntry> Snapshot() const;
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;
  [[nodiscard]] std::size_t ApproximateMemoryUsage() const;
  [[nodiscard]] bool empty() const;
  void Clear();

 private:
  enum class ValueType {
    kValue,
    kDeletion,
  };

  struct Entry {
    ValueType type = ValueType::kValue;
    std::string value;
    std::uint64_t sequence = 0;
  };

  [[nodiscard]] static std::size_t EntrySize(std::string_view key,
                                             const Entry& entry);

  std::map<std::string, Entry> entries_;
  std::size_t approximate_memory_usage_ = 0;
};

}  // namespace stratakv
