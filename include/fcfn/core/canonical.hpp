// FCFN - deterministic canonical serialization.
//
// Canonical encoding rules (stable across platforms and compilers):
//   * integers are little-endian fixed width
//   * booleans are one byte, 0 or 1
//   * byte strings are u32 length followed by raw bytes
//   * text is a byte string restricted to valid UTF-8-free ASCII/UTF-8 passthrough
//   * digests are hi then lo as u64 each
//   * order is defined by the caller; maps are always emitted in ascending key order
//
// Decoding is total: any malformed input yields a classified Status and poisons
// the reader so no further reads can be interpreted.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_CANONICAL_HPP
#define FCFN_CORE_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fcfn/core/checked.hpp"
#include "fcfn/core/hash.hpp"
#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn {

/// Append-only canonical encoder. The buffer is the canonical form.
class CanonicalWriter {
 public:
  CanonicalWriter() { buffer_.reserve(256); }
  explicit CanonicalWriter(std::size_t reserve_bytes) { buffer_.reserve(reserve_bytes); }

  void u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void boolean(bool value) { u8(value ? 1u : 0u); }

  void raw(std::span<const std::byte> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

  /// Length-prefixed byte string.
  void blob(std::span<const std::byte> bytes) {
    u32(static_cast<std::uint32_t>(bytes.size()));
    raw(bytes);
  }

  /// Length-prefixed text.
  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (const char c : value) {
      u8(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }
  }

  void digest(const Digest& value) {
    u64(value.hi);
    u64(value.lo);
  }

  template <class Tag, class T>
  void strong(StrongId<Tag, T> value) {
    static_assert(sizeof(T) <= 8, "strong id payload must be at most 64 bits");
    if constexpr (sizeof(T) == 8) {
      u64(static_cast<std::uint64_t>(value.value()));
    } else if constexpr (sizeof(T) == 4) {
      u32(static_cast<std::uint32_t>(value.value()));
    } else if constexpr (sizeof(T) == 2) {
      u16(static_cast<std::uint16_t>(value.value()));
    } else {
      u8(static_cast<std::uint8_t>(value.value()));
    }
  }

  void resource_id(const ResourceId& id) { text(id.value()); }

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }

  [[nodiscard]] Digest digest() const noexcept {
    DigestBuilder builder;
    builder.update(span());
    return builder.finish();
  }

  void clear() noexcept { buffer_.clear(); }

 private:
  std::vector<std::byte> buffer_;
};

/// Sticky-failure canonical decoder.
class CanonicalReader {
 public:
  explicit CanonicalReader(std::span<const std::byte> input) noexcept : input_(input) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return offset_ <= input_.size() ? input_.size() - offset_ : 0;
  }
  [[nodiscard]] bool at_end() const noexcept { return ok_ && offset_ == input_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  std::uint8_t u8() {
    if (!require(1)) {
      return 0;
    }
    return static_cast<std::uint8_t>(input_[offset_++]);
  }

  std::uint16_t u16() {
    std::uint16_t value = 0;
    for (unsigned shift = 0; shift < 16; shift += 8) {
      value = static_cast<std::uint16_t>(value | static_cast<std::uint16_t>(u8()) << shift);
    }
    return value;
  }

  std::uint32_t u32() {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
  }

  std::uint64_t u64() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
  }

  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  bool boolean() {
    const std::uint8_t raw = u8();
    if (ok_ && raw > 1) {
      poison(StatusCode::InvalidArgument, "boolean byte out of domain");
    }
    return raw == 1;
  }

  /// Length-prefixed blob with an explicit bound applied before allocation.
  std::span<const std::byte> blob(std::size_t max_bytes) {
    const std::uint32_t length = u32();
    if (!ok_) {
      return {};
    }
    if (static_cast<std::size_t>(length) > max_bytes) {
      poison(StatusCode::LimitExceeded, "blob length exceeds bound");
      return {};
    }
    if (!require(length)) {
      return {};
    }
    const std::span<const std::byte> out(input_.data() + offset_, length);
    offset_ += length;
    return out;
  }

  std::string text(std::size_t max_bytes = kMaxCanonicalTextLength) {
    const std::span<const std::byte> bytes = blob(max_bytes);
    if (!ok_) {
      return {};
    }
    std::string out;
    out.resize(bytes.size());
    if (!bytes.empty()) {
      std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return out;
  }

  Digest digest() {
    Digest value;
    value.hi = u64();
    value.lo = u64();
    return value;
  }

  template <class Tag, class T = std::uint64_t>
  StrongId<Tag, T> strong() {
    StrongId<Tag, T> value;
    if constexpr (sizeof(T) == 8) {
      value = StrongId<Tag, T>(static_cast<T>(u64()));
    } else if constexpr (sizeof(T) == 4) {
      value = StrongId<Tag, T>(static_cast<T>(u32()));
    } else if constexpr (sizeof(T) == 2) {
      value = StrongId<Tag, T>(static_cast<T>(u16()));
    } else {
      value = StrongId<Tag, T>(static_cast<T>(u8()));
    }
    return value;
  }

  Result<ResourceId> resource_id() {
    const std::string raw = text(kMaxResourceIdLength);
    if (!ok_) {
      return status_;
    }
    return ResourceId::parse(raw);
  }

  /// Poison the reader with a classified failure. The first failure wins.
  void poison(StatusCode code, std::string_view message) noexcept {
    if (ok_) {
      ok_ = false;
      status_ = Status{code, message};
    }
  }

  void require_end() {
    if (ok_ && offset_ != input_.size()) {
      poison(StatusCode::TrailingGarbage, "bytes remain after canonical document");
    }
  }

 private:
  bool require(std::size_t count) {
    if (!ok_) {
      return false;
    }
    if (count > remaining()) {
      poison(StatusCode::InvalidArgument, "canonical document truncated");
      return false;
    }
    return true;
  }

  std::span<const std::byte> input_{};
  std::size_t offset_{0};
  bool ok_{true};
  Status status_{};
};

}  // namespace fcfn

#endif  // FCFN_CORE_CANONICAL_HPP
