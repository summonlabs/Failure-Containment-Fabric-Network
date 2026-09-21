// FCFN solver suite: adversarial graphs built to break greedy local choices.
//
// Every case is hand-built with a known optimum, independently confirmed by the
// exhaustive reference solver. Where the scalable heuristic is strictly worse,
// the difference is printed: the point is that the heuristic is honestly
// labelled as not optimal, never that it matched the exact search.
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

using engine::CutSolution;
using engine::ObjectiveVector;
using engine::SolveBudget;
using engine::SolveStatus;

struct AdversarialCase {
  std::string name;
  CaseGraph graph;
  /// Failure sources, named explicitly so every case states its own premise.
  std::vector<std::string> sources;
  /// Hand-computed optimal containment set.
  std::vector<std::string> expected_members;
};

std::size_t greedy_gaps = 0;

/// One source, one protected obligation, and three routes. The cheap branch
/// nodes a and b are the greedy's choices, yet they lie in no optimal cut: the
/// optimum contains the expensive node on the shortest route plus the shared
/// node y.
AdversarialCase greedy_branch_nodes_are_in_no_optimal_cut() {
  AdversarialCase test_case;
  test_case.name = "greedy_branch_nodes_are_in_no_optimal_cut";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "x", true, 6);
  add_node(test_case.graph, "a", true, 2);
  add_node(test_case.graph, "b", true, 2);
  add_node(test_case.graph, "y", true, 2);
  add_node(test_case.graph, "p0", false, 1, true);
  add_edge(test_case.graph, "s0", "x");
  add_edge(test_case.graph, "x", "p0");
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "s0", "b");
  add_edge(test_case.graph, "a", "y");
  add_edge(test_case.graph, "b", "y");
  add_edge(test_case.graph, "y", "p0");
  test_case.sources = {"s0"};
  test_case.expected_members = {"x", "y"};
  return test_case;
}

/// Shared-risk node: containing the single shared node is cheaper than
/// containing both branches, and the greedy takes both branches.
AdversarialCase shared_risk_node_beats_both_branches() {
  AdversarialCase test_case;
  test_case.name = "shared_risk_node_beats_both_branches";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "a", true, 2);
  add_node(test_case.graph, "b", true, 2);
  add_node(test_case.graph, "h", true, 3);
  add_node(test_case.graph, "p0", false, 1, true);
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "s0", "b");
  add_edge(test_case.graph, "a", "h");
  add_edge(test_case.graph, "b", "h");
  add_edge(test_case.graph, "h", "p0");
  test_case.sources = {"s0"};
  test_case.expected_members = {"h"};
  return test_case;
}

/// Cycle: the back edge a->b->a does not create a bypass, so the cheapest single
/// member on the residual path is still the optimum.
AdversarialCase cycle_back_edge() {
  AdversarialCase test_case;
  test_case.name = "cycle_back_edge";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "a", true, 3);
  add_node(test_case.graph, "b", true, 1);
  add_node(test_case.graph, "p0", false, 1, true);
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "a", "b");
  add_edge(test_case.graph, "b", "a");  // cycle
  add_edge(test_case.graph, "b", "p0");
  test_case.sources = {"s0"};
  test_case.expected_members = {"b"};
  return test_case;
}

/// Two uncontainable failure sources, one obligation, and a shared node that is
/// on a residual path from both sources: the union of the two branch cuts is
/// strictly worse than containing the shared node.
AdversarialCase multiple_sources_shared_risk() {
  AdversarialCase test_case;
  test_case.name = "multiple_sources_shared_risk";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "s1", false, 1);
  add_node(test_case.graph, "a", true, 3);
  add_node(test_case.graph, "b", true, 3);
  add_node(test_case.graph, "h", true, 5);
  add_node(test_case.graph, "p0", false, 1, true);
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "s1", "b");
  add_edge(test_case.graph, "a", "h");
  add_edge(test_case.graph, "b", "h");
  add_edge(test_case.graph, "h", "p0");
  test_case.sources = {"s0", "s1"};
  test_case.expected_members = {"h"};
  return test_case;
}

/// Two protected obligations that are only jointly reachable: every path must be
/// cut, so the optimum is the union of one member per branch plus the shared one.
AdversarialCase multiple_protected_obligations() {
  AdversarialCase test_case;
  test_case.name = "multiple_protected_obligations";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "a", true, 6);
  add_node(test_case.graph, "b", true, 1);
  add_node(test_case.graph, "h", true, 4);
  add_node(test_case.graph, "p0", false, 1, true);
  add_node(test_case.graph, "p1", false, 1, true);
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "s0", "b");
  add_edge(test_case.graph, "s0", "h");
  add_edge(test_case.graph, "a", "p0");
  add_edge(test_case.graph, "b", "p1");
  add_edge(test_case.graph, "h", "p0");
  add_edge(test_case.graph, "h", "p1");
  test_case.sources = {"s0"};
  test_case.expected_members = {"a", "b", "h"};
  return test_case;
}

