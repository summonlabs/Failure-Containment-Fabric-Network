// FCFN solver suite: hand-computed known-answer cases.
//
// layered_topology is a complete bipartite cascade: every path from the source
// passes through exactly one node of every intermediate layer, and the source
// and the protected last layer are not containable. A set is therefore a cut if
// and only if it contains one complete intermediate layer, so the optimum is
// known by hand: the cheapest complete intermediate layer, or the source itself
// when the source may be contained.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "case_builder.hpp"
#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/solver.hpp"
#include "fixtures.hpp"
#include "reference_solver.hpp"

namespace fcfn::test::solver_cases {
namespace {

using engine::ObjectiveVector;
using engine::SolveBudget;

/// Layer index encoded in a layered_topology node name ("l<layer>c<column>").
std::size_t layer_of(const std::string& id) {
  const std::size_t end = id.find('c');
  return static_cast<std::size_t>(std::stoul(id.substr(1, end - 1)));
}

struct Layer {
  std::size_t index{0};
  std::vector<model::NodeIndex> members{};
};

std::vector<Layer> layers_of(const model::Topology& topology) {
  std::vector<Layer> layers;
  for (std::size_t i = 0; i < topology.node_count(); ++i) {
    const auto index = static_cast<model::NodeIndex>(i);
    const std::size_t layer = layer_of(topology.resource(index).value());
    if (layers.size() <= layer) {
      layers.resize(layer + 1);
    }
    layers[layer].index = layer;
    layers[layer].members.push_back(index);
  }
  for (Layer& layer : layers) {
    std::sort(layer.members.begin(), layer.members.end());
  }
  return layers;
}

ObjectiveVector objective_for(const engine::ContainmentProblem& problem,
                              const std::vector<model::NodeIndex>& members) {
  const Result<ObjectiveVector> verified = engine::verify_cut(problem, members);
  FCFN_CHECK_CTX(verified.ok(), "hand-built expectation is not a valid cut: " +
                                    verified.status().to_string());
  return verified.value();
}

/// Expected optimum of a layered instance: the lexicographic minimum over the
/// single source (when containable) and every complete intermediate layer.
ObjectiveVector expected_layered_optimum(const engine::ContainmentProblem& problem,
                                         std::size_t layers) {
  const std::vector<Layer> all = layers_of(problem.graph().topology());
  const model::NodeIndex source = problem.sources().front();
  const bool source_mandatory =
      problem.require_source_inclusion() && problem.is_containable(source);
  if (source_mandatory) {
    // The source is required and already sufficient: adding anything increases
    // the objective, so the source alone is the optimum.
    return objective_for(problem, {source});
  }
  std::vector<ObjectiveVector> candidates;
  if (problem.is_containable(source)) {
    candidates.push_back(objective_for(problem, {source}));
  }
  for (const Layer& layer : all) {
    if (layer.index == 0 || layer.index + 1 >= layers) {
      continue;
    }
    candidates.push_back(objective_for(problem, layer.members));
  }
  FCFN_CHECK_CTX(!candidates.empty(), "no hand-computed candidate for the layered instance");
  ObjectiveVector best = candidates.front();
  for (const ObjectiveVector& candidate : candidates) {
    if (engine::objective_less(candidate, best, problem.minimize_protected_inclusions())) {
      best = candidate;
    }
  }
  return best;
}

void check_layered(std::size_t layers, std::size_t width, std::uint64_t case_seed,
                   bool containable_source, bool require_source_inclusion) {
  model::TopologySpec spec = layered_topology(layers, width, case_seed, 11 + case_seed);
  if (containable_source) {
    spec.nodes[0].containable = true;
  }
  const model::Topology topology = require_topology(std::move(spec));
  const std::string context = "layers=" + std::to_string(layers) + " width=" + std::to_string(width) +
                              " case_seed=" + std::to_string(case_seed) +
                              " containable_source=" + (containable_source ? "true" : "false") +
                              " require_source_inclusion=" +
                              (require_source_inclusion ? "true" : "false") +
                              " seed=" + std::to_string(fcfn::test::seed());

  SpecOptions spec_options;
  spec_options.source_count = 1;
  spec_options.require_source_inclusion = require_source_inclusion;
  const engine::ContainmentInstanceSpec instance_spec = make_spec(topology, spec_options);
  Instance instance(topology, instance_spec);
  const engine::ContainmentProblem& problem = instance.problem();
  FCFN_CHECK_CTX(problem.sources().size() == 1, context);
  FCFN_CHECK_CTX(problem.protected_nodes().size() == width, context);

  const ObjectiveVector expected = expected_layered_optimum(problem, layers);
  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && reference.feasible, context + " (reference infeasible)");
  FCFN_CHECK_CTX(reference.objective == expected,
                 context + "\n  reference: " + describe_objective(reference.objective) +
                     "\n  hand:      " + describe_objective(expected));

  SolveBudget budget;
  budget.max_explored_nodes = 2000000;
  const engine::CutSolution solution = solve_exact(problem, budget);
  FCFN_CHECK_CTX(solution.status == engine::SolveStatus::ProvenOptimal,
                 context + "\n  " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.objective == expected,
                 context + "\n  exact: " + describe_objective(solution.objective) +
                     "\n  hand:  " + describe_objective(expected));
  FCFN_CHECK_CTX(solution.members == reference.members, context);
}

}  // namespace

FCFN_TEST(solver, layered_optimum_is_cheapest_complete_layer) {
  for (const std::uint64_t case_seed : {1ull, 7ull, 23ull}) {
    for (const std::size_t width : {1u, 2u, 3u, 4u}) {
      check_layered(4, width, case_seed, false, true);
      check_layered(5, width, case_seed, false, true);
    }
  }
  std::printf("layered_cheapest_layer cases=%zu\n", static_cast<std::size_t>(3 * 4 * 2));
}

FCFN_TEST(solver, layered_optimum_when_source_is_containable) {
  // With require_source_inclusion the (now containable) source is mandatory and
  // already sufficient, so the optimum is the source alone.
  for (const std::uint64_t case_seed : {3ull, 11ull}) {
    for (const std::size_t width : {1u, 2u, 3u}) {
      check_layered(4, width, case_seed, true, true);
    }
  }
  // Without source inclusion the source competes with the cheapest layer.
  for (const std::uint64_t case_seed : {5ull, 13ull}) {
    for (const std::size_t width : {1u, 2u, 4u}) {
      check_layered(5, width, case_seed, true, false);
    }
  }
  std::printf("layered_source_cases=%zu\n", static_cast<std::size_t>(2 * 3 + 2 * 3));
}

FCFN_TEST(solver, layered_uncontainable_source_is_proven_infeasible) {
  // Two layers leave no intermediate layer: the source cannot be contained and
  // nothing else disconnects layer 0 from the protected layer 1.
  model::TopologySpec spec = layered_topology(2, 3, 42, 77);
  const model::Topology topology = require_topology(std::move(spec));
  SpecOptions spec_options;
  spec_options.source_count = 1;
  const engine::ContainmentInstanceSpec instance_spec = make_spec(topology, spec_options);
  Instance instance(topology, instance_spec);
  const engine::ContainmentProblem& problem = instance.problem();
  const std::string context = describe_instance(problem);
  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && !reference.feasible, context + " (reference feasible)");
  SolveBudget budget;
  const engine::CutSolution solution = solve_exact(problem, budget);
  FCFN_CHECK_CTX(solution.status == engine::SolveStatus::ProvenInfeasible,
                 context + "\n  " + describe_solution(problem, solution));
}

}  // namespace fcfn::test::solver_cases
