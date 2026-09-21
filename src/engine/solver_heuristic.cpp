// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Scalable deterministic construction for MCC-1.
//
// The heuristic repeatedly takes a canonical shortest residual path and adds its
// cheapest eligible member (ties broken by canonical resource identity), then
// restores minimality by reverse deletion in canonical order. It never claims
// optimality, and when it stops early the outcome is an explicit
// LimitReachedNoSolution rather than a false minimality claim.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "fcfn/engine/solver.hpp"
#include "solver_common.hpp"

namespace fcfn::engine {
namespace {

using detail::shortest_residual_path;

}  // namespace

CutSolution solve_heuristic(const ContainmentProblem& problem, const SolveBudget& budget) {
  CutSolution solution;
  solution.counters.candidate_nodes = static_cast<std::uint64_t>(problem.candidates().size());
  solution.counters.budget = budget.max_iterations;
  solution.counters.heuristic_used = true;

  const std::size_t n = problem.node_count();
  std::vector<std::uint8_t> included(n, 0);
  std::vector<model::NodeIndex> members;
  std::uint64_t total_weight = 0;

  auto add_member = [&](model::NodeIndex node) {
    included[node] = 1;
    members.push_back(node);
    total_weight += problem.weight(node);
  };

  for (const model::NodeIndex source : problem.sources()) {
    if (detail::is_mandatory_member(problem, source)) {
      add_member(source);
    }
  }

  std::uint64_t iterations = 0;
  bool reached_limit = false;
  for (;;) {
    if (iterations >= budget.max_iterations) {
      reached_limit = true;
      solution.counters.budget_exhausted = true;
      break;
    }
    ++iterations;
    const std::vector<model::NodeIndex> path = shortest_residual_path(problem, included);
    if (path.empty()) {
      break;
    }
    model::NodeIndex cheapest = model::kInvalidNode;
    std::uint64_t cheapest_weight = 0;
    for (const model::NodeIndex node : path) {
      if (problem.is_containable(node) == 0) {
        continue;
      }
      const std::uint64_t weight = problem.weight(node);
      if (cheapest == model::kInvalidNode || weight < cheapest_weight ||
          (weight == cheapest_weight && problem.resource_ids()[node] < problem.resource_ids()[cheapest])) {
        cheapest = node;
        cheapest_weight = weight;
      }
    }
    if (cheapest == model::kInvalidNode) {
      // A residual path with no eligible member proves this instance infeasible.
      solution.status = SolveStatus::ProvenInfeasible;
      solution.has_infeasibility_witness = true;
      solution.infeasibility_path = path;
      return solution;
    }
    add_member(cheapest);
  }

  if (reached_limit) {
    solution.status = SolveStatus::LimitReachedNoSolution;
    solution.counters.explored_nodes = iterations;
    return solution;
  }

  // Minimality restoration: reverse deletion in canonical ascending order.
  std::sort(members.begin(), members.end());
  for (std::size_t i = 0; i < members.size();) {
    const model::NodeIndex candidate = members[i];
    if (detail::is_mandatory_member(problem, candidate)) {
      ++i;
      continue;
    }
    std::vector<model::NodeIndex> trial;
    trial.reserve(members.size() - 1);
    for (const model::NodeIndex member : members) {
      if (member != candidate) {
        trial.push_back(member);
      }
    }
    if (verify_cut(problem, trial).ok()) {
      members = std::move(trial);
    } else {
      ++i;
    }
  }

  const Result<ObjectiveVector> objective = verify_cut(problem, members);
  if (!objective.ok()) {
    // The construction could not satisfy every hard constraint (for example the
    // weight bound). Feasibility stays unknown: never reported as infeasible.
    solution.status = SolveStatus::LimitReachedNoSolution;
    solution.counters.explored_nodes = iterations;
    return solution;
  }

  solution.members = std::move(members);
  solution.objective = objective.value();
  solution.status = SolveStatus::FeasibleNotProvenOptimal;
  solution.counters.explored_nodes = iterations;
  return solution;
}

CutSolution solve(const ContainmentProblem& problem, const SolveBudget& budget, std::size_t exact_node_limit) {
  if (problem.candidates().size() <= exact_node_limit) {
    return solve_exact(problem, budget);
  }
  CutSolution heuristic = solve_heuristic(problem, budget);
  if (heuristic.has_solution() || heuristic.status == SolveStatus::ProvenInfeasible) {
    return heuristic;
  }
  return heuristic;
}

}  // namespace fcfn::engine
