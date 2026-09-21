// FCFN - containment plan: the externally visible answer.
//
// A plan states the minimum governed scope that must be contained now, the
// exact generations that made the decision legal, an explicit claim strength,
// and deterministic explanations. Claims never overstate what was proven:
// PROVEN_CONTAINMENT requires a verified cut over complete evidence, while
// bounded search, missing adjacency, stale evidence, or uncontainable sources
// all degrade the claim to INDETERMINATE rather than being rounded up.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_PLAN_HPP
#define FCFN_MODEL_PLAN_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/boundary.hpp"
#include "fcfn/model/explanation.hpp"

namespace fcfn::model {

/// Strength of the containment claim carried by a plan.
enum class ContainmentClaim : std::uint8_t {
  /// Verified cut, proven optimal, complete current evidence, all required
  /// sources contained. This is the only affirmative claim FCFN makes.
  ProvenContainment = 0,
  /// A verified cut exists but optimality was not proven within the budget.
  ProvenFeasibleNotMinimal = 1,
  /// The cut (if any) cannot be claimed as containment: evidence incomplete,
  /// stale, conflicting, or a required source cannot be contained.
  Indeterminate = 2,
  /// A certificate proves no eligible containment exists for this instance.
  ProvenInfeasible = 3,
  /// The request itself was rejected.
  Invalid = 4,
  /// Recognised but not implemented by this build.
  Unsupported = 5,
};

[[nodiscard]] const char* to_string(ContainmentClaim value) noexcept;

enum class FeasibilityStatus : std::uint8_t {
  ProvenFeasible = 0,
  ProvenInfeasible = 1,
  Unknown = 2,
};

[[nodiscard]] const char* to_string(FeasibilityStatus value) noexcept;

enum class OptimalityStatus : std::uint8_t {
  ProvenOptimal = 0,
  BoundedNotProven = 1,
  NotAttempted = 2,
  NotApplicable = 3,
};

[[nodiscard]] const char* to_string(OptimalityStatus value) noexcept;

/// Quality of the evidence the plan was computed against.
enum class EvidenceCurrency : std::uint8_t {
  /// Every evidence generation matches current state and the whole propagation
  /// frontier has complete adjacency information.
  Current = 0,
  /// Some frontier node has partial or unknown adjacency: paths may be missing.
  IncompleteFrontier = 1,
  /// Evidence generations have been superseded or regressed.
  Stale = 2,
  /// Contradictory evidence exists for at least one resource.
  Conflicting = 3,
};

[[nodiscard]] const char* to_string(EvidenceCurrency value) noexcept;

/// Deterministic search accounting, reported with every plan.
struct SearchCounters {
  std::uint64_t candidate_nodes{0};
  std::uint64_t explored_nodes{0};
  std::uint64_t pruned_branches{0};
  std::uint64_t residual_paths_checked{0};
  std::uint64_t budget{0};
  bool budget_exhausted{false};
  bool heuristic_used{false};
};

/// Compact refutation: a path from a failure source to a protected obligation
/// whose nodes are all ineligible for containment, so no containment exists.
struct InfeasibilityWitness {
  bool present{false};
  std::vector<ResourceId> path{};
};

/// The externally visible containment decision.
struct ContainmentPlan {
  PlanGeneration generation{};
  ContainmentBoundary boundary{};
  ContainmentClaim claim{ContainmentClaim::Indeterminate};
  FeasibilityStatus feasibility{FeasibilityStatus::Unknown};
  OptimalityStatus optimality{OptimalityStatus::NotAttempted};
  EvidenceCurrency currency{EvidenceCurrency::Current};

  AuthorityVector authority{};
  Digest topology_digest{};
  Digest policy_digest{};
  Digest evidence_digest{};
  Digest instance_digest{};

  std::vector<ResourceId> failure_sources{};
  std::vector<ResourceId> protected_obligations{};
  std::vector<ResourceId> uncontainable_sources{};
  std::vector<ResourceId> proven_necessary_members{};
  std::vector<ResourceId> indeterminate_necessity{};

  std::uint64_t total_weight{0};
  std::size_t member_count{0};
  SearchCounters counters{};
  InfeasibilityWitness infeasibility{};
  Explanation explanation{};

  /// Canonical digest over every decision-bearing field.
  [[nodiscard]] Digest compute_digest() const;
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
  void seal() { digest_ = compute_digest(); }

  /// A plan is releasable only when the claim proves containment and the
  /// boundary is empty: no active failure evidence remains.
  [[nodiscard]] bool is_releasable() const noexcept;

  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<ContainmentPlan> decode(std::span<const std::byte> bytes);

 private:
  Digest digest_{};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_PLAN_HPP
