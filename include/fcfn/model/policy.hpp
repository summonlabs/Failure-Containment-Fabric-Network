// FCFN - containment policy.
//
// Policy is durable definition state. It carries a generation so that every
// plan can bind the exact policy revision that made it legal.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_POLICY_HPP
#define FCFN_MODEL_POLICY_HPP

#include <cstdint>
#include <span>
#include <string_view>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"

namespace fcfn::model {

/// Bounded containment policy.
struct ContainmentPolicy {
  PolicyGeneration generation{};

  /// Every eligible failure source must appear in the containment boundary.
  bool require_failure_source_inclusion{true};
  /// Protected obligations may only be contained when explicitly reasoned.
  bool allow_protected_inclusion{false};
  /// Minimum blast radius: prefer zero protected inclusions over any weight saving.
  bool minimize_protected_inclusions{true};
  /// Maximum total weight of a containment boundary.
  std::uint64_t max_boundary_weight{kMaxPlanWeightSum};
  /// Maximum number of boundary members.
  std::size_t max_boundary_members{kMaxBoundaryMembers};
  /// Exact-search node budget. Reaching it yields an explicit bounded outcome.
  std::uint64_t exact_search_budget{kMaxExactSearchNodesDefault};
  /// Heuristic iteration budget for large instances.
  std::uint64_t heuristic_budget{kMaxHeuristicIterationsDefault};
  /// Use the scalable heuristic when the exact instance exceeds this size.
  std::size_t exact_instance_node_limit{4096};

  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<ContainmentPolicy> decode(std::span<const std::byte> bytes);
};

/// The policy a runtime starts with before any update is applied.
[[nodiscard]] ContainmentPolicy default_policy();

/// Validate every domain rule of a policy. Applied both when decoding a durable
/// policy and when accepting one at runtime, so a policy that is accepted can
/// always be encoded, stored, and recovered.
[[nodiscard]] VoidResult validate_policy(const ContainmentPolicy& policy);

}  // namespace fcfn::model

#endif  // FCFN_MODEL_POLICY_HPP
