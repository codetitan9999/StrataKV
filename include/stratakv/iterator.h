#pragma once

#include <string_view>

#include "stratakv/status.h"

namespace stratakv {

class Iterator {
 public:
  virtual ~Iterator() = default;

  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;

  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;
  [[nodiscard]] virtual Status status() const = 0;
};

}  // namespace stratakv
