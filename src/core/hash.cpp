// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/core/hash.hpp"

namespace fcfn {
namespace {

constexpr std::uint32_t kCrc32cPolynomial = 0x82f63b78u;

inline constexpr std::size_t kCrc32cTableSize = 256;

[[nodiscard]] constexpr std::array<std::uint32_t, kCrc32cTableSize> make_crc32c_table() noexcept {
  std::array<std::uint32_t, kCrc32cTableSize> table{};
  for (std::size_t i = 0; i < kCrc32cTableSize; ++i) {
    std::uint32_t crc = static_cast<std::uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? static_cast<std::uint32_t>((crc >> 1) ^ kCrc32cPolynomial)
                             : static_cast<std::uint32_t>(crc >> 1);
    }
    table.at(i) = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, kCrc32cTableSize> kCrc32cTable = make_crc32c_table();

}  // namespace

const std::array<std::uint32_t, 256>& Crc32c::table() noexcept { return kCrc32cTable; }


}  // namespace fcfn
