// FCFN - containment boundary.
//
// A boundary is the governed scope that must be contained now. Every member
// carries an explicit inclusion reason and, where applicable, a witness path
// proving the member is load-bearing rather than incidental. Protected
// obligations can only appear with an explicit isolation reason.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_BOUNDARY_HPP
#define FCFN_MODEL_BOUNDARY_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::model {

/// Why a resource is inside the containment boundary.
enum class InclusionReason : std::uint8_t {
  /// The resource itself carries authoritative failure evidence.
  FailureSource = 0,
  /// Removing the resource breaks a propagation path proven to exist.
  CutVertexOnProvenPath = 1,
  /// Removing the resource breaks a path that relies on UNKNOWN evidence.
  CutVertexOnUnknownPath = 2,
  /// Policy requires every eligible failure source to be contained.
  PolicyMandatedSource = 3,
  /// A protected obligation is deliberately isolated, with explicit authority.
  ProtectedObligationIsolation = 4,
  /// The resource is inside a shared-risk fence that cannot be split.
  SharedRiskDomainFence = 5,
};

[[nodiscard]] const char* to_string(InclusionReason value) noexcept;
[[nodiscard]] bool parse_inclusion_reason(std::string_view token, InclusionReason& out) noexcept;

/// One member of a containment boundary.
struct BoundaryMember {
  ResourceId resource{};
  InclusionReason reason{InclusionReason::FailureSource};
  bool protected_obligation{false};
  std::uint64_t weight{0};
  /// Bounded witness path (source .. member .. protected node) present in the
  /// graph after removing every other member. Empty for policy-mandated members.
  std::vector<ResourceId> witness_path{};
  /// True when the witness path relies on at least one UNKNOWN edge.
  bool witness_uses_unknown{false};
};

/// Canonically ordered containment boundary.
class ContainmentBoundary {
 public:
  ContainmentBoundary() = default;

  /// Create a boundary. Rejects duplicate members, members with no reason, and
  /// boundaries exceeding the configured member bound. Members are sorted into
  /// canonical ascending resource order.
  [[nodiscard]] static Result<ContainmentBoundary> create(BoundaryGeneration generation,
                                                          std::vector<BoundaryMember> members);

  [[nodiscard]] const BoundaryGeneration& generation() const noexcept { return generation_; }
  [[nodiscard]] const std::vector<BoundaryMember>& members() const noexcept { return members_; }
  [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
  [[nodiscard]] bool empty() const noexcept { return members_.empty(); }
  [[nodiscard]] bool contains(const ResourceId& resource) const noexcept;
  [[nodiscard]] const BoundaryMember* find(const ResourceId& resource) const noexcept;
  [[nodiscard]] std::uint64_t total_weight() const noexcept;
  [[nodiscard]] std::size_t protected_member_count() const noexcept;
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }

  [[nodiscard]] bool is_subset_of(const ContainmentBoundary& other) const noexcept;
  [[nodiscard]] bool is_superset_of(const ContainmentBoundary& other) const noexcept;
  [[nodiscard]] bool same_members(const ContainmentBoundary& other) const noexcept;

  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<ContainmentBoundary> decode(std::span<const std::byte> bytes);
  [[nodiscard]] std::string to_json() const;

 private:
  void recompute();

  BoundaryGeneration generation_{};
  std::vector<BoundaryMember> members_{};
  Digest digest_{};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_BOUNDARY_HPP
