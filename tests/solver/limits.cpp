// FCFN solver suite: search-limit honesty.
//
// A bounded search may not claim minimality, and a search that ran out of
// budget may not claim infeasibility: "no solution found yet" and "no solution
// exists" are different answers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
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
using engine::SolveBudget;
using engine::SolveStatus;

/// s(no) -> a1 -> a2 -> p(protected): two eligible interior nodes, so the exact
/// search must branch and a budget of one explored node cannot finish.
CaseGraph budget_chain() {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);
  add_node(graph, "a1", true, 1);
  add_node(graph, "a2", true, 1);
  add_node(graph, "p0", false, 1, true);
  add_edge(graph, "s0", "a1");
  add_edge(graph, "a1", "a2");
  add_edge(graph, "a2", "p0");
  return graph;
}

}  // namespace

FCFN_TEST(solver, tiny_budget_is_reported_as_bounded) {
  const CaseGraph graph = budget_chain();
  const model::Topology topology = require_topology(to_spec(graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && reference.feasible,
                 "the fixture must be feasible: " + describe_objective(reference.objective));

  SolveBudget budget;
  budget.max_explored_nodes = 1;
  const CutSolution tiny = solve_exact(problem, budget);
  FCFN_CHECK_CTX(tiny.status == SolveStatus::LimitReachedNoSolution ||
                     tiny.status == SolveStatus::FeasibleNotProvenOptimal,
                 "tiny budget: " + describe_solution(problem, tiny));
  FCFN_CHECK_CTX(tiny.counters.budget_exhausted,
                 "tiny budget did not report exhaustion: " + describe_solution(problem, tiny));
  FCFN_CHECK_CTX(tiny.status != SolveStatus::ProvenOptimal,
                 "tiny budget claimed optimality: " + describe_solution(problem, tiny));
  FCFN_CHECK_CTX(tiny.status != SolveStatus::ProvenInfeasible,
                 "tiny budget claimed infeasibility: " + describe_solution(problem, tiny));
  FCFN_CHECK_CTX(tiny.counters.budget == 1, "counters.budget must echo the configured budget");

  // The same instance is solved completely once the budget is large enough.
  SolveBudget ample;
  ample.max_explored_nodes = 1000;
  const CutSolution full = solve_exact(problem, ample);
  FCFN_CHECK_CTX(full.status == SolveStatus::ProvenOptimal,
                 "ample budget: " + describe_solution(problem, full));
  FCFN_CHECK_CTX(!full.counters.budget_exhausted, "ample budget reported exhaustion");
  FCFN_CHECK_CTX(full.objective == reference.objective,
                 "ample budget: " + describe_objective(full.objective) + " vs reference " +
                     describe_objective(reference.objective));
  FCFN_CHECK_CTX(full.counters.explored_nodes <= ample.max_explored_nodes,
                 "explored node count exceeded the budget");
}

FCFN_TEST(solver, bounded_search_never_claims_infeasibility) {
  // Sweep budgets upward on a feasible fixture: every bounded outcome must be
  // explicitly bounded and none may be reported as infeasible.
  const CaseGraph graph = budget_chain();
  const model::Topology topology = require_topology(to_spec(graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  std::size_t bounded = 0;
  for (std::uint64_t cap = 1; cap <= 4; ++cap) {
    SolveBudget budget;
    budget.max_explored_nodes = cap;
    const CutSolution solution = solve_exact(problem, budget);
    const std::string context = "budget=" + std::to_string(cap) + " " +
                                describe_solution(problem, solution);
    FCFN_CHECK_CTX(solution.status != SolveStatus::ProvenInfeasible, context);
    FCFN_CHECK_CTX(solution.status != SolveStatus::InvalidProblem, context);
    if (solution.status == SolveStatus::LimitReachedNoSolution ||
        solution.status == SolveStatus::FeasibleNotProvenOptimal) {
      FCFN_CHECK_CTX(solution.counters.budget_exhausted, context);
      ++bounded;
    } else {
      FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenOptimal, context);
      FCFN_CHECK_CTX(!solution.counters.budget_exhausted, context);
    }
  }
  FCFN_CHECK_CTX(bounded > 0, "at least one tiny budget must be reported as bounded");
}

FCFN_TEST(solver, dispatch_above_the_node_limit_never_claims_optimality) {
  // layered_topology(5, 3) has 9 eligible interior nodes; a node limit of four
  // forces the dispatcher onto the scalable solver.
  const model::Topology topology = require_topology(layered_topology(5, 3, 99, 31));
  SpecOptions spec_options;
  spec_options.source_count = 1;
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  FCFN_CHECK_CTX(problem.candidates().size() > 4,
                 "expected more than four candidates: " + describe_instance(problem));

  SolveBudget budget;
  budget.max_explored_nodes = 200000;
  budget.max_iterations = 200000;

  const CutSolution heuristic = solve(problem, budget, 4);
  const std::string context = describe_instance(problem) + "\n  " +
                              describe_solution(problem, heuristic);
  FCFN_CHECK_CTX(heuristic.counters.heuristic_used, "dispatch did not use the heuristic: " + context);
  FCFN_CHECK_CTX(heuristic.status != SolveStatus::ProvenOptimal,
                 "heuristic claimed optimality: " + context);
  FCFN_CHECK_CTX(heuristic.status == SolveStatus::FeasibleNotProvenOptimal ||
                     heuristic.status == SolveStatus::LimitReachedNoSolution,
                 "unexpected heuristic status: " + context);
  if (heuristic.has_solution()) {
    FCFN_CHECK_CTX(engine::verify_cut(problem, heuristic.members).ok(),
                   "heuristic cut does not verify: " + context);
  }

  const CutSolution exact = solve(problem, budget, 4096);
  FCFN_CHECK_CTX(!exact.counters.heuristic_used, "dispatch did not use the exact solver");
  FCFN_CHECK_CTX(exact.status == SolveStatus::ProvenOptimal,
                 "exact dispatch: " + describe_solution(problem, exact));
  if (heuristic.has_solution()) {
    FCFN_CHECK_CTX(!engine::objective_less(heuristic.objective, exact.objective,
                                           problem.minimize_protected_inclusions()),
                   "heuristic beat the proven optimum: " + describe_objective(heuristic.objective) +
                       " vs " + describe_objective(exact.objective));
  }
  std::printf("dispatch exact=%s heuristic=%s\n", describe_objective(exact.objective).c_str(),
              heuristic.has_solution() ? describe_objective(heuristic.objective).c_str() : "no solution");
}

FCFN_TEST(solver, heuristic_iteration_budget_is_honest) {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);
  for (int i = 0; i < 6; ++i) {
    add_node(graph, "a" + std::to_string(i), true, 1);
    add_node(graph, "p" + std::to_string(i), false, 1, true);
    add_edge(graph, "s0", "a" + std::to_string(i));
    add_edge(graph, "a" + std::to_string(i), "p" + std::to_string(i));
  }
  const model::Topology topology = require_topology(to_spec(graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();

  SolveBudget starved;
  starved.max_iterations = 1;
  const CutSolution bounded = solve_heuristic(problem, starved);
  FCFN_CHECK_CTX(bounded.status == SolveStatus::LimitReachedNoSolution,
                 "starved heuristic: " + describe_solution(problem, bounded));
  FCFN_CHECK_CTX(bounded.counters.budget_exhausted, "starved heuristic did not report exhaustion");
  FCFN_CHECK_CTX(!bounded.has_solution(), "starved heuristic returned a partial cut as a solution");
  FCFN_CHECK_CTX(bounded.counters.budget == 1, "counters.budget must echo the iteration budget");

  SolveBudget ample;
  ample.max_iterations = 1000;
  const CutSolution solution = solve_heuristic(problem, ample);
  FCFN_CHECK_CTX(solution.status == SolveStatus::FeasibleNotProvenOptimal,
                 "ample heuristic: " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(engine::verify_cut(problem, solution.members).ok(), "heuristic cut does not verify");
  FCFN_CHECK_CTX(solution.objective.cardinality == 6,
                 "expected one member per disjoint branch: " + describe_objective(solution.objective));
}

}  // namespace fcfn::test::solver_cases
