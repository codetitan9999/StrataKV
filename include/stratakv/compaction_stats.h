#pragma once

#include <cstdint>

namespace stratakv {

struct CompactionStats {
  std::uint64_t jobs = 0;
  std::uint64_t input_files = 0;
  std::uint64_t output_files = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t elapsed_nanoseconds = 0;
};

}  // namespace stratakv
