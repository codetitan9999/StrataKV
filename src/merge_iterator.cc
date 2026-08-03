#include "merge_iterator.h"

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
    FindNext();
  }

  void Next() override {
    if (!valid_) return;
    for (auto& child : children_) {
      if (child.iterator->Valid() && child.iterator->key() == key_) {
        child.iterator->Next();
      }
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

  void FindNext() {
    valid_ = false;
    key_.clear();
    value_.clear();

    while (status_.ok()) {
      for (const auto& child : children_) {
        if (!child.iterator->status().ok()) {
          status_ = child.iterator->status();
          return;
        }
      }

      std::string next_key;
      bool found = false;
      for (const auto& child : children_) {
        if (child.iterator->Valid() &&
            (!found || child.iterator->key() < next_key)) {
          next_key = child.iterator->key();
          found = true;
        }
      }
      if (!found) return;
      if ((upper_bound_ && next_key >= *upper_bound_) || PastPrefix(next_key)) {
        return;
      }

      MergeIteratorChild* winner = nullptr;
      for (auto& child : children_) {
        if (child.iterator->Valid() && child.iterator->key() == next_key &&
            (winner == nullptr || child.priority > winner->priority)) {
          winner = &child;
        }
      }

      if (winner->iterator->type() == RecordType::kPut &&
          MatchesPrefix(next_key)) {
        key_ = next_key;
        value_ = winner->iterator->value();
        valid_ = true;
        return;
      }

      for (auto& child : children_) {
        if (child.iterator->Valid() && child.iterator->key() == next_key) {
          child.iterator->Next();
        }
      }
    }
  }

  std::vector<MergeIteratorChild> children_;
  std::optional<std::string> lower_bound_;
  std::optional<std::string> upper_bound_;
  std::optional<std::string> prefix_;
  std::string key_;
  std::string value_;
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
