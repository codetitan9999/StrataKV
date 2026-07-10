#pragma once

#include <ostream>
#include <string>
#include <string_view>

namespace stratakv {

class Status {
 public:
  enum class Code {
    kOk = 0,
    kNotFound,
    kInvalidArgument,
    kIOError,
    kCorruption,
    kNotSupported,
  };

  Status() = default;

  static Status OK();
  static Status NotFound(std::string_view message);
  static Status InvalidArgument(std::string_view message);
  static Status IOError(std::string_view message);
  static Status Corruption(std::string_view message);
  static Status NotSupported(std::string_view message);

  [[nodiscard]] bool ok() const;
  [[nodiscard]] Code code() const;
  [[nodiscard]] const std::string& message() const;
  [[nodiscard]] std::string ToString() const;

 private:
  Status(Code code, std::string message);

  Code code_{Code::kOk};
  std::string message_;
};

std::ostream& operator<<(std::ostream& os, const Status& status);

}  // namespace stratakv
