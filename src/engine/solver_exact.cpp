// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Complete branch-and-bound solver for problem class MCC-1.
//
// Branching rule (complete): take a canonical shortest residual path from a
// source to a protected obligation in the conservative graph. Any feasible
// containment set must contain at least one node of that path; let path[i] be
// the first such node. The search therefore branches over i: every node before
// i is excluded from the containment set, and path[i] is included. This
// enumerates every feasible set exactly once up to the canonical order of the
// enumeration, so a completed search proves optimality.
//
// Pruning uses a valid lower bound: greedily collected vertex-disjoint residual
// paths each require at least one member, so the sum of per-path minimum weights
// is a lower bound on the additional containment weight.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "fcfn/core/checked.hpp"
#include "fcfn/engine/solver.hpp"
#include "solver_common.hpp"

namespace fcfn::engine {
namespace {

using detail::shortest_residual_path;

/// Disjoint-path lower bound. Returns false when the branch is provably
/// uncompletable (a residual path carries no eligible node), in which case the
/// path is reported through "witness".
bool disjoint_path_bound(const ContainmentProblem& problem, const std::vector<std::uint8_t>& included,
                         const std::vector<std::uint8_t>& excluded, std::uint64_t& bound,
                         std::vector<model::NodeIndex>* witness, SolveCounters& counters,
                         std::uint64_t iteration_cap) {
  std::vector<std::uint8_t> blocked = included;
  bound = 0;
  std::uint64_t iterations = 0;
  while (iterations < iteration_cap) {
    ++iterations;
    ++counters.residual_paths_checked;
    const std::vector<model::NodeIndex> path = shortest_residual_path(problem, blocked);
    if (path.empty()) {
      return true;
    }
    model::NodeIndex cheapest = model::kInvalidNode;
    std::uint64_t cheapest_weight = std::numeric_limits<std::uint64_t>::max();
    for (const model::NodeIndex node : path) {
      if (problem.is_containable(node) == 0 || excluded[node] != 0) {
        continue;
      }
      const std::uint64_t weight = problem.weight(node);
      if (weight < cheapest_weight ||
          (weight == cheapest_weight && cheapest != model::kInvalidNode && node < cheapest)) {
        cheapest = node;
        cheapest_weight = weight;
      }
    }
    if (cheapest == model::kInvalidNode) {
      // No eligible node on this path: the branch cannot be completed.
      if (witness != nullptr) {
        *witness = path;
      }
      return false;
    }
    if (!checked_add(bound, cheapest_weight, bound)) {
      bound = std::numeric_limits<std::uint64_t>::max();
      return true;
    }
    // The path is consumed: block the cheapest eligible node and every other
    // eligible node on the path so the next path is vertex-disjoint from it.
    for (const model::NodeIndex node : path) {
      if (problem.is_containable(node) != 0 && excluded[node] == 0) {
        blocked[node] = 1;
      }
    }
  }
  return true;
}

class ExactSearch {
 public:
  ExactSearch(const ContainmentProblem& problem, const SolveBudget& budget)
      : problem_(problem), budget_(budget) {
    const std::size_t n = problem.node_count();
    included_.assign(n, 0);
    excluded_.assign(n, 0);
    counters_.candidate_nodes = static_cast<std::uint64_t>(problem.candidates().size());
    counters_.budget = budget.max_explored_nodes;
  }

  CutSolution run() {
    // A mandatory eligible source must be contained: it is part of every
    // feasible set, so include it before searching.
    for (const model::NodeIndex source : problem_.sources()) {
      if (detail::is_mandatory_member(problem_, source)) {
        include(source);
      }
    }
    search();
    CutSolution solution;
    solution.counters = counters_;
    if (has_best_) {
      solution.members = best_members_;
      solution.objective = best_objective_;
      solution.status = counters_.budget_exhausted ? SolveStatus::FeasibleNotProvenOptimal
                                                   : SolveStatus::ProvenOptimal;
      return solution;
    }
    if (counters_.budget_exhausted) {
      solution.status = SolveStatus::LimitReachedNoSolution;
      return solution;
    }
    solution.status = SolveStatus::ProvenInfeasible;
    if (has_infeasibility_witness_) {
      solution.has_infeasibility_witness = true;
      solution.infeasibility_path = infeasibility_witness_;
    }
    return solution;
  }

 private:
  void include(model::NodeIndex node) {
    included_[node] = 1;
    current_.push_back(node);
    current_weight_ += problem_.weight(node);
    if (problem_.is_protected(node)) {
      current_protected_ += 1;
    }
  }

  void uninclude(model::NodeIndex node) {
    included_[node] = 0;
    if (!current_.empty() && current_.back() == node) {
      current_.pop_back();
    } else {
      const auto it = std::find(current_.begin(), current_.end(), node);
      if (it != current_.end()) {
        current_.erase(it);
      }
    }
    current_weight_ -= problem_.weight(node);
    if (problem_.is_protected(node)) {
      current_protected_ -= 1;
    }
  }

