// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/engine/cut.hpp"

#include <algorithm>
#include <deque>
#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/checked.hpp"

namespace fcfn::engine {
namespace {

constexpr const char* kSolveStatusTokens[] = {
    "proven_optimal",
    "feasible_not_proven_optimal",
    "proven_infeasible",
    "limit_reached_no_solution",
    "invalid_problem",
};

}  // namespace

const char* to_string(SolveStatus value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index < (sizeof(kSolveStatusTokens) / sizeof(kSolveStatusTokens[0]))) {
    return kSolveStatusTokens[index];
  }
  return "unrecognised_solve_status";
}

bool operator<(const ObjectiveVector& a, const ObjectiveVector& b) noexcept {
  // The documented default order minimises protected inclusions first. It is
  // defined in terms of objective_less so the two orders can never disagree.
  return objective_less(a, b, true);
}

bool operator==(const ObjectiveVector& a, const ObjectiveVector& b) noexcept {
  return a.protected_inclusions == b.protected_inclusions && a.total_weight == b.total_weight &&
         a.cardinality == b.cardinality && a.signature == b.signature;
}

bool objective_less(const ObjectiveVector& a, const ObjectiveVector& b, bool minimize_protected) noexcept {
  if (minimize_protected && a.protected_inclusions != b.protected_inclusions) {
    return a.protected_inclusions < b.protected_inclusions;
  }
  if (a.total_weight != b.total_weight) {
    return a.total_weight < b.total_weight;
  }
  if (a.cardinality != b.cardinality) {
    return a.cardinality < b.cardinality;
  }
  return std::lexicographical_compare(a.signature.begin(), a.signature.end(), b.signature.begin(),
                                      b.signature.end());
}

