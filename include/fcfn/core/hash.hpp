// FCFN - integrity primitives (non-cryptographic).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_HASH_HPP
#define FCFN_CORE_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fcfn/core/ids.hpp"

namespace fcfn {

inline constexpr std::uint64_t kFnvOffsetBasis64 = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime64 = 0x100000001b3ULL;

/// Streaming FNV-1a (64-bit). Used for canonical digests and hash tables.
class Fnv1a64 {
 public:
  constexpr Fnv1a64() noexcept = default;
  constexpr explicit Fnv1a64(std::uint64_t seed) noexcept : state_(seed) {}

  constexpr void update(std::uint8_t byte) noexcept {
    state_ ^= static_cast<std::uint64_t>(byte);
    state_ = static_cast<std::uint64_t>(state_ * kFnvPrime64);
  }

  void update(std::span<const std::byte> bytes) noexcept {
    for (const std::byte b : bytes) {
      update(static_cast<std::uint8_t>(b));
    }
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_{kFnvOffsetBasis64};
};

/// 128-bit digest: two independent FNV-1a lanes with distinct offset bases.
///
/// This is an integrity checksum suitable for detecting corruption and
/// accidental mismatch. It is NOT a cryptographic hash and FCFN makes no
/// authentication claim based on it.
class DigestBuilder {
 public:
  constexpr DigestBuilder() noexcept : lo_(kFnvOffsetBasis64), hi_(kFnvOffsetBasis64 * 3 + 1) {}

  void update(std::span<const std::byte> bytes) noexcept {
    for (const std::byte b : bytes) {
      const auto v = static_cast<std::uint8_t>(b);
      lo_.update(v);
      hi_.update(static_cast<std::uint8_t>(v ^ 0x5au));
    }
  }

  void update_u64(std::uint64_t value) noexcept {
    std::array<std::byte, 8> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
      raw[i] = static_cast<std::byte>((value >> (8u * i)) & 0xffu);
    }
    update(std::span<const std::byte>(raw.data(), raw.size()));
  }

  [[nodiscard]] Digest finish() const noexcept { return Digest{hi_.value(), lo_.value()}; }

 private:
  Fnv1a64 lo_;
  Fnv1a64 hi_;
};

/// CRC-32C (Castagnoli), used for durable record and wire frame integrity.
class Crc32c {
 public:
  constexpr Crc32c() noexcept = default;

  void update(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = state_;
    for (const std::byte b : bytes) {
      const auto index = static_cast<std::uint8_t>((crc ^ static_cast<std::uint32_t>(b)) & 0xffu);
      crc = static_cast<std::uint32_t>((crc >> 8) ^ table()[index]);
    }
    state_ = crc;
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return state_ ^ 0xffffffffu; }

  [[nodiscard]] static std::uint32_t compute(std::span<const std::byte> bytes) noexcept {
    Crc32c crc;
    crc.update(bytes);
    return crc.value();
  }

 private:
  [[nodiscard]] static const std::array<std::uint32_t, 256>& table() noexcept;

  std::uint32_t state_{0xffffffffu};
};

}  // namespace fcfn

#endif  // FCFN_CORE_HASH_HPP
