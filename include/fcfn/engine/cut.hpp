// FCFN - the containment problem class MCC-1 and its certificates.
//
// Problem class MCC-1 (Minimum Containment Cut, class 1)
// ------------------------------------------------------
// Input:
//   * a validated propagation graph G over nodes V; each edge is PROVEN or
//     UNKNOWN; the conservative graph G+ contains both
//   * a non-empty set F of failure sources (authoritative Present evidence)
//   * a set P of protected obligations
//   * per-node eligibility (containable), per-node containment weight
//   * policy flags: require_source_inclusion, allow_protected_inclusion,
//     minimize_protected_inclusions, max_total_weight, max_members
// Decision variable: a containment set C subset of V.
// Hard constraints:
//   1. C contains only eligible nodes
//   2. every eligible failure source is in C when require_source_inclusion
//   3. C contains no protected obligation unless allow_protected_inclusion
//   4. C has no repeated members and respects max_members and max_total_weight
//   5. in G+ with C removed there is no path from any source in F to any p in P
// Objective, minimised lexicographically:
//   (protected inclusions, total weight, cardinality, canonical member signature)
// Correctness: a returned C satisfies 1-5 and is checked by an independent
// verifier. Optimality: only claimed when the exact search completed.
// Completeness: PROVEN_INFEASIBLE is returned only in one of two ways -- with a
// compact certificate that engine::verify_infeasibility_witness accepts, or
// after an exhaustive search that ended without a feasible set (reported through
// SolveCounters::budget_exhausted == false). A search that ends early returns
// LimitReachedNoSolution, which never claims that no solution exists.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_ENGINE_CUT_HPP
#define FCFN_ENGINE_CUT_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/model/plan.hpp"

namespace fcfn::engine {

/// External description of one containment instance.
struct ContainmentInstanceSpec {
  std::vector<model::ResourceId> failure_sources{};
  std::vector<model::ResourceId> protected_obligations{};
  /// Nodes forced to be ineligible (used by necessity analysis).
  std::vector<model::ResourceId> forced_ineligible{};
  bool require_source_inclusion{true};
  bool allow_protected_inclusion{false};
  bool minimize_protected_inclusions{true};
  std::uint64_t max_total_weight{kMaxPlanWeightSum};
  std::size_t max_members{kMaxBoundaryMembers};
};

/// Validated MCC-1 instance over dense node indices.
class ContainmentProblem {
 public:
  ContainmentProblem() = default;

  [[nodiscard]] static Result<ContainmentProblem> build(const PropagationGraph& graph,
                                                        const ContainmentInstanceSpec& spec);

  [[nodiscard]] const PropagationGraph& graph() const noexcept { return *graph_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

  [[nodiscard]] const std::vector<model::NodeIndex>& sources() const noexcept { return sources_; }
  [[nodiscard]] const std::vector<model::NodeIndex>& protected_nodes() const noexcept { return protected_; }
  [[nodiscard]] const std::vector<model::NodeIndex>& candidates() const noexcept { return candidates_; }
  [[nodiscard]] bool is_candidate(model::NodeIndex node) const noexcept { return candidate_mask_[node] != 0; }
  [[nodiscard]] bool is_containable(model::NodeIndex node) const noexcept { return containable_[node] != 0; }
  [[nodiscard]] bool is_protected(model::NodeIndex node) const noexcept { return protected_mask_[node] != 0; }
  [[nodiscard]] bool is_source(model::NodeIndex node) const noexcept { return source_mask_[node] != 0; }
  [[nodiscard]] std::uint64_t weight(model::NodeIndex node) const noexcept { return weights_[node]; }
  [[nodiscard]] bool require_source_inclusion() const noexcept { return require_source_inclusion_; }
  [[nodiscard]] bool allow_protected_inclusion() const noexcept { return allow_protected_inclusion_; }
  [[nodiscard]] bool minimize_protected_inclusions() const noexcept { return minimize_protected_inclusions_; }
  [[nodiscard]] std::uint64_t max_total_weight() const noexcept { return max_total_weight_; }
  [[nodiscard]] std::size_t max_members() const noexcept { return max_members_; }
  [[nodiscard]] const std::vector<model::ResourceId>& resource_ids() const noexcept { return resource_ids_; }

  /// Failure sources that this instance cannot contain (ineligible).
  [[nodiscard]] const std::vector<model::NodeIndex>& uncontainable_sources() const noexcept {
    return uncontainable_sources_;
  }

  /// Nodes barred from containment by this instance, beyond the ones the
  /// topology itself marks ineligible.
  [[nodiscard]] const std::vector<model::NodeIndex>& barred_nodes() const noexcept { return barred_; }

  /// True when the node lies on some source-to-protected path in G+.
  [[nodiscard]] bool on_relevant_path(model::NodeIndex node) const noexcept { return relevant_[node] != 0; }

  /// Canonical digest of everything that defines this instance.
  [[nodiscard]] const Digest& instance_digest() const noexcept { return instance_digest_; }

  /// Nodes reachable from any source in G+ (the propagation frontier).
  [[nodiscard]] const std::vector<model::NodeIndex>& frontier() const noexcept { return frontier_; }

 private:
  const PropagationGraph* graph_{nullptr};
  std::size_t node_count_{0};
  std::vector<model::NodeIndex> sources_{};
  std::vector<model::NodeIndex> protected_{};
  std::vector<model::NodeIndex> candidates_{};
  std::vector<model::NodeIndex> uncontainable_sources_{};
  /// Nodes this instance barred through ContainmentInstanceSpec::forced_ineligible.
  /// Necessity analysis must carry them into every derived instance.
  std::vector<model::NodeIndex> barred_{};
  std::vector<model::NodeIndex> frontier_{};
  std::vector<model::ResourceId> resource_ids_{};
  std::vector<std::uint8_t> containable_{};
  std::vector<std::uint8_t> protected_mask_{};
  std::vector<std::uint8_t> source_mask_{};
  std::vector<std::uint8_t> candidate_mask_{};
  std::vector<std::uint8_t> relevant_{};
  std::vector<std::uint64_t> weights_{};
  bool require_source_inclusion_{true};
  bool allow_protected_inclusion_{false};
  bool minimize_protected_inclusions_{true};
  std::uint64_t max_total_weight_{kMaxPlanWeightSum};
  std::size_t max_members_{kMaxBoundaryMembers};
  Digest instance_digest_{};
};

/// Explicit solve outcome. Never conflates "no solution found yet" with "no
/// solution exists".
enum class SolveStatus : std::uint8_t {
  /// A feasible cut was found and proven optimal by exhaustive search.
  ProvenOptimal = 0,
  /// A feasible cut was found, but the budget ended before optimality was proven.
  FeasibleNotProvenOptimal = 1,
  /// Search completed without a feasible cut, or a compact certificate proves
  /// infeasibility.
  ProvenInfeasible = 2,
  /// The budget ended without any feasible cut: feasibility is unknown.
  LimitReachedNoSolution = 3,
  /// The instance is not solvable as stated.
  InvalidProblem = 4,
};

[[nodiscard]] const char* to_string(SolveStatus value) noexcept;

/// Lexicographic objective value.
struct ObjectiveVector {
  std::uint64_t protected_inclusions{0};
  std::uint64_t total_weight{0};
  std::uint64_t cardinality{0};
  std::vector<model::ResourceId> signature{};

