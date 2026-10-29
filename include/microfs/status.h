#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace microfs {

enum class StatusCode {
  kOk,
  kNotFound,
  kInvalidArgument,
  kIoError,
  kCorruption,
  kNotSupported,
  kBusy,
  kResourceExhausted,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status OK() { return {}; }
  static Status NotFound(std::string message) {
    return {StatusCode::kNotFound, std::move(message)};
  }
  static Status InvalidArgument(std::string message) {
    return {StatusCode::kInvalidArgument, std::move(message)};
  }
  static Status IOError(std::string message) {
    return {StatusCode::kIoError, std::move(message)};
  }
  static Status Corruption(std::string message) {
    return {StatusCode::kCorruption, std::move(message)};
  }
  static Status NotSupported(std::string message) {
    return {StatusCode::kNotSupported, std::move(message)};
  }
  static Status Busy(std::string message) {
    return {StatusCode::kBusy, std::move(message)};
  }
  static Status ResourceExhausted(std::string message) {
    return {StatusCode::kResourceExhausted, std::move(message)};
  }

  [[nodiscard]] bool ok() const { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

template <typename T>
class StatusOr {
 public:
  StatusOr(Status status) : status_(std::move(status)) {
    assert(!status_.ok());
  }
  StatusOr(T value) : value_(std::move(value)) {}

  [[nodiscard]] bool ok() const { return status_.ok(); }
  [[nodiscard]] const Status& status() const { return status_; }
  [[nodiscard]] const T& value() const& {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] T& value() & {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] T&& value() && {
    assert(ok());
    return std::move(*value_);
  }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace microfs
