// FCFN solver suite: shared construction helpers for MCC-1 instances.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TESTS_SOLVER_CASE_BUILDER_HPP
#define FCFN_TESTS_SOLVER_CASE_BUILDER_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/model/topology.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

namespace fcfn::test::solver_cases {

/// Check that also reports the instance parameters that produced the failure.
#define FCFN_CHECK_CTX(condition, context)                                                        \
  do {                                                                                            \
    if (!(condition)) {                                                                            \
      ::fcfn::test::fail(__FILE__, __LINE__, std::string("check failed: ") + #condition +          \
                                                 "\n  context: " + (context));                     \
    }                                                                                              \
  } while (false)

struct CaseNode {
  std::string id{};
  bool containable{true};
  std::uint64_t weight{1};
  bool protected_obligation{false};
};

struct CaseEdge {
  std::string from{};
  std::string to{};
  model::EdgeEvidence evidence{model::EdgeEvidence::Proven};
};

struct CaseGraph {
  std::vector<CaseNode> nodes{};
  std::vector<CaseEdge> edges{};
  std::uint64_t generation{1};
};

inline void add_node(CaseGraph& graph, std::string id, bool containable, std::uint64_t weight,
                     bool protected_obligation = false) {
  CaseNode node;
  node.id = std::move(id);
  node.containable = containable;
  node.weight = weight;
  node.protected_obligation = protected_obligation;
  graph.nodes.push_back(std::move(node));
}

inline void add_edge(CaseGraph& graph, std::string from, std::string to,
                     model::EdgeEvidence evidence = model::EdgeEvidence::Proven) {
  CaseEdge edge;
  edge.from = std::move(from);
  edge.to = std::move(to);
  edge.evidence = evidence;
  graph.edges.push_back(std::move(edge));
}

inline model::TopologySpec to_spec(const CaseGraph& graph) {
  model::TopologySpec spec;
  spec.generation = model::TopologyGeneration{graph.generation};
  for (const CaseNode& node : graph.nodes) {
    model::NodeSpec entry;
    entry.id = model::ResourceId::unchecked(node.id);
    entry.containable = node.containable;
    entry.weight = node.weight;
    entry.protected_obligation = node.protected_obligation;
    entry.completeness = model::AdjacencyCompleteness::Complete;
    spec.nodes.push_back(std::move(entry));
  }
  for (const CaseEdge& edge : graph.edges) {
    model::EdgeSpec entry;
    entry.from = model::ResourceId::unchecked(edge.from);
    entry.to = model::ResourceId::unchecked(edge.to);
    entry.evidence = edge.evidence;
    entry.generation = model::EvidenceGeneration{1};
    spec.edges.push_back(std::move(entry));
  }
  return spec;
}

/// Owns a topology, its indexed graph, and the built MCC-1 problem. The members
/// are constructed in place: the graph points at the owned topology and the
/// problem points at the owned graph, so the object is neither copyable nor
/// movable.
class Instance {
 public:
  Instance(model::Topology topology, const engine::ContainmentInstanceSpec& spec)
      : topology_(std::move(topology)) {
    graph_ = engine::PropagationGraph::build(topology_);
    const Result<engine::ContainmentProblem> built = engine::ContainmentProblem::build(graph_, spec);
    if (!built.ok()) {
      throw TestFailure("ContainmentProblem::build rejected the instance: " + built.status().to_string());
    }
    problem_ = built.value();
  }

  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;

  [[nodiscard]] const model::Topology& topology() const noexcept { return topology_; }
  [[nodiscard]] const engine::PropagationGraph& graph() const noexcept { return graph_; }
  [[nodiscard]] const engine::ContainmentProblem& problem() const noexcept { return problem_; }

