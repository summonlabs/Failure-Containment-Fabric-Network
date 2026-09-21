// FCFN - containment solvers.
//
// solve_exact is complete: it either proves optimality, proves infeasibility, or
// reports that the budget ended (LimitReachedNoSolution). solve_heuristic is
// scalable and never claims optimality. Both return a verified feasible cut when
// they return one.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_ENGINE_SOLVER_HPP
#define FCFN_ENGINE_SOLVER_HPP

#include "fcfn/engine/cut.hpp"

namespace fcfn::engine {

/// Complete branch-and-bound solver for MCC-1.
[[nodiscard]] CutSolution solve_exact(const ContainmentProblem& problem, const SolveBudget& budget);

/// Scalable deterministic greedy solver with minimality restoration.
[[nodiscard]] CutSolution solve_heuristic(const ContainmentProblem& problem, const SolveBudget& budget);

/// Dispatch: exact search for instances at or below the node limit, heuristic above.
[[nodiscard]] CutSolution solve(const ContainmentProblem& problem, const SolveBudget& budget,
                                std::size_t exact_node_limit);

}  // namespace fcfn::engine

#endif  // FCFN_ENGINE_SOLVER_HPP
