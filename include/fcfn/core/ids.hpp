// FCFN - strongly typed identities.
//
// Identifiers are not interchangeable: a ResourceId cannot be passed where a
// BoundaryId is expected, and a generation cannot be passed where a sequence is
// expected. Matching text is never sufficient evidence of matching generation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_IDS_HPP
#define FCFN_CORE_IDS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fcfn/core/result.hpp"

namespace fcfn {

/// Numeric identity parameterised by a tag so distinct domains cannot be mixed.
template <class Tag, class T = std::uint64_t>
class StrongId {
 public:
  using value_type = T;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(T value) noexcept : value_(value) {}

  [[nodiscard]] constexpr T value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == T{0}; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return a.value_ >= b.value_; }

 private:
  T value_{0};
};

struct ResourceIdTag;
struct TopologyGenerationTag;
struct PolicyGenerationTag;
struct EvidenceGenerationTag;
struct BoundaryGenerationTag;
struct PlanGenerationTag;
struct CoordinatorEpochTag;
struct BootIdTag;
struct SequenceTag;
struct SessionIdTag;
struct AttemptSequenceTag;
struct TransitionGenerationTag;
struct SnapshotSequenceTag;
struct ApplierEpochTag;

using TopologyGeneration = StrongId<TopologyGenerationTag>;
using PolicyGeneration = StrongId<PolicyGenerationTag>;
using BoundaryGeneration = StrongId<BoundaryGenerationTag>;
using PlanGeneration = StrongId<PlanGenerationTag>;
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;
using BootId = StrongId<BootIdTag>;
using Sequence = StrongId<SequenceTag>;
using SessionId = StrongId<SessionIdTag>;
using AttemptSequence = StrongId<AttemptSequenceTag>;
using TransitionGeneration = StrongId<TransitionGenerationTag>;
using SnapshotSequence = StrongId<SnapshotSequenceTag>;
using ApplierEpoch = StrongId<ApplierEpochTag>;

/// Identity of a detection/evidence record; monotonic per evidence authority.
using EvidenceGeneration = StrongId<EvidenceGenerationTag>;

/// Bounds applied to every textual identity accepted from outside the process.
inline constexpr std::size_t kMaxResourceIdLength = 63;

/// Validate the canonical resource identifier form:
///   - 1..kMaxResourceIdLength bytes
///   - ASCII letters, digits, '.', '_', ':', '-'
///   - no ".." sequence, no leading or trailing '.' or '-'
[[nodiscard]] bool is_valid_resource_id(std::string_view text) noexcept;

/// A canonical resource identity (fabric node, link, rack, tenant, obligation).
class ResourceId {
 public:
  ResourceId() = default;

  [[nodiscard]] static Result<ResourceId> parse(std::string_view text);
  [[nodiscard]] static ResourceId unchecked(std::string text) { return ResourceId{std::move(text)}; }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const ResourceId& a, const ResourceId& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const ResourceId& a, const ResourceId& b) noexcept { return a.value_ != b.value_; }
  friend bool operator<(const ResourceId& a, const ResourceId& b) noexcept { return a.value_ < b.value_; }

  void append_canonical(std::string& out) const { out.append(value_); }

 private:
  explicit ResourceId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

struct ResourceIdHash {
  [[nodiscard]] std::size_t operator()(const ResourceId& id) const noexcept {
    return std::hash<std::string>{}(id.value());
  }
};

/// Integrity digest (non-cryptographic, 128-bit FNV-1a variant).
///
/// FCFN deliberately does not invent cryptographic security: this digest detects
/// corruption and accidental mismatch. It is not an authentication primitive and
/// is documented as such in the README trust-boundary section.
struct Digest {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  [[nodiscard]] static constexpr Digest zero() noexcept { return Digest{}; }
  [[nodiscard]] bool is_zero() const noexcept { return hi == 0 && lo == 0; }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static bool parse(std::string_view text, Digest& out) noexcept;

  friend constexpr bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
  friend constexpr bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend constexpr bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
  }
};

/// A concise identifier for a boot (process incarnation), rendered as 16 hex chars.
struct BootIdentity {
  BootId id{};
  std::uint32_t process_id{0};

  friend bool operator==(const BootIdentity& a, const BootIdentity& b) noexcept {
    return a.id == b.id && a.process_id == b.process_id;
  }
  friend bool operator!=(const BootIdentity& a, const BootIdentity& b) noexcept { return !(a == b); }
  friend bool operator<(const BootIdentity& a, const BootIdentity& b) noexcept {
    return a.id != b.id ? a.id < b.id : a.process_id < b.process_id;
  }

  [[nodiscard]] std::string render() const;
};

}  // namespace fcfn

#endif  // FCFN_CORE_IDS_HPP
