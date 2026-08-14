#include "version_set.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

stratakv::TableMetadata Table(std::uint64_t number, std::uint32_t level,
                              std::string smallest, std::string largest) {
  stratakv::TableMetadata table;
  table.file_number = number;
  table.level = level;
  table.smallest_key = std::move(smallest);
  table.largest_key = std::move(largest);
  table.entry_count = 1;
  return table;
}

}  // namespace

int main() {
  int failures = 0;
  auto expect = [&](bool condition, const char* message) {
    if (!condition) {
      ++failures;
      std::cerr << "FAIL: " << message << '\n';
    }
  };

  stratakv::VersionSet version;
  expect(version.AddTable(Table(1, 0, "b", "z")).ok(),
         "level zero table should be accepted");
  expect(version.AddTable(Table(2, 0, "a", "c")).ok(),
         "overlapping level zero table should be accepted");
  expect(version.AddTable(Table(3, 1, "a", "m")).ok(),
         "first sorted-level table should be accepted");
  expect(version.AddTable(Table(4, 1, "n", "z")).ok(),
         "disjoint sorted-level table should be accepted");
  expect(!version.AddTable(Table(5, 1, "m", "q")).ok(),
         "overlapping sorted-level table should be rejected");

  const auto ordered = version.TablesInReadOrder();
  std::vector<std::uint64_t> numbers;
  for (const auto& table : ordered) numbers.push_back(table.file_number);
  expect(numbers == std::vector<std::uint64_t>({2, 1, 3, 4}),
         "read order should be newest level zero then sorted higher levels");
  expect(version.LevelTableCount(0) == 2, "level zero count should be tracked");

  const auto selection = version.PickLevel0Compaction(1);
  numbers.clear();
  for (const auto& table : selection.inputs) numbers.push_back(table.file_number);
  expect(numbers == std::vector<std::uint64_t>({3, 4, 1}),
         "selection should include overlapping level one before oldest level zero");
  expect(selection.smallest_key == "b" && selection.largest_key == "z",
         "selection should report the level-zero key range");

  if (failures == 0) std::cout << "All version-set tests passed\n";
  return failures == 0 ? 0 : 1;
}
