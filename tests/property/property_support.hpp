// FCFN property suite: independent MCC-1 constraints and enumeration.
//
// The helpers here restate the MCC-1 definition directly (eligibility, mandatory
// sources, protected policy, bounds, disconnection) and enumerate the complete
// feasible set of small instances. Nothing here calls the production solver or
// its verifier, so agreement is evidence rather than a tautology.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TESTS_PROPERTY_SUPPORT_HPP
#define FCFN_TESTS_PROPERTY_SUPPORT_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/solver.hpp"
#include "solver/case_builder.hpp"

namespace fcfn::test::property_cases {

using engine::ContainmentProblem;
using engine::CutSolution;
using engine::ObjectiveVector;
using engine::SolveBudget;
using engine::SolveStatus;
using solver_cases::CaseGraph;
using solver_cases::Instance;
using solver_cases::SpecOptions;

/// Independent re-statement of the five MCC-1 hard constraints.
struct ConstraintReport {
  bool eligibility{true};
  bool mandatory_sources{true};
  bool protected_policy{true};
  bool bounds{true};
  bool disconnected{true};

  [[nodiscard]] bool ok() const noexcept {
    return eligibility && mandatory_sources && protected_policy && bounds && disconnected;
  }

  [[nodiscard]] std::string describe() const {
    std::string text;
    if (!eligibility) {
      text += " eligibility";
    }
    if (!mandatory_sources) {
      text += " mandatory_sources";
    }
    if (!protected_policy) {
      text += " protected_policy";
    }
    if (!bounds) {
      text += " bounds";
    }
    if (!disconnected) {
      text += " disconnection";
    }
    return text.empty() ? std::string("all five constraints hold") : "violated:" + text;
  }
};

/// Independent breadth-first reachability over the conservative graph.
inline bool reaches_protected(const ContainmentProblem& problem,
                              std::span<const model::NodeIndex> removed) {
  const std::size_t n = problem.node_count();
  std::vector<std::uint8_t> blocked(n, 0);
  std::vector<std::uint8_t> seen(n, 0);
  for (const model::NodeIndex node : removed) {
    blocked[node] = 1;
  }
  std::deque<model::NodeIndex> queue;
  for (const model::NodeIndex source : problem.sources()) {
    if (blocked[source] == 0 && seen[source] == 0) {
      seen[source] = 1;
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
      if (blocked[next] == 0 && seen[next] == 0) {
        seen[next] = 1;
        queue.push_back(next);
      }
    }
  }
  return false;
}

inline ConstraintReport check_constraints(const ContainmentProblem& problem,
                                          std::span<const model::NodeIndex> members) {
  ConstraintReport report;
  std::vector<model::NodeIndex> sorted(members.begin(), members.end());
  std::sort(sorted.begin(), sorted.end());

  // 1. eligibility
  for (const model::NodeIndex node : sorted) {
    if (node >= problem.node_count() || !problem.is_containable(node)) {
      report.eligibility = false;
    }
  }
  // 4a. no repeated members
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    report.bounds = false;
  }
  // 3. protected policy
  for (const model::NodeIndex node : sorted) {
    if (problem.is_protected(node) && !problem.allow_protected_inclusion()) {
      report.protected_policy = false;
    }
  }
  // 4b. member and weight bounds
  if (sorted.size() > problem.max_members()) {
    report.bounds = false;
  }
  std::uint64_t weight = 0;
  for (const model::NodeIndex node : sorted) {
    const std::uint64_t node_weight = problem.weight(node);
    if (weight > problem.max_total_weight() - std::min(node_weight, problem.max_total_weight())) {
      weight = problem.max_total_weight() + 1;
      break;
    }
    weight += node_weight;
  }
  if (weight > problem.max_total_weight()) {
    report.bounds = false;
  }
  // 2. mandatory failure sources
  if (problem.require_source_inclusion()) {
    for (const model::NodeIndex source : problem.sources()) {
      if (problem.is_containable(source) &&
          !std::binary_search(sorted.begin(), sorted.end(), source)) {
        report.mandatory_sources = false;
      }
    }
  }
  // 5. disconnection in the conservative graph
  report.disconnected = !reaches_protected(problem, sorted);
  return report;
}

