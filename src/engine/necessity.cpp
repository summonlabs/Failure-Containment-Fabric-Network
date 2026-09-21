// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/engine/necessity.hpp"

#include <algorithm>

namespace fcfn::engine {
namespace {

/// Solve the instance with one node barred from the containment set.
SolveStatus status_without(const ContainmentProblem& base, model::NodeIndex barred,
                           const SolveBudget& budget, SolveCounters& counters) {
  // Rebuild the instance through the public builder so the modified instance
  // goes through exactly the same validation as any other instance.
  ContainmentInstanceSpec spec;
  for (const model::NodeIndex source : base.sources()) {
    spec.failure_sources.push_back(base.resource_ids()[source]);
  }
  for (const model::NodeIndex obligation : base.protected_nodes()) {
    spec.protected_obligations.push_back(base.resource_ids()[obligation]);
  }
  // Every node the base instance already barred stays barred: dropping them would
  // make nodes containable again and could turn a necessary member into an
  // apparently unnecessary one.
  for (const model::NodeIndex node : base.barred_nodes()) {
    spec.forced_ineligible.push_back(base.resource_ids()[node]);
  }
  spec.forced_ineligible.push_back(base.resource_ids()[barred]);
  spec.require_source_inclusion = base.require_source_inclusion();
  spec.allow_protected_inclusion = base.allow_protected_inclusion();
  spec.minimize_protected_inclusions = base.minimize_protected_inclusions();
  spec.max_total_weight = base.max_total_weight();
  spec.max_members = base.max_members();

  const Result<ContainmentProblem> rebuilt = ContainmentProblem::build(base.graph(), spec);
  if (!rebuilt.ok()) {
    return SolveStatus::InvalidProblem;
  }
  const CutSolution solution = solve_exact(rebuilt.value(), budget);
  counters.explored_nodes += solution.counters.explored_nodes;
  counters.residual_paths_checked += solution.counters.residual_paths_checked;
  counters.pruned_branches += solution.counters.pruned_branches;
  if (solution.counters.budget_exhausted) {
    counters.budget_exhausted = true;
  }
  return solution.status;
}

NecessityResult analyze(const ContainmentProblem& problem, const std::vector<model::NodeIndex>& subjects,
                        const SolveBudget& budget) {
  NecessityResult result;
  for (const model::NodeIndex node : subjects) {
    if (!problem.is_containable(node)) {
      result.not_necessary.push_back(node);
      continue;
    }
    if (problem.require_source_inclusion() && problem.is_source(node)) {
      // Policy requires every eligible failure source inside the containment
      // set, so this member is necessary by construction.
      result.proven_necessary.push_back(node);
      continue;
    }
    const SolveStatus status = status_without(problem, node, budget, result.counters);
    switch (status) {
      case SolveStatus::ProvenInfeasible:
        result.proven_necessary.push_back(node);
        break;
      case SolveStatus::ProvenOptimal:
      case SolveStatus::FeasibleNotProvenOptimal:
        result.not_necessary.push_back(node);
        break;
      case SolveStatus::LimitReachedNoSolution:
      case SolveStatus::InvalidProblem:
        result.indeterminate.push_back(node);
        break;
    }
    if (result.counters.budget_exhausted) {
      // Once the budget is gone, remaining subjects cannot be decided.
      continue;
    }
  }
  return result;
}

}  // namespace

NecessityResult analyze_necessity(const ContainmentProblem& problem,
                                  std::span<const model::NodeIndex> members,
                                  const SolveBudget& budget) {
  std::vector<model::NodeIndex> subjects(members.begin(), members.end());
  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
  return analyze(problem, subjects, budget);
}

NecessityResult analyze_necessity_exhaustive(const ContainmentProblem& problem, const SolveBudget& budget) {
  std::vector<model::NodeIndex> subjects;
  for (const model::NodeIndex node : problem.candidates()) {
    if (problem.is_containable(node) != 0) {
      subjects.push_back(node);
    }
  }
  std::sort(subjects.begin(), subjects.end());
  return analyze(problem, subjects, budget);
}

}  // namespace fcfn::engine
