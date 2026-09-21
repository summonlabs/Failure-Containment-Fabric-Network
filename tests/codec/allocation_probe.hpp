// FCFN codec suite: allocation accounting.
//
// Canonical decoding must refuse an out-of-bound declared count or length
// BEFORE materialising anything. These counters make that testable: the
// replacement global operator new below counts every byte the program hands out.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_CODEC_ALLOCATION_PROBE_HPP
#define FCFN_TEST_CODEC_ALLOCATION_PROBE_HPP

#include <cstdint>

namespace fcfn::test {

/// Bytes handed out by the replacement global operator new since process start.
[[nodiscard]] std::uint64_t allocated_bytes() noexcept;

/// Number of replacement global operator new calls since process start.
[[nodiscard]] std::uint64_t allocation_calls() noexcept;

/// Records the allocation delta over its lifetime.
class AllocationWindow {
 public:
  AllocationWindow() noexcept : bytes_(allocated_bytes()), calls_(allocation_calls()) {}
  AllocationWindow(const AllocationWindow&) = delete;
  AllocationWindow& operator=(const AllocationWindow&) = delete;

  [[nodiscard]] std::uint64_t bytes() const noexcept { return allocated_bytes() - bytes_; }
  [[nodiscard]] std::uint64_t calls() const noexcept { return allocation_calls() - calls_; }

 private:
  std::uint64_t bytes_;
  std::uint64_t calls_;
};

}  // namespace fcfn::test

#endif  // FCFN_TEST_CODEC_ALLOCATION_PROBE_HPP
