#include "stratakv/status.h"

#include <string>

namespace stratakv {
namespace {

std::string CodeToString(Status::Code code) {
  switch (code) {
    case Status::Code::kOk:
      return "OK";
    case Status::Code::kNotFound:
      return "NotFound";
    case Status::Code::kInvalidArgument:
      return "InvalidArgument";
    case Status::Code::kIOError:
      return "IOError";
    case Status::Code::kCorruption:
      return "Corruption";
    case Status::Code::kNotSupported:
      return "NotSupported";
  }

  return "Unknown";
}

}  // namespace

Status::Status(Code code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::OK() { return Status(); }

Status Status::NotFound(std::string_view message) {
  return Status(Code::kNotFound, std::string(message));
}

Status Status::InvalidArgument(std::string_view message) {
  return Status(Code::kInvalidArgument, std::string(message));
}

Status Status::IOError(std::string_view message) {
  return Status(Code::kIOError, std::string(message));
}

Status Status::Corruption(std::string_view message) {
  return Status(Code::kCorruption, std::string(message));
}

Status Status::NotSupported(std::string_view message) {
  return Status(Code::kNotSupported, std::string(message));
}

bool Status::ok() const { return code_ == Code::kOk; }

Status::Code Status::code() const { return code_; }

const std::string& Status::message() const { return message_; }

std::string Status::ToString() const {
  if (ok()) {
    return "OK";
  }

  if (message_.empty()) {
    return CodeToString(code_);
  }

  return CodeToString(code_) + ": " + message_;
}

std::ostream& operator<<(std::ostream& os, const Status& status) {
  os << status.ToString();
  return os;
}

}  // namespace stratakv
