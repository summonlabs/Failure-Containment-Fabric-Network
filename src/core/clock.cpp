// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/core/clock.hpp"

#include <chrono>

namespace fcfn {

std::uint64_t SteadyClock::now_millis() const noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace fcfn