  void search() {
    if (aborted_) {
      return;
    }
    ++counters_.explored_nodes;
    if (counters_.explored_nodes > budget_.max_explored_nodes) {
      aborted_ = true;
      counters_.budget_exhausted = true;
      return;
    }
    if (current_.size() > problem_.max_members() || current_weight_ > problem_.max_total_weight()) {
      return;
    }

    const std::vector<model::NodeIndex> path = shortest_residual_path(problem_, included_);

    // A residual path with no eligible node is a globally valid infeasibility
    // certificate: no containment set can contain any node of it. It is recorded
    // once and is independently checkable by
    // engine::verify_infeasibility_witness.
    if (!path.empty()) {
      bool any_containable = false;
      for (const model::NodeIndex node : path) {
        if (problem_.is_containable(node) != 0) {
          any_containable = true;
          break;
        }
      }
      if (!any_containable) {
        if (!has_infeasibility_witness_ && path.size() <= kMaxCutCertificatePathNodes) {
          has_infeasibility_witness_ = true;
          infeasibility_witness_ = path;
        }
        return;
      }
    }

    if (path.empty()) {
      const Result<ObjectiveVector> objective = verify_cut(problem_, current_);
      if (!objective.ok()) {
        return;
      }
      if (!has_best_ ||
          objective_less(objective.value(), best_objective_, problem_.minimize_protected_inclusions())) {
        has_best_ = true;
        best_objective_ = objective.value();
        best_members_ = current_;
        std::sort(best_members_.begin(), best_members_.end());
        best_members_.erase(std::unique(best_members_.begin(), best_members_.end()), best_members_.end());
      }
      return;
    }

    if (has_best_) {
      std::uint64_t weight_bound = 0;
      std::vector<model::NodeIndex> branch_witness;
      const bool completable = disjoint_path_bound(problem_, included_, excluded_, weight_bound,
                                                   &branch_witness, counters_, kBoundIterations);
      if (!completable) {
        return;
      }
      ObjectiveVector lower;
      lower.protected_inclusions = current_protected_;
      lower.total_weight = current_weight_;
      if (!checked_add(lower.total_weight, weight_bound, lower.total_weight)) {
        lower.total_weight = std::numeric_limits<std::uint64_t>::max();
      }
      lower.cardinality = static_cast<std::uint64_t>(current_.size());
      if (objective_less(best_objective_, lower, problem_.minimize_protected_inclusions())) {
        ++counters_.pruned_branches;
        return;
      }
    }

    std::vector<model::NodeIndex> excluded_here;
    for (std::size_t i = 0; i < path.size(); ++i) {
      const model::NodeIndex node = path[i];
      if (excluded_[node] != 0 || problem_.is_containable(node) == 0) {
        continue;
      }
      include(node);
      search();
      uninclude(node);
      if (aborted_) {
        break;
      }
      if (detail::is_mandatory_member(problem_, node)) {
        // A mandatory member may not be excluded, so no further branch on this
        // path is legal.
        break;
      }
      excluded_[node] = 1;
      excluded_here.push_back(node);
    }
    for (const model::NodeIndex node : excluded_here) {
      excluded_[node] = 0;
    }

    if (!aborted_ && !has_best_ && excluded_here.size() == path.size()) {
      // Every node of the path was excluded without a completed branch: this
      // branch is uncompletable. At the root this is an infeasibility witness.
      if (current_.empty() && !has_infeasibility_witness_) {
        has_infeasibility_witness_ = true;
        infeasibility_witness_ = path;
      }
    }
  }

  static constexpr std::uint64_t kBoundIterations = 32;

  const ContainmentProblem& problem_;
  SolveBudget budget_;
  std::vector<std::uint8_t> included_;
  std::vector<std::uint8_t> excluded_;
  std::vector<model::NodeIndex> current_;
  std::vector<model::NodeIndex> best_members_;
  ObjectiveVector best_objective_{};
  std::uint64_t current_weight_{0};
  std::uint64_t current_protected_{0};
  bool has_best_{false};
  bool aborted_{false};
  bool has_infeasibility_witness_{false};
  std::vector<model::NodeIndex> infeasibility_witness_{};
  SolveCounters counters_{};
};

}  // namespace

CutSolution solve_exact(const ContainmentProblem& problem, const SolveBudget& budget) {
  if (problem.node_count() == 0 || problem.sources().empty() || problem.protected_nodes().empty()) {
    CutSolution solution;
    solution.status = SolveStatus::InvalidProblem;
    return solution;
  }
  ExactSearch search(problem, budget);
  return search.run();
}

}  // namespace fcfn::engine
