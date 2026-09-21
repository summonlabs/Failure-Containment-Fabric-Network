// FCFN - necessity analysis and monotonic safety accounting.
//
// A member is PROVEN NECESSARY when the instance becomes infeasible once that
// member is made ineligible. Necessity is only reported with a proof; when the
// budget ends first the member is reported as indeterminate instead.
//
// One case is decided by policy rather than by search: when
// require_source_inclusion is set, every eligible failure source must appear in
// every valid containment set, so it is necessary by construction. That is a
// proof, not a shortcut, and it is reported as proven_necessary rather than as
// indeterminate.
//
// Monotonic safety: adding failure sources or promoting UNKNOWN evidence to
// PROVEN can only shrink the feasible set, so the proven-necessary set grows and
// the optimal objective value cannot improve. Tests assert this exhaustively on
// small instances against an independent reference solver.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_ENGINE_NECESSITY_HPP
#define FCFN_ENGINE_NECESSITY_HPP

#include <span>
#include <vector>

#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/solver.hpp"

namespace fcfn::engine {

struct NecessityResult {
  std::vector<model::NodeIndex> proven_necessary{};
  std::vector<model::NodeIndex> indeterminate{};
  std::vector<model::NodeIndex> not_necessary{};
  SolveCounters counters{};
};

/// Test each proposed member for necessity by forcing it ineligible.
[[nodiscard]] NecessityResult analyze_necessity(const ContainmentProblem& problem,
                                                std::span<const model::NodeIndex> members,
                                                const SolveBudget& budget);

/// Exhaustive necessity over every eligible candidate (used by proofs and
/// differential tests; bounded by the same budget).
[[nodiscard]] NecessityResult analyze_necessity_exhaustive(const ContainmentProblem& problem,
                                                           const SolveBudget& budget);

}  // namespace fcfn::engine

#endif  // FCFN_ENGINE_NECESSITY_HPP
