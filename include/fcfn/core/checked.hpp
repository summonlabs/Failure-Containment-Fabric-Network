// FCFN - checked arithmetic helpers.
//
// All accounting paths in FCFN use these helpers: no wraparound, no silent
// truncation, and deterministic refusal when a bound would be exceeded.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_CHECKED_HPP
#define FCFN_CORE_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace fcfn {

/// Add with overflow detection. Returns false and leaves out untouched on overflow.
template <class T>
[[nodiscard]] constexpr bool checked_add(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_add requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return false;
    }
  } else {
    if ((b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) ||
        (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b))) {
      return false;
    }
  }
  out = static_cast<T>(a + b);
  return true;
}

/// Multiply with overflow detection.
template <class T>
[[nodiscard]] constexpr bool checked_mul(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_mul requires an integral type");
  if (a == 0 || b == 0) {
    out = T{0};
    return true;
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
      return false;
    }
  } else {
    const T max = std::numeric_limits<T>::max();
    const T min = std::numeric_limits<T>::min();
    if (a > 0) {
      if (b > 0 ? a > max / b : b < min / a) {
        return false;
      }
    } else {
      if (b > 0 ? a < min / b : (a != 0 && b < max / a)) {
        return false;
      }
    }
  }
  out = static_cast<T>(a * b);
  return true;
}

/// Narrowing conversion that reports failure instead of truncating.
template <class To, class From>
[[nodiscard]] constexpr bool checked_narrow(From value, To& out) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "integral types required");
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
    if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
        value > static_cast<From>(std::numeric_limits<To>::max())) {
      return false;
    }
  } else if constexpr (std::is_signed_v<From>) {
    if (value < 0 || static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<To>::max())) {
      return false;
    }
  } else {
    if (static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(std::numeric_limits<To>::max())) {
      return false;
    }
  }
  out = static_cast<To>(value);
  return true;
}

/// Saturating accumulator used by explanation and telemetry counters.
class SaturatingCounter {
 public:
  constexpr SaturatingCounter() noexcept = default;
  constexpr explicit SaturatingCounter(std::uint64_t initial) noexcept : value_(initial) {}

  constexpr void add(std::uint64_t delta) noexcept {
    if (!checked_add(value_, delta, value_)) {
      value_ = std::numeric_limits<std::uint64_t>::max();
      saturated_ = true;
    }
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool saturated() const noexcept { return saturated_; }

 private:
  std::uint64_t value_{0};
  bool saturated_{false};
};

/// Guard that requires a value to stay within an inclusive range.
[[nodiscard]] constexpr bool within(std::uint64_t value, std::uint64_t low, std::uint64_t high) noexcept {
  return value >= low && value <= high;
}

}  // namespace fcfn

#endif  // FCFN_CORE_CHECKED_HPP
