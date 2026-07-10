#pragma once

#include <cstdint>
#include <string>

namespace stratakv {

enum class RecordType : std::uint8_t {
  kPut = 1,
  kDelete = 2,
};

struct LogRecord {
  RecordType type = RecordType::kPut;
  std::uint64_t sequence = 0;
  std::string key;
  std::string value;
};

}  // namespace stratakv
