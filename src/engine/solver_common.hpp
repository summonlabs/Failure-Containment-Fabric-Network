// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal helpers shared by the FCFN solvers. Not installed.
#ifndef FCFN_SRC_ENGINE_SOLVER_COMMON_HPP
#define FCFN_SRC_ENGINE_SOLVER_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "fcfn/engine/cut.hpp"

namespace fcfn::engine::detail {

/// Shortest path (in hops) from any source to any protected obligation in G+,
/// avoiding every node marked in "blocked". Returns an empty vector when no such
/// path exists. The returned path is canonical: it is the lexicographically
/// smallest shortest path, because successors are visited in ascending order
/// and the first newly discovered predecessor wins.
[[nodiscard]] inline std::vector<model::NodeIndex> shortest_residual_path(
    const ContainmentProblem& problem, const std::vector<std::uint8_t>& blocked) {
  const std::size_t n = problem.node_count();
  std::vector<std::uint8_t> seen(n, 0);
  std::vector<model::NodeIndex> parent(n, model::kInvalidNode);
  std::deque<model::NodeIndex> queue;
  for (const model::NodeIndex source : problem.sources()) {
    if (blocked[source] == 0 && seen[source] == 0) {
      seen[source] = 1;
      queue.push_back(source);
    }
  }
  model::NodeIndex target = model::kInvalidNode;
  while (!queue.empty() && target == model::kInvalidNode) {
    const model::NodeIndex node = queue.front();
    queue.pop_front();
    for (const model::NodeIndex next : problem.graph().conservative_successors(node)) {
      if (blocked[next] != 0 || seen[next] != 0) {
        continue;
      }
      seen[next] = 1;
      parent[next] = node;
      if (problem.is_protected(next)) {
        target = next;
        break;
      }
      queue.push_back(next);
    }
  }
  if (target == model::kInvalidNode) {
    return {};
  }
  std::vector<model::NodeIndex> path;
  for (model::NodeIndex node = target; node != model::kInvalidNode; node = parent[node]) {
    path.push_back(node);
    if (problem.is_source(node)) {
      break;
    }
  }
  std::reverse(path.begin(), path.end());
  return path;
}

/// A node that policy requires inside every containment set.
[[nodiscard]] inline bool is_mandatory_member(const ContainmentProblem& problem, model::NodeIndex node) {
  return problem.require_source_inclusion() && problem.is_source(node) && problem.is_containable(node);
}

/// Upper bound on the remaining work: how many distinct containment members the
/// instance can still accept.
[[nodiscard]] inline std::size_t remaining_member_capacity(const ContainmentProblem& problem,
                                                           std::size_t used) {
  if (used >= problem.max_members()) {
    return 0;
  }
  return problem.max_members() - used;
}

}  // namespace fcfn::engine::detail

#endif  // FCFN_SRC_ENGINE_SOLVER_COMMON_HPP
