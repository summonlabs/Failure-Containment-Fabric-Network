// FCFN - clock abstraction.
//
// Timestamps are informational: they are never used as authority and never
// restored as freshness. Lease expiry is evaluated against this clock, which
// tests drive deterministically instead of sleeping.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_CLOCK_HPP
#define FCFN_CORE_CLOCK_HPP

#include <atomic>
#include <cstdint>

namespace fcfn {

/// Monotone millisecond clock. Implementations must never move backwards.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual std::uint64_t now_millis() const noexcept = 0;
};

/// Process-wide monotone clock.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] std::uint64_t now_millis() const noexcept override;
};

/// Deterministic clock for tests and replay: time only advances when told to.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::uint64_t start_millis = 1000) noexcept : now_(start_millis) {}

  [[nodiscard]] std::uint64_t now_millis() const noexcept override { return now_.load(); }
  void advance(std::uint64_t millis) noexcept { now_.fetch_add(millis); }
  void set(std::uint64_t millis) noexcept { now_.store(millis); }

 private:
  std::atomic<std::uint64_t> now_;
};

}  // namespace fcfn

#endif  // FCFN_CORE_CLOCK_HPP