Result<ContainmentProblem> ContainmentProblem::build(const PropagationGraph& graph,
                                                     const ContainmentInstanceSpec& spec) {
  if (!graph.valid()) {
    return Status{StatusCode::InvalidArgument, "propagation graph was not built"};
  }
  if (spec.failure_sources.empty()) {
    return Status{StatusCode::InvalidArgument, "containment instance requires at least one failure source"};
  }
  if (spec.protected_obligations.empty()) {
    return Status{StatusCode::InvalidArgument, "containment instance requires at least one protected obligation"};
  }
  if (spec.failure_sources.size() > kMaxFailureSources) {
    return Status{StatusCode::LimitExceeded, "failure source count exceeds bound"};
  }
  if (spec.protected_obligations.size() > kMaxProtectedObligations) {
    return Status{StatusCode::LimitExceeded, "protected obligation count exceeds bound"};
  }
  if (spec.max_members == 0 || spec.max_members > kMaxBoundaryMembers) {
    return Status{StatusCode::InvalidArgument, "member bound out of range"};
  }

  const model::Topology& topology = graph.topology();
  ContainmentProblem problem;
  problem.graph_ = &graph;
  problem.node_count_ = topology.node_count();
  problem.require_source_inclusion_ = spec.require_source_inclusion;
  problem.allow_protected_inclusion_ = spec.allow_protected_inclusion;
  problem.minimize_protected_inclusions_ = spec.minimize_protected_inclusions;
  problem.max_total_weight_ = spec.max_total_weight;
  problem.max_members_ = spec.max_members;

  const std::size_t n = problem.node_count_;
  problem.containable_.assign(n, 0);
  problem.protected_mask_.assign(n, 0);
  problem.source_mask_.assign(n, 0);
  problem.candidate_mask_.assign(n, 0);
  problem.relevant_.assign(n, 0);
  problem.weights_.assign(n, 0);
  problem.resource_ids_.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    const model::NodeSpec& node = topology.node(static_cast<model::NodeIndex>(i));
    problem.resource_ids_.push_back(node.id);
    problem.weights_[i] = node.weight;
    problem.containable_[i] = static_cast<std::uint8_t>(node.containable ? 1 : 0);
  }

  for (const model::ResourceId& id : spec.forced_ineligible) {
    const auto index = topology.find(id);
    if (!index.has_value()) {
      return Status{StatusCode::NotFound, "forced-ineligible resource is not in the topology"};
    }
    problem.containable_[*index] = 0;
    problem.barred_.push_back(*index);
  }
  std::sort(problem.barred_.begin(), problem.barred_.end());
  problem.barred_.erase(std::unique(problem.barred_.begin(), problem.barred_.end()),
                        problem.barred_.end());

  for (const model::ResourceId& id : spec.failure_sources) {
    const auto index = topology.find(id);
    if (!index.has_value()) {
      return Status{StatusCode::NotFound, "failure source is not present in the topology"};
    }
    if (problem.source_mask_[*index] != 0) {
      return Status{StatusCode::AlreadyExists, "duplicate failure source"};
    }
    problem.source_mask_[*index] = 1;
    problem.sources_.push_back(*index);
    if (problem.protected_mask_[*index] != 0) {
      return Status{StatusCode::InvalidArgument, "a resource cannot be both failed and protected"};
    }
  }

  for (const model::ResourceId& id : spec.protected_obligations) {
    const auto index = topology.find(id);
    if (!index.has_value()) {
      return Status{StatusCode::NotFound, "protected obligation is not present in the topology"};
    }
    if (problem.protected_mask_[*index] != 0) {
      return Status{StatusCode::AlreadyExists, "duplicate protected obligation"};
    }
    problem.protected_mask_[*index] = 1;
    problem.protected_.push_back(*index);
    // Protected obligations are protected because this instance says so. They are
    // only containable when the policy explicitly authorizes it.
    problem.containable_[*index] = spec.allow_protected_inclusion ? 1 : 0;
  }
  std::sort(problem.protected_.begin(), problem.protected_.end());

  for (const model::NodeIndex source : problem.sources_) {
    if (problem.containable_[source] == 0) {
      problem.uncontainable_sources_.push_back(source);
    }
  }

  // Propagation frontier: everything reachable from a source in G+.
  {
    std::vector<std::uint8_t> seen(n, 0);
    std::deque<model::NodeIndex> queue;
    for (const model::NodeIndex source : problem.sources_) {
      if (seen[source] == 0) {
        seen[source] = 1;
        queue.push_back(source);
      }
    }
    while (!queue.empty()) {
      const model::NodeIndex node = queue.front();
      queue.pop_front();
      problem.frontier_.push_back(node);
      for (const model::NodeIndex next : graph.conservative_successors(node)) {
        if (seen[next] == 0) {
          seen[next] = 1;
          queue.push_back(next);
        }
      }
    }
    std::sort(problem.frontier_.begin(), problem.frontier_.end());
  }

  // Reverse frontier: everything that can still reach a protected obligation.
  {
    std::vector<std::uint8_t> seen(n, 0);
    std::deque<model::NodeIndex> queue;
    for (const model::NodeIndex obligation : problem.protected_) {
      if (seen[obligation] == 0) {
        seen[obligation] = 1;
        queue.push_back(obligation);
      }
    }
    while (!queue.empty()) {
      const model::NodeIndex node = queue.front();
      queue.pop_front();
      problem.relevant_[node] = 1;
      for (const model::NodeIndex previous : graph.conservative_predecessors(node)) {
        if (seen[previous] == 0) {
          seen[previous] = 1;
          queue.push_back(previous);
        }
      }
    }
  }

  // Candidates: nodes that lie on some source-to-protected path, plus eligible
  // mandatory sources. No other node can appear in an optimal cut: removing it
  // would not disconnect anything while still increasing the objective.
  for (const model::NodeIndex node : problem.frontier_) {
    if (problem.relevant_[node] != 0) {
      problem.candidate_mask_[node] = 1;
    }
  }
  if (spec.require_source_inclusion) {
    for (const model::NodeIndex source : problem.sources_) {
      if (problem.containable_[source] != 0) {
        problem.candidate_mask_[source] = 1;
      }
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (problem.candidate_mask_[i] != 0) {
      problem.candidates_.push_back(static_cast<model::NodeIndex>(i));
    }
  }

  CanonicalWriter writer;
  writer.digest(topology.definition_digest());
  writer.u32(static_cast<std::uint32_t>(spec.failure_sources.size()));
  for (const model::NodeIndex source : problem.sources_) {
    writer.u32(source);
  }
  writer.u32(static_cast<std::uint32_t>(spec.protected_obligations.size()));
  for (const model::NodeIndex obligation : problem.protected_) {
    writer.u32(obligation);
  }
  writer.u32(static_cast<std::uint32_t>(problem.uncontainable_sources_.size()));
  for (const model::NodeIndex source : problem.uncontainable_sources_) {
    writer.u32(source);
  }
  writer.u32(static_cast<std::uint32_t>(problem.candidates_.size()));
  for (const model::NodeIndex candidate : problem.candidates_) {
    writer.u32(candidate);
    writer.u64(problem.weights_[candidate]);
  }
  writer.u32(static_cast<std::uint32_t>(problem.barred_.size()));
  for (const model::NodeIndex node : problem.barred_) {
    writer.u32(node);
  }
  writer.boolean(spec.require_source_inclusion);
  writer.boolean(spec.allow_protected_inclusion);
  writer.boolean(spec.minimize_protected_inclusions);
  writer.u64(spec.max_total_weight);
  writer.u64(static_cast<std::uint64_t>(spec.max_members));
  problem.instance_digest_ = writer.digest();

  return problem;
}

