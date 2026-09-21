// FCFN - result carrier that keeps failure classification explicit.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_RESULT_HPP
#define FCFN_CORE_RESULT_HPP

#include <optional>
#include <utility>
#include <variant>

#include "fcfn/core/status.hpp"

namespace fcfn {

/// Either a value or a classified Status. There is no "empty" success state.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::in_place_index<1>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(std::in_place_index<0>, std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 1; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status ok_status{};
    return ok() ? ok_status : std::get<0>(storage_);
  }

  [[nodiscard]] T& value() & { return std::get<1>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<1>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<1>(std::move(storage_)); }

  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<1>(storage_) : std::move(fallback);
  }

 private:
  std::variant<Status, T> storage_;
};

/// Result with no payload: success is the only value.
class VoidResult {
 public:
  VoidResult() noexcept = default;
  VoidResult(Status status) noexcept : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

using StatusResult = VoidResult;

/// Convenience constructors.
[[nodiscard]] inline StatusResult ok_result() noexcept { return VoidResult{}; }
[[nodiscard]] inline StatusResult fail(StatusCode code, std::string_view message = {}) {
  return VoidResult{Status{code, message}};
}

/// Propagate a failure out of a function returning Result<T>.
#define FCFN_TRY(expr)                        \
  do {                                        \
    auto fcfn_try_result = (expr);            \
    if (!fcfn_try_result.ok()) {              \
      return fcfn_try_result.status();        \
    }                                         \
  } while (false)

}  // namespace fcfn

#endif  // FCFN_CORE_RESULT_HPP
