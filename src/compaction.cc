#include "compaction.h"

#include <memory>
#include <queue>
#include <string>
#include <utility>

namespace stratakv {

CompactionJob::CompactionJob(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {}

Status CompactionJob::Run(const CompactionInput& input,
                          const CompactionOutputSink& sink) {
  if (!sink.add || !sink.finish) {
    return Status::InvalidArgument("compaction output sink must not be null");
  }

  struct Source {
    std::unique_ptr<InternalIterator> iterator;
    std::size_t precedence = 0;
  };
  struct HeapEntry {
    std::string key;
    std::size_t source = 0;
  };
  const auto later_key = [](const HeapEntry& left, const HeapEntry& right) {
    if (left.key != right.key) return left.key > right.key;
    return left.source < right.source;
  };

  std::vector<Source> sources;
  sources.reserve(input.tables.size());
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(later_key)>
      heap(later_key);
  for (std::size_t index = 0; index < input.tables.size(); ++index) {
    const SSTableReader* table = input.tables[index];
    if (table == nullptr) {
      return Status::InvalidArgument("compaction input table must not be null");
    }
    auto iterator = table->NewEntryIterator();
    iterator->SeekToFirst();
    if (!iterator->status().ok()) return iterator->status();
    sources.push_back(Source{std::move(iterator), index});
    if (sources.back().iterator->Valid()) {
      heap.push(HeapEntry{std::string(sources.back().iterator->key()), index});
    }
  }

  std::size_t output_bytes = 0;
  while (!heap.empty()) {
    const std::string key = heap.top().key;
    TableEntry entry;
    std::size_t winning_precedence = 0;
    bool have_entry = false;
    while (!heap.empty() && heap.top().key == key) {
      const std::size_t source_index = heap.top().source;
      heap.pop();
      Source& source = sources[source_index];
      if (!have_entry || source.precedence >= winning_precedence) {
        entry = TableEntry{source.iterator->type(),
                           std::string(source.iterator->key()),
                           std::string(source.iterator->value())};
        winning_precedence = source.precedence;
        have_entry = true;
      }
      source.iterator->Next();
      if (!source.iterator->status().ok()) return source.iterator->status();
      if (source.iterator->Valid()) {
        heap.push(HeapEntry{std::string(source.iterator->key()), source_index});
      }
    }

    if (entry.type == RecordType::kDelete && input.drop_tombstones) continue;
    const std::size_t entry_bytes = key.size() + entry.value.size() + 16;
    if (output_bytes > 0 && input.max_output_file_size > 0 &&
        output_bytes + entry_bytes > input.max_output_file_size) {
      Status status = sink.finish();
      if (!status.ok()) return status;
      output_bytes = 0;
    }
    Status status = sink.add(entry);
    if (!status.ok()) return status;
    output_bytes += entry_bytes;
  }

  if (output_bytes > 0) return sink.finish();

  return Status::OK();
}

}  // namespace stratakv
