#include "merge_iterator.h"

#include <queue>
#include <string>
#include <string_view>
#include <utility>

namespace stratakv {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(std::vector<MergeIteratorChild> children,
                  const ReadOptions& read_options)
      : children_(std::move(children)),
        lower_bound_(read_options.lower_bound),
        upper_bound_(read_options.upper_bound),
        prefix_(read_options.prefix) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    status_ = Status::OK();
    const std::string* start = StartBound();
    for (auto& child : children_) {
      if (start == nullptr) {
        child.iterator->SeekToFirst();
      } else {
        child.iterator->Seek(*start);
      }
    }
    RebuildFrontier();
    FindNext();
  }

  void Seek(std::string_view target) override {
    status_ = Status::OK();
    std::string_view start = target;
    if (const std::string* bound = StartBound();
        bound != nullptr && start < *bound) {
      start = *bound;
    }
    for (auto& child : children_) child.iterator->Seek(start);
    RebuildFrontier();
    FindNext();
  }

  void Next() override {
    if (!valid_) return;
    for (std::size_t index : current_sources_) {
      children_[index].iterator->Next();
      AddToFrontier(index);
    }
    FindNext();
  }

  std::string_view key() const override { return valid_ ? key_ : std::string_view{}; }
  std::string_view value() const override {
    return valid_ ? value_ : std::string_view{};
  }
  Status status() const override { return status_; }

 private:
  const std::string* StartBound() const {
    if (!lower_bound_) return prefix_ ? &*prefix_ : nullptr;
    if (!prefix_) return &*lower_bound_;
    return *lower_bound_ < *prefix_ ? &*prefix_ : &*lower_bound_;
  }

  bool MatchesPrefix(std::string_view key) const {
    return !prefix_ || key.starts_with(*prefix_);
  }

  bool PastPrefix(std::string_view key) const {
    return prefix_ && key > *prefix_ && !key.starts_with(*prefix_);
  }

  struct FrontierEntry {
    std::string key;
    std::size_t child_index = 0;
    std::size_t priority = 0;
  };

  struct FrontierCompare {
    bool operator()(const FrontierEntry& left,
                    const FrontierEntry& right) const {
      if (left.key != right.key) return left.key > right.key;
      return left.priority < right.priority;
    }
  };

  void AddToFrontier(std::size_t index) {
    const auto& child = children_[index];
    if (child.iterator->Valid()) {
      frontier_.push(
          {std::string(child.iterator->key()), index, child.priority});
    } else if (!child.iterator->status().ok() && status_.ok()) {
      status_ = child.iterator->status();
    }
  }

  void RebuildFrontier() {
    frontier_ = {};
    current_sources_.clear();
    for (std::size_t i = 0; i < children_.size(); ++i) AddToFrontier(i);
  }

  void FindNext() {
    valid_ = false;
    key_.clear();
    value_.clear();

    while (status_.ok()) {
      current_sources_.clear();
      if (frontier_.empty()) return;
      const std::string next_key = frontier_.top().key;
      if ((upper_bound_ && next_key >= *upper_bound_) || PastPrefix(next_key)) {
        return;
      }

      std::size_t winner_index = frontier_.top().child_index;
      while (!frontier_.empty() && frontier_.top().key == next_key) {
        current_sources_.push_back(frontier_.top().child_index);
        frontier_.pop();
      }
      const auto& winner = children_[winner_index];

      if (winner.iterator->type() == RecordType::kPut &&
          MatchesPrefix(next_key)) {
        key_ = next_key;
        value_ = winner.iterator->value();
        valid_ = true;
        return;
      }

      for (std::size_t index : current_sources_) {
        children_[index].iterator->Next();
        AddToFrontier(index);
      }
    }
  }

  std::vector<MergeIteratorChild> children_;
  std::optional<std::string> lower_bound_;
  std::optional<std::string> upper_bound_;
  std::optional<std::string> prefix_;
  std::string key_;
  std::string value_;
  std::priority_queue<FrontierEntry, std::vector<FrontierEntry>,
                      FrontierCompare>
      frontier_;
  std::vector<std::size_t> current_sources_;
  Status status_ = Status::OK();
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<MergeIteratorChild> children,
    const ReadOptions& read_options) {
  return std::make_unique<MergingIterator>(std::move(children), read_options);
}

}  // namespace stratakv