bool source_reaches_protected(const ContainmentProblem& problem, std::span<const model::NodeIndex> excluded) {
  const std::size_t n = problem.node_count();
  std::vector<std::uint8_t> blocked(n, 0);
  for (const model::NodeIndex node : excluded) {
    blocked[node] = 1;
  }
  std::vector<std::uint8_t> seen(n, 0);
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

bool cut_is_valid(const ContainmentProblem& problem, std::span<const model::NodeIndex> removed) {
  return !source_reaches_protected(problem, removed);
}

std::vector<model::NodeIndex> find_witness_path(const ContainmentProblem& problem,
                                                std::span<const model::NodeIndex> excluded,
                                                bool* uses_unknown) {
  const std::size_t n = problem.node_count();
  std::vector<std::uint8_t> blocked(n, 0);
  for (const model::NodeIndex node : excluded) {
    blocked[node] = 1;
  }
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
  if (uses_unknown != nullptr) {
    bool unknown = false;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      const std::span<const model::NodeIndex> proven = problem.graph().proven_successors(path[i]);
      const bool proven_edge = std::binary_search(proven.begin(), proven.end(), path[i + 1]);
      if (!proven_edge && problem.graph().has_unknown_edge(path[i], path[i + 1])) {
        unknown = true;
        break;
      }
    }
    *uses_unknown = unknown;
  }
  return path;
}

Result<ObjectiveVector> verify_cut(const ContainmentProblem& problem,
                                   std::span<const model::NodeIndex> members) {
  if (members.size() > problem.max_members()) {
    return Status{StatusCode::LimitExceeded, "containment set exceeds the member bound"};
  }
  std::vector<model::NodeIndex> sorted(members.begin(), members.end());
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i] == sorted[i - 1]) {
      return Status{StatusCode::AlreadyExists, "duplicate containment member"};
    }
  }
  std::uint64_t total_weight = 0;
  std::uint64_t protected_inclusions = 0;
  for (const model::NodeIndex node : sorted) {
    if (node >= problem.node_count()) {
      return Status{StatusCode::InvalidArgument, "containment member index out of range"};
    }
    if (!problem.is_containable(node)) {
      return Status{StatusCode::Denied, "containment member is not eligible for containment"};
    }
    if (problem.is_protected(node) && !problem.allow_protected_inclusion()) {
      return Status{StatusCode::Denied, "protected obligation may not be contained by this policy"};
    }
    if (problem.is_protected(node)) {
      ++protected_inclusions;
    }
    if (!checked_add(total_weight, problem.weight(node), total_weight)) {
      return Status{StatusCode::LimitExceeded, "containment weight overflowed"};
    }
  }
  if (total_weight > problem.max_total_weight()) {
    return Status{StatusCode::LimitExceeded, "containment set exceeds the weight bound"};
  }
  if (problem.require_source_inclusion()) {
    for (const model::NodeIndex source : problem.sources()) {
      if (problem.is_containable(source) && !std::binary_search(sorted.begin(), sorted.end(), source)) {
        return Status{StatusCode::Denied, "eligible failure source is not contained"};
      }
    }
  }
  if (source_reaches_protected(problem, sorted)) {
    return Status{StatusCode::Denied,
                  "containment set does not disconnect failure sources from protected obligations"};
  }
  ObjectiveVector objective;
  objective.protected_inclusions = protected_inclusions;
  objective.total_weight = total_weight;
  objective.cardinality = static_cast<std::uint64_t>(sorted.size());
  objective.signature.reserve(sorted.size());
  for (const model::NodeIndex node : sorted) {
    objective.signature.push_back(problem.resource_ids()[node]);
  }
  return objective;
}

Result<ObjectiveVector> objective_of(const ContainmentProblem& problem,
                                     std::span<const model::NodeIndex> members) {
  return verify_cut(problem, members);
}

bool verify_infeasibility_witness(const ContainmentProblem& problem,
                                  std::span<const model::NodeIndex> path) {
  if (path.size() < 2) {
    return false;
  }
  if (!problem.is_source(path.front())) {
    return false;
  }
  if (!problem.is_protected(path.back())) {
    return false;
  }
  for (const model::NodeIndex node : path) {
    if (problem.is_containable(node)) {
      return false;
    }
  }
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const std::span<const model::NodeIndex> successors = problem.graph().conservative_successors(path[i]);
    if (!std::binary_search(successors.begin(), successors.end(), path[i + 1])) {
      return false;
    }
  }
  return true;
}

}  // namespace fcfn::engine
