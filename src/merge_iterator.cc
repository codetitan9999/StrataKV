#include "merge_iterator.h"

#include <string>
#include <string_view>
#include <utility>

namespace stratakv {
namespace {

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<MergeIteratorChild> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    status_ = Status::OK();
    for (auto& child : children_) child.iterator->SeekToFirst();
    FindNext();
  }

  void Seek(std::string_view target) override {
    status_ = Status::OK();
    for (auto& child : children_) child.iterator->Seek(target);
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

      MergeIteratorChild* winner = nullptr;
      for (auto& child : children_) {
        if (child.iterator->Valid() && child.iterator->key() == next_key &&
            (winner == nullptr || child.priority > winner->priority)) {
          winner = &child;
        }
      }

      if (winner->iterator->type() == RecordType::kPut) {
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
  std::string key_;
  std::string value_;
  Status status_ = Status::OK();
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<MergeIteratorChild> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace stratakv
