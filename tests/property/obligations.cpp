// FCFN property suite: adding a protected obligation.
//
// Declaring a new protected obligation can only remove freedom: the node stops
// being eligible and a new obligation must be kept reachable-free. The optimum
// therefore cannot improve.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "property_support.hpp"
#include "reference_solver.hpp"

namespace fcfn::test::property_cases {
namespace {

/// Deterministic small base graph: "n0" is the failure source, the highest index
/// is the existing protected obligation.
CaseGraph base_graph(Rng& rng, std::size_t node_count) {
  CaseGraph graph;
  graph.generation = 1 + rng.below(1000);
  for (std::size_t i = 0; i < node_count; ++i) {
    const bool is_protected = i + 1 == node_count;
    // The failure source cannot be contained: containment must be found
    // downstream, so an optimum can contain interior members.
    const bool containable = !is_protected && i != 0;
    solver_cases::add_node(graph, "n" + std::to_string(i), containable, 1 + rng.below(4),
                           is_protected);
  }
  // A spine from the source to the obligation guarantees that the failure can
  // actually reach it, so the instance is not vacuous.
  for (std::size_t i = 0; i + 1 < node_count; ++i) {
    solver_cases::add_edge(graph, "n" + std::to_string(i), "n" + std::to_string(i + 1));
  }
  const std::size_t edge_count = node_count + static_cast<std::size_t>(rng.below(node_count));
  for (std::size_t e = 0; e < edge_count; ++e) {
    const std::size_t from = static_cast<std::size_t>(rng.below(node_count));
    const std::size_t to = static_cast<std::size_t>(rng.below(node_count));
    if (from == to) {
      continue;
    }
    if (from == 0 && to + 1 == node_count) {
      continue;  // a direct source-to-obligation edge leaves nothing to contain
    }
    const std::string from_id = "n" + std::to_string(from);
    const std::string to_id = "n" + std::to_string(to);
    bool duplicate = false;
    for (const solver_cases::CaseEdge& edge : graph.edges) {
      if (edge.from == from_id && edge.to == to_id) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    solver_cases::add_edge(graph, from_id, to_id,
                           rng.chance(30) ? model::EdgeEvidence::Unknown : model::EdgeEvidence::Proven);
  }
  return graph;
}

}  // namespace

FCFN_TEST(property, adding_a_protected_obligation_cannot_improve_the_optimum) {
  Rng rng(fcfn::test::seed() ^ 0x41f7b2d9ull);
  std::size_t iterations = 600;
  std::size_t promotions = 0;
  std::size_t feasible_extensions = 0;
  std::size_t strictly_worse = 0;
  std::size_t infeasible_extensions = 0;
  std::size_t skipped_no_member = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const std::size_t node_count = 5 + static_cast<std::size_t>(rng.below(5));
    const CaseGraph base = base_graph(rng, node_count);
    SpecOptions options;
    options.source_count = 0;
    options.extra_sources = {"n0"};
    const model::Topology base_topology = require_topology(solver_cases::to_spec(base));
    Instance probe(base_topology, solver_cases::make_spec(base_topology, options));

    // Promote a member of the base optimum that is not a failure source, so the
    // extension is guaranteed to remove a containment that was optimal before.
    const CutSolution probe_solution = engine::solve_exact(probe.problem(), SolveBudget{});
    std::string promoted_id;
    if (probe_solution.has_solution()) {
      for (const model::NodeIndex member : probe_solution.members) {
        if (member == 0 || probe.problem().is_source(member)) {
          continue;
        }
        promoted_id = probe.problem().resource_ids()[member].value();
        break;
      }
    }
    if (promoted_id.empty()) {
      ++skipped_no_member;
      continue;
    }
    CaseGraph extended = base;
    bool promoted = false;
    for (solver_cases::CaseNode& node : extended.nodes) {
      if (node.id == promoted_id) {
        node.protected_obligation = true;
        node.containable = false;
        promoted = true;
      }
    }
    FCFN_CHECK_CTX(promoted, "the promoted node is not in the extended graph: " + promoted_id);

    const model::Topology extended_topology = require_topology(solver_cases::to_spec(extended));
    Instance base_instance(base_topology, solver_cases::make_spec(base_topology, options));
    Instance extended_instance(extended_topology,
                               solver_cases::make_spec(extended_topology, options));
    const ContainmentProblem& base_problem = base_instance.problem();
    const ContainmentProblem& extended_problem = extended_instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " promoted=" + promoted_id +
                                " nodes=" + std::to_string(node_count);
    FCFN_CHECK_CTX(extended_problem.protected_nodes().size() ==
                       base_problem.protected_nodes().size() + 1,
                   context + " (the new obligation is missing from the extended instance)");
    ++promotions;

    const std::vector<std::uint64_t> base_feasible = enumerate_feasible(base_problem);
    const std::vector<std::uint64_t> extended_feasible = enumerate_feasible(extended_problem);
    FCFN_CHECK_CTX(is_subset(extended_feasible, base_feasible),
                   context + " (the extended instance admits containment sets the base does not)");

    const ReferenceResult base_reference = reference_solve(base_problem);
    const ReferenceResult extended_reference = reference_solve(extended_problem);
    FCFN_CHECK_CTX(base_reference.supported && extended_reference.supported, context);
    if (!extended_reference.feasible) {
      // The extended instance lost feasibility entirely, which is also a
      // monotone move: nothing it now admits was admissible before.
      ++infeasible_extensions;
      continue;
    }
    ++feasible_extensions;
    FCFN_CHECK_CTX(base_reference.feasible,
                   context + " (the extended instance is feasible but the base is not)");
    FCFN_CHECK_CTX(!engine::objective_less(extended_reference.objective, base_reference.objective,
                                           base_problem.minimize_protected_inclusions()),
                   context + "\n  base:     " + solver_cases::describe_objective(base_reference.objective) +
                       "\n  extended: " + solver_cases::describe_objective(extended_reference.objective));
    // The promoted node was part of the base optimum and is no longer
    // containable, so the extended optimum must be strictly worse.
    FCFN_CHECK_CTX(engine::objective_less(base_reference.objective, extended_reference.objective,
                                          base_problem.minimize_protected_inclusions()),
                   context + "\n  base:     " + solver_cases::describe_objective(base_reference.objective) +
                       "\n  extended: " + solver_cases::describe_objective(extended_reference.objective));
    ++strictly_worse;

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    const CutSolution base_solution = engine::solve_exact(base_problem, budget);
    const CutSolution extended_solution = engine::solve_exact(extended_problem, budget);
    FCFN_CHECK_CTX(base_solution.objective == base_reference.objective, context);
    FCFN_CHECK_CTX(extended_solution.objective == extended_reference.objective, context);
  }
  std::printf("obligation_monotonicity iterations=%zu promotions=%zu feasible_extensions=%zu "
              "infeasible_extensions=%zu strictly_worse=%zu\n",
              iterations, promotions, feasible_extensions, infeasible_extensions, strictly_worse);
  FCFN_CHECK_CTX(promotions > iterations / 2,
                 "expected most instances to be extended: " + std::to_string(promotions) +
                     " (skipped " + std::to_string(skipped_no_member) + ")");
  FCFN_CHECK_CTX(feasible_extensions > 20,
                 "expected a population of feasible extensions: " +
                     std::to_string(feasible_extensions));
  FCFN_CHECK_CTX(strictly_worse == feasible_extensions,
                 "every extension must strictly worsen the optimum");
}

}  // namespace fcfn::test::property_cases
