// FCFN test support: independent exact reference solver.
//
// This solver is deliberately written from the MCC-1 definition rather than from
// the production solver: it enumerates every subset of the eligible nodes in
// canonical order, checks each hard constraint with its own breadth-first
// search, and keeps the lexicographically minimal objective. It shares no code
// with fcfn::engine except the problem accessors, so agreement between the two
// is meaningful evidence.
//
// It is exponential and bounded: instances with more than the configured number
// of eligible nodes are rejected rather than approximated.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_REFERENCE_SOLVER_HPP
#define FCFN_TEST_REFERENCE_SOLVER_HPP

#include <cstddef>
#include <vector>

#include "fcfn/engine/cut.hpp"

namespace fcfn::test {

inline constexpr std::size_t kReferenceSolverEligibleLimit = 16;

struct ReferenceResult {
  bool feasible{false};
  /// False when the instance exceeded the reference solver's documented bound.
  bool supported{true};
  std::vector<model::NodeIndex> members{};
  engine::ObjectiveVector objective{};
  std::uint64_t examined{0};
};

/// Exhaustive reference solution for small instances.
[[nodiscard]] ReferenceResult reference_solve(const engine::ContainmentProblem& problem);

/// Independent reachability check written directly against the graph.
[[nodiscard]] bool reference_reaches(const engine::ContainmentProblem& problem,
                                     const std::vector<model::NodeIndex>& removed);

}  // namespace fcfn::test

#endif  // FCFN_TEST_REFERENCE_SOLVER_HPP
