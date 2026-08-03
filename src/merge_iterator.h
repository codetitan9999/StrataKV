#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "internal_iterator.h"
#include "stratakv/iterator.h"
#include "stratakv/options.h"

namespace stratakv {

struct MergeIteratorChild {
  std::unique_ptr<InternalIterator> iterator;
  std::size_t priority = 0;
};

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<MergeIteratorChild> children,
    const ReadOptions& read_options = {});

}  // namespace stratakv