  /// Lexicographic comparison; the signature is the deterministic tie-break.
  friend bool operator<(const ObjectiveVector& a, const ObjectiveVector& b) noexcept;
  friend bool operator==(const ObjectiveVector& a, const ObjectiveVector& b) noexcept;
};

/// Bounded search accounting.
struct SolveCounters {
  std::uint64_t candidate_nodes{0};
  std::uint64_t explored_nodes{0};
  std::uint64_t pruned_branches{0};
  std::uint64_t residual_paths_checked{0};
  std::uint64_t budget{0};
  bool budget_exhausted{false};
  bool heuristic_used{false};
};

/// Result of a solve attempt.
struct CutSolution {
  SolveStatus status{SolveStatus::InvalidProblem};
  std::vector<model::NodeIndex> members{};
  ObjectiveVector objective{};
  SolveCounters counters{};
  /// Compact infeasibility certificate when present (all path nodes ineligible).
  bool has_infeasibility_witness{false};
  std::vector<model::NodeIndex> infeasibility_path{};

  [[nodiscard]] bool has_solution() const noexcept {
    return status == SolveStatus::ProvenOptimal || status == SolveStatus::FeasibleNotProvenOptimal;
  }
};

/// Budget for one solve.
struct SolveBudget {
  std::uint64_t max_explored_nodes{kMaxExactSearchNodesDefault};
  std::uint64_t max_iterations{kMaxHeuristicIterationsDefault};
};

/// True when no source reaches any protected node after removing the members.
[[nodiscard]] bool cut_is_valid(const ContainmentProblem& problem, std::span<const model::NodeIndex> removed);

/// Independent verifier used by solvers and tests: checks every hard constraint
/// of MCC-1 rather than only the disconnection property.
[[nodiscard]] Result<ObjectiveVector> verify_cut(const ContainmentProblem& problem,
                                                 std::span<const model::NodeIndex> members);

/// Objective comparison honouring the policy minimisation order.
[[nodiscard]] bool objective_less(const ObjectiveVector& a, const ObjectiveVector& b,
                                  bool minimize_protected) noexcept;

/// Verify a compact infeasibility certificate: a path in G+ from a source to a
/// protected obligation whose intermediate and terminal nodes are all ineligible.
[[nodiscard]] bool verify_infeasibility_witness(const ContainmentProblem& problem,
                                                std::span<const model::NodeIndex> path);

/// Reachability question with an explicit exclusion set.
[[nodiscard]] bool source_reaches_protected(const ContainmentProblem& problem,
                                            std::span<const model::NodeIndex> excluded);

/// Shortest witness path (in hops) from any source to any protected node in G+
/// while avoiding the excluded nodes. Empty when no path exists.
[[nodiscard]] std::vector<model::NodeIndex> find_witness_path(const ContainmentProblem& problem,
                                                              std::span<const model::NodeIndex> excluded,
                                                              bool* uses_unknown = nullptr);

/// Compute the objective vector for a member set, or a failure status.
[[nodiscard]] Result<ObjectiveVector> objective_of(const ContainmentProblem& problem,
                                                   std::span<const model::NodeIndex> members);

}  // namespace fcfn::engine

#endif  // FCFN_ENGINE_CUT_HPP
