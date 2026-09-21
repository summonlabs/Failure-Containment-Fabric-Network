// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "reference_solver.hpp"

#include <algorithm>
#include <deque>
#include <limits>

namespace fcfn::test {

bool reference_reaches(const engine::ContainmentProblem& problem,
                       const std::vector<model::NodeIndex>& removed) {
  const std::size_t n = problem.node_count();
  std::vector<bool> blocked(n, false);
  for (const model::NodeIndex node : removed) {
    blocked[node] = true;
  }
  std::vector<bool> seen(n, false);
  std::deque<model::NodeIndex> queue;
  for (const model::NodeIndex source : problem.sources()) {
    if (!blocked[source] && !seen[source]) {
      seen[source] = true;
      queue.push_back(source);
    }
  }
  while (!queue.empty()) {
    const model::NodeIndex node = queue.front();
    queue.pop_front();
    if (problem.is_protected(node)) {
      return true;
    }
    for (const model::NodeIndex next : problem.graph().conservative_successors(node)) {
      if (!blocked[next] && !seen[next]) {
        seen[next] = true;
        queue.push_back(next);
      }
    }
  }
  return false;
}

ReferenceResult reference_solve(const engine::ContainmentProblem& problem) {
  ReferenceResult result;
  std::vector<model::NodeIndex> eligible;
  for (model::NodeIndex node = 0; node < problem.node_count(); ++node) {
    if (problem.is_containable(node)) {
      eligible.push_back(node);
    }
  }
  if (eligible.size() > kReferenceSolverEligibleLimit) {
    result.supported = false;
    return result;
  }

  const std::uint64_t total = 1ull << eligible.size();
  for (std::uint64_t mask = 0; mask < total; ++mask) {
    ++result.examined;
    std::vector<model::NodeIndex> members;
    std::uint64_t weight = 0;
    std::uint64_t protected_inclusions = 0;
    bool valid = true;
    for (std::size_t bit = 0; bit < eligible.size(); ++bit) {
      if ((mask & (1ull << bit)) == 0) {
        continue;
      }
      const model::NodeIndex node = eligible[bit];
      members.push_back(node);
      weight += problem.weight(node);
      if (problem.is_protected(node)) {
        ++protected_inclusions;
        if (!problem.allow_protected_inclusion()) {
          valid = false;
          break;
        }
      }
    }
    if (!valid) {
      continue;
    }
    if (members.size() > problem.max_members() || weight > problem.max_total_weight()) {
      continue;
    }
    if (problem.require_source_inclusion()) {
      for (const model::NodeIndex source : problem.sources()) {
        if (!problem.is_containable(source)) {
          continue;
        }
        if (std::find(members.begin(), members.end(), source) == members.end()) {
          valid = false;
          break;
        }
      }
      if (!valid) {
        continue;
      }
    }
    if (reference_reaches(problem, members)) {
      continue;
    }
    std::sort(members.begin(), members.end());
    engine::ObjectiveVector objective;
    objective.protected_inclusions = protected_inclusions;
    objective.total_weight = weight;
    objective.cardinality = static_cast<std::uint64_t>(members.size());
    objective.signature.reserve(members.size());
    for (const model::NodeIndex node : members) {
      objective.signature.push_back(problem.resource_ids()[node]);
    }
    if (!result.feasible ||
        engine::objective_less(objective, result.objective, problem.minimize_protected_inclusions())) {
      result.feasible = true;
      result.objective = objective;
      result.members = members;
    }
  }
  return result;
}

}  // namespace fcfn::test