/// A REFUTED direct edge must not be treated as a bypass: if it were, no
/// eligible containment would exist at all.
AdversarialCase refuted_edge_is_not_a_bypass() {
  AdversarialCase test_case;
  test_case.name = "refuted_edge_is_not_a_bypass";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "a", true, 5);
  add_node(test_case.graph, "p0", false, 1, true);
  add_edge(test_case.graph, "s0", "a");
  add_edge(test_case.graph, "a", "p0");
  add_edge(test_case.graph, "s0", "p0", model::EdgeEvidence::Refuted);
  test_case.sources = {"s0"};
  test_case.expected_members = {"a"};
  return test_case;
}

/// Wide diamond: five branches converge on one hub. The greedy contains every
/// branch; the optimum contains the hub alone.
AdversarialCase wide_diamond_hub() {
  AdversarialCase test_case;
  test_case.name = "wide_diamond_hub";
  add_node(test_case.graph, "s0", false, 1);
  add_node(test_case.graph, "h", true, 5);
  add_node(test_case.graph, "p0", false, 1, true);
  for (int i = 0; i < 5; ++i) {
    const std::string name = "a" + std::to_string(i);
    add_node(test_case.graph, name, true, 3);
    add_edge(test_case.graph, "s0", name);
    add_edge(test_case.graph, name, "h");
  }
  add_edge(test_case.graph, "h", "p0");
  test_case.sources = {"s0"};
  test_case.expected_members = {"h"};
  return test_case;
}

std::vector<model::NodeIndex> resolve(const model::Topology& topology,
                                      const std::vector<std::string>& ids) {
  std::vector<model::NodeIndex> members;
  for (const std::string& id : ids) {
    const auto index = topology.find(model::ResourceId::unchecked(id));
    FCFN_CHECK_CTX(index.has_value(), "expected member is not in the topology: " + id);
    members.push_back(*index);
  }
  std::sort(members.begin(), members.end());
  return members;
}

void run_case(const AdversarialCase& test_case) {
  const model::Topology topology = require_topology(to_spec(test_case.graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = test_case.sources;
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const std::string context = test_case.name + ": " + describe_instance(problem);

  const std::vector<model::NodeIndex> expected = resolve(topology, test_case.expected_members);
  const Result<ObjectiveVector> hand = engine::verify_cut(problem, expected);
  FCFN_CHECK_CTX(hand.ok(), context + " (hand-built optimum is not a valid cut: " +
                                  hand.status().to_string() + ")");

  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && reference.feasible,
                 context + " (reference solver found no feasible containment)");
  FCFN_CHECK_CTX(reference.objective == hand.value(),
                 context + "\n  reference: " + describe_objective(reference.objective) +
                     "\n  hand:      " + describe_objective(hand.value()));

  SolveBudget budget;
  budget.max_explored_nodes = 2000000;
  budget.max_iterations = 200000;
  const CutSolution exact = solve_exact(problem, budget);
  FCFN_CHECK_CTX(exact.status == SolveStatus::ProvenOptimal,
                 context + "\n  " + describe_solution(problem, exact));
  FCFN_CHECK_CTX(exact.objective == hand.value(),
                 context + "\n  exact: " + describe_objective(exact.objective) + "\n  hand:  " +
                     describe_objective(hand.value()));
  FCFN_CHECK_CTX(exact.members == expected,
                 context + "\n  exact members" + describe_members(problem, exact.members) +
                     "\n  hand members" + describe_members(problem, expected));

  const CutSolution heuristic = solve_heuristic(problem, budget);
  long long delta_weight = 0;
  bool worse = false;
  if (heuristic.has_solution()) {
    FCFN_CHECK_CTX(engine::verify_cut(problem, heuristic.members).ok(),
                   context + " (heuristic cut does not verify)");
    FCFN_CHECK_CTX(!engine::objective_less(heuristic.objective, exact.objective,
                                           problem.minimize_protected_inclusions()),
                   context + "\n  heuristic beat the proven optimum: " +
                       describe_objective(heuristic.objective));
    worse = !(heuristic.objective == exact.objective);
    if (worse) {
      delta_weight = static_cast<long long>(heuristic.objective.total_weight) -
                     static_cast<long long>(exact.objective.total_weight);
      ++greedy_gaps;
    }
  }
  std::printf("adversarial %-40s exact[%s] heuristic[%s] gap=%s%lld\n", test_case.name.c_str(),
              describe_objective(exact.objective).c_str(),
              heuristic.has_solution() ? describe_objective(heuristic.objective).c_str()
                                       : "no solution",
              worse ? "+" : "", delta_weight);
}

}  // namespace

FCFN_TEST(solver, adversarial_greedy_traps) {
  greedy_gaps = 0;
  run_case(greedy_branch_nodes_are_in_no_optimal_cut());
  run_case(shared_risk_node_beats_both_branches());
  run_case(cycle_back_edge());
  run_case(multiple_sources_shared_risk());
  run_case(multiple_protected_obligations());
  run_case(refuted_edge_is_not_a_bypass());
  run_case(wide_diamond_hub());
  std::printf("adversarial cases=7 greedy_gaps=%zu\n", greedy_gaps);
  FCFN_CHECK_CTX(greedy_gaps >= 3,
                 "expected the greedy construction to be strictly suboptimal on several hand-built "
                 "cases; observed " +
                     std::to_string(greedy_gaps));
}

}  // namespace fcfn::test::solver_cases
