#pragma once

#include <cstdint>
#include <vector>

namespace stratakv {

struct CompactionLevelStats {
  std::uint32_t input_level = 0;
  std::uint32_t output_level = 0;
  std::uint64_t jobs = 0;
  std::uint64_t input_files = 0;
  std::uint64_t output_files = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t elapsed_nanoseconds = 0;
};

struct CompactionStats {
  std::uint64_t jobs = 0;
  std::uint64_t input_files = 0;
  std::uint64_t output_files = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t elapsed_nanoseconds = 0;
  std::vector<CompactionLevelStats> levels;
};

}  // namespace stratakv