 private:
  model::Topology topology_{};
  engine::PropagationGraph graph_{};
  engine::ContainmentProblem problem_{};
};

inline Instance make_instance(const CaseGraph& graph, const engine::ContainmentInstanceSpec& spec) {
  return Instance(require_topology(to_spec(graph)), spec);
}

inline Instance make_instance(model::TopologySpec spec, const engine::ContainmentInstanceSpec& instance_spec) {
  return Instance(require_topology(std::move(spec)), instance_spec);
}

/// Resource ids of every protected obligation in a materialised topology.
inline std::vector<std::string> protected_ids(const model::Topology& topology) {
  std::vector<std::string> ids;
  for (std::size_t i = 0; i < topology.node_count(); ++i) {
    if (topology.node(static_cast<model::NodeIndex>(i)).protected_obligation) {
      ids.push_back(topology.resource(static_cast<model::NodeIndex>(i)).value());
    }
  }
  return ids;
}

/// Resource ids of every node that is not protected.
inline std::vector<std::string> unprotected_ids(const model::Topology& topology) {
  std::vector<std::string> ids;
  for (std::size_t i = 0; i < topology.node_count(); ++i) {
    if (!topology.node(static_cast<model::NodeIndex>(i)).protected_obligation) {
      ids.push_back(topology.resource(static_cast<model::NodeIndex>(i)).value());
    }
  }
  return ids;
}

/// Add a back edge to the fixture's source node, creating a cycle.
///
/// The SyntheticGraphOptions::add_cycle option cannot be used: it appends n2->n0
/// without checking whether the generator already produced that edge, and
/// Topology::build rejects duplicate propagation edges.
inline void add_back_edge_to_source(model::TopologySpec& spec, std::size_t from_index) {
  if (from_index == 0 || from_index >= spec.nodes.size()) {
    return;
  }
  const model::ResourceId& from = spec.nodes[from_index].id;
  const model::ResourceId& to = spec.nodes[0].id;
  for (const model::EdgeSpec& edge : spec.edges) {
    if (edge.from == from && edge.to == to) {
      return;
    }
  }
  model::EdgeSpec edge;
  edge.from = from;
  edge.to = to;
  edge.evidence = model::EdgeEvidence::Proven;
  edge.generation = model::EvidenceGeneration{1};
  spec.edges.push_back(std::move(edge));
}

/// Knobs for deriving an MCC-1 instance specification from a topology.
struct SpecOptions {
  /// How many of the lowest-index non-protected nodes become failure sources.
  std::size_t source_count{1};
  /// Additional failure sources named explicitly.
  std::vector<std::string> extra_sources{};
  /// Nodes barred from containment (used to create uncontainable sources).
  std::vector<std::string> forced_ineligible{};
  bool require_source_inclusion{true};
  bool allow_protected_inclusion{false};
  bool minimize_protected_inclusions{true};
  std::size_t max_members{kMaxBoundaryMembers};
  std::uint64_t max_total_weight{kMaxPlanWeightSum};
};

inline engine::ContainmentInstanceSpec make_spec(const model::Topology& topology,
                                                 const SpecOptions& options) {
  engine::ContainmentInstanceSpec spec;
  std::size_t taken = 0;
  for (std::size_t i = 0; i < topology.node_count() && taken < options.source_count; ++i) {
    const auto index = static_cast<model::NodeIndex>(i);
    if (topology.node(index).protected_obligation) {
      continue;
    }
    spec.failure_sources.push_back(topology.resource(index));
    ++taken;
  }
  for (const std::string& id : options.extra_sources) {
    spec.failure_sources.push_back(model::ResourceId::unchecked(id));
  }
  for (const std::string& id : protected_ids(topology)) {
    spec.protected_obligations.push_back(model::ResourceId::unchecked(id));
  }
  for (const std::string& id : options.forced_ineligible) {
    spec.forced_ineligible.push_back(model::ResourceId::unchecked(id));
  }
  spec.require_source_inclusion = options.require_source_inclusion;
  spec.allow_protected_inclusion = options.allow_protected_inclusion;
  spec.minimize_protected_inclusions = options.minimize_protected_inclusions;
  spec.max_members = options.max_members;
  spec.max_total_weight = options.max_total_weight;
  return spec;
}

inline std::string describe_objective(const engine::ObjectiveVector& objective) {
  std::string text = "protected=" + std::to_string(objective.protected_inclusions) +
                     " weight=" + std::to_string(objective.total_weight) +
                     " cardinality=" + std::to_string(objective.cardinality) + " signature={";
  for (std::size_t i = 0; i < objective.signature.size(); ++i) {
    if (i != 0) {
      text += ",";
    }
    text += objective.signature[i].value();
  }
  text += "}";
  return text;
}

inline std::string describe_members(const engine::ContainmentProblem& problem,
                                    const std::vector<model::NodeIndex>& members) {
  std::string text = "{";
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i != 0) {
      text += ",";
    }
    text += problem.resource_ids()[members[i]].value();
  }
  text += "}";
  return text;
}

inline std::string describe_solution(const engine::ContainmentProblem& problem,
                                     const engine::CutSolution& solution) {
  std::string text = std::string("status=") + engine::to_string(solution.status);
  if (solution.has_solution()) {
    text += " objective{" + describe_objective(solution.objective) + "}";
    text += " members" + describe_members(problem, solution.members);
  }
  text += " explored=" + std::to_string(solution.counters.explored_nodes) +
          " budget=" + std::to_string(solution.counters.budget) +
          " budget_exhausted=" + (solution.counters.budget_exhausted ? "true" : "false") +
          " heuristic_used=" + (solution.counters.heuristic_used ? "true" : "false");
  return text;
}

inline std::size_t containable_count(const engine::ContainmentProblem& problem) {
  std::size_t count = 0;
  for (model::NodeIndex node = 0; node < problem.node_count(); ++node) {
    if (problem.is_containable(node)) {
      ++count;
    }
  }
  return count;
}

inline std::string describe_instance(const engine::ContainmentProblem& problem) {
  std::string text = "nodes=" + std::to_string(problem.node_count()) +
                     " sources=" + std::to_string(problem.sources().size()) +
                     " protected=" + std::to_string(problem.protected_nodes().size()) +
                     " candidates=" + std::to_string(problem.candidates().size()) +
                     " containable=" + std::to_string(containable_count(problem));
  return text;
}

}  // namespace fcfn::test::solver_cases

#endif  // FCFN_TESTS_SOLVER_CASE_BUILDER_HPP