/// Bitmask over node indices, so masks from different variants of the same node
/// set are directly comparable.
[[nodiscard]] inline std::uint64_t node_mask(std::span<const model::NodeIndex> members) {
  std::uint64_t mask = 0;
  for (const model::NodeIndex node : members) {
    mask |= (1ull << node);
  }
  return mask;
}

/// Every containment set that satisfies all five constraints, as ascending node
/// masks. Exhaustive: instances must have at most 20 eligible nodes.
[[nodiscard]] inline std::vector<std::uint64_t> enumerate_feasible(const ContainmentProblem& problem) {
  std::vector<model::NodeIndex> eligible;
  for (model::NodeIndex node = 0; node < problem.node_count(); ++node) {
    if (problem.is_containable(node)) {
      eligible.push_back(node);
    }
  }
  std::vector<std::uint64_t> feasible;
  if (eligible.size() > 20 || problem.node_count() > 60) {
    return feasible;
  }
  const std::uint64_t total = 1ull << eligible.size();
  std::vector<model::NodeIndex> members;
  for (std::uint64_t mask = 0; mask < total; ++mask) {
    members.clear();
    for (std::size_t bit = 0; bit < eligible.size(); ++bit) {
      if ((mask & (1ull << bit)) != 0) {
        members.push_back(eligible[bit]);
      }
    }
    if (check_constraints(problem, members).ok()) {
      feasible.push_back(node_mask(members));
    }
  }
  return feasible;
}

/// Solve the instance with one node barred from containment, through the public
/// builder so the variant is validated exactly like any other instance.
///
/// The variant must be the base instance plus one restriction. ContainmentProblem
/// every node the instance already bars (ContainmentProblem::barred_nodes) is
/// carried over before the newly barred node is added, so the variant is the base
/// instance plus exactly one restriction.
[[nodiscard]] inline CutSolution solve_without(const model::Topology& topology,
                                               const ContainmentProblem& base,
                                               model::NodeIndex barred,
                                               const SolveBudget& budget) {
  static_cast<void>(topology);
  engine::ContainmentInstanceSpec spec;
  for (const model::NodeIndex source : base.sources()) {
    spec.failure_sources.push_back(base.resource_ids()[source]);
  }
  for (const model::NodeIndex obligation : base.protected_nodes()) {
    spec.protected_obligations.push_back(base.resource_ids()[obligation]);
  }
  for (const model::NodeIndex node : base.barred_nodes()) {
    spec.forced_ineligible.push_back(base.resource_ids()[node]);
  }
  spec.forced_ineligible.push_back(base.resource_ids()[barred]);
  spec.require_source_inclusion = base.require_source_inclusion();
  spec.allow_protected_inclusion = base.allow_protected_inclusion();
  spec.minimize_protected_inclusions = base.minimize_protected_inclusions();
  spec.max_total_weight = base.max_total_weight();
  spec.max_members = base.max_members();
  Instance variant(topology, spec);
  return engine::solve_exact(variant.problem(), budget);
}

/// Sorted comparison helpers.
[[nodiscard]] inline bool is_subset(const std::vector<std::uint64_t>& subset,
                                    const std::vector<std::uint64_t>& superset) {
  return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
}

[[nodiscard]] inline std::string mask_text(std::uint64_t mask) {
  std::string text = "{";
  bool first = true;
  for (std::size_t bit = 0; bit < 64; ++bit) {
    if ((mask & (1ull << bit)) != 0) {
      if (!first) {
        text += ",";
      }
      text += "n" + std::to_string(bit);
      first = false;
    }
  }
  text += "}";
  return text;
}

}  // namespace fcfn::test::property_cases

#endif  // FCFN_TESTS_PROPERTY_SUPPORT_HPP
