// FCFN solver suite: differential correctness against the independent solver.
//
// Every instance built here is small enough for the exhaustive reference solver
// in tests/support/reference_solver.hpp, which is written from the MCC-1
// definition rather than from the production search. Agreement on feasibility,
// objective vector, and member set is therefore evidence, not a tautology.
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
using engine::SolveBudget;
using engine::SolveStatus;

/// Generous exact budget: the differential suites require the search to finish,
/// so LimitReachedNoSolution is a finding rather than an accepted outcome.
constexpr std::uint64_t kDifferentialBudget = 4000000;

std::string exact_text(const engine::ContainmentProblem& problem, const CutSolution& solution) {
  return "exact: " + describe_solution(problem, solution);
}

/// Compare solve_exact with the reference solver on one instance. Returns the
/// reference verdict so callers can account for population coverage.
bool compare_exact_to_reference(const engine::ContainmentProblem& problem, const std::string& context) {
  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported, context + " (reference solver eligible-node bound exceeded)");

  SolveBudget budget;
  budget.max_explored_nodes = kDifferentialBudget;
  const CutSolution solution = solve_exact(problem, budget);

  if (reference.feasible) {
    FCFN_CHECK_CTX(solution.has_solution(),
                   context + "\n  reference: feasible " + describe_objective(reference.objective) +
                       " members" + describe_members(problem, reference.members) + "\n  " +
                       exact_text(problem, solution));
    FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenOptimal,
                   context + "\n  reference: feasible " + describe_objective(reference.objective) +
                       "\n  " + exact_text(problem, solution));
    FCFN_CHECK_CTX(!solution.counters.budget_exhausted,
                   context + "\n  " + exact_text(problem, solution));
    FCFN_CHECK_CTX(solution.objective == reference.objective,
                   context + "\n  reference: " + describe_objective(reference.objective) +
                       "\n  " + exact_text(problem, solution));
    FCFN_CHECK_CTX(solution.members == reference.members,
                   context + "\n  reference members" + describe_members(problem, reference.members) +
                       "\n  " + exact_text(problem, solution));
  } else {
    FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenInfeasible,
                   context + "\n  reference: infeasible\n  " + exact_text(problem, solution));
  }
  return reference.feasible;
}

std::string instance_context(std::size_t iteration, std::uint64_t case_seed,
                             const engine::ContainmentProblem& problem) {
  return "iteration=" + std::to_string(iteration) + " case_seed=" + std::to_string(case_seed) +
         " " + describe_instance(problem) + " seed=" + std::to_string(fcfn::test::seed());
}

}  // namespace

FCFN_TEST(solver, differential_thousands_of_small_instances) {
  const std::uint64_t base = fcfn::test::seed();
  Rng rng(base ^ 0x9d1ff3a1ull);
  std::size_t iterations = 2400;
  std::size_t feasible = 0;
  std::size_t infeasible = 0;
  std::size_t eligible_total = 0;

  for (std::size_t i = 0; i < iterations; ++i) {
    SyntheticGraphOptions options;
    options.nodes = 4 + static_cast<std::size_t>(rng.below(7));  // 4..10 nodes
    options.edges = options.nodes + static_cast<std::size_t>(rng.below(2 * options.nodes + 1));
    options.containable_percent = rng.chance(70) ? 100u : static_cast<std::uint32_t>(50 + rng.below(51));
    options.unknown_percent = static_cast<std::uint32_t>(rng.below(101));
    options.max_weight = 1 + rng.below(5);
    options.protected_count = 1 + static_cast<std::size_t>(rng.below(2));
    options.seed = base + i * 7919ull;
    options.topology_generation = 1 + i;

    model::TopologySpec topology_spec = synthetic_topology(options);
    if (rng.chance(25) && topology_spec.nodes.size() > 1) {
      add_back_edge_to_source(topology_spec, 1 + static_cast<std::size_t>(rng.below(topology_spec.nodes.size() - 1)));
    }
    model::Topology topology = require_topology(std::move(topology_spec));
    SpecOptions spec_options;
    spec_options.source_count = 1 + static_cast<std::size_t>(rng.below(2));
    spec_options.require_source_inclusion = !rng.chance(20);
    spec_options.allow_protected_inclusion = rng.chance(20);
    spec_options.minimize_protected_inclusions = !rng.chance(15);
    if (rng.chance(20)) {
      spec_options.max_members = 1 + static_cast<std::size_t>(rng.below(3));
    }
    if (rng.chance(15)) {
      spec_options.max_total_weight = 1 + rng.below(12);
    }
    if (rng.chance(30)) {
      const std::vector<std::string> candidates = unprotected_ids(topology);
      if (!candidates.empty()) {
        spec_options.forced_ineligible.push_back(candidates[rng.below(candidates.size())]);
      }
    }

    const engine::ContainmentInstanceSpec spec = make_spec(topology, spec_options);
    Instance instance(std::move(topology), spec);
    const engine::ContainmentProblem& problem = instance.problem();
    eligible_total += containable_count(problem);
    const std::string context = instance_context(i, options.seed, problem);
    if (compare_exact_to_reference(problem, context)) {
      ++feasible;
    } else {
      ++infeasible;
    }
  }

  std::printf("differential_small instances=%zu eligible_total=%zu feasible=%zu infeasible=%zu\n",
              iterations, eligible_total, feasible, infeasible);
  FCFN_CHECK_CTX(iterations >= 2000, "the suite must exercise thousands of instances");
  FCFN_CHECK_CTX(feasible > iterations / 4, "expected a substantial feasible population: " +
                                                std::to_string(feasible) + "/" + std::to_string(iterations));
  FCFN_CHECK_CTX(infeasible > iterations / 20,
                 "expected an infeasible population: " + std::to_string(infeasible) + "/" +
                     std::to_string(iterations));
}

FCFN_TEST(solver, differential_larger_instances) {
  const std::uint64_t base = fcfn::test::seed();
  Rng rng(base ^ 0x2f7c1d5bull);
  const std::size_t iterations = 14;
  for (std::size_t i = 0; i < iterations; ++i) {
    SyntheticGraphOptions options;
    options.nodes = 12 + static_cast<std::size_t>(rng.below(6));  // 12..17 nodes
    options.edges = options.nodes + 4 + static_cast<std::size_t>(rng.below(3 * options.nodes));
    options.containable_percent = 100;
    options.unknown_percent = static_cast<std::uint32_t>(rng.below(101));
    options.max_weight = 1 + rng.below(6);
    options.protected_count = 1;
    options.seed = base + i * 104729ull;
    options.topology_generation = 100 + i;

    model::TopologySpec topology_spec = synthetic_topology(options);
    if (rng.chance(30) && topology_spec.nodes.size() > 1) {
      add_back_edge_to_source(topology_spec, 1 + static_cast<std::size_t>(rng.below(topology_spec.nodes.size() - 1)));
    }
    model::Topology topology = require_topology(std::move(topology_spec));
    SpecOptions spec_options;
    spec_options.source_count = 1 + static_cast<std::size_t>(rng.below(2));
    spec_options.require_source_inclusion = !rng.chance(15);
    const engine::ContainmentInstanceSpec spec = make_spec(topology, spec_options);
    Instance instance(std::move(topology), spec);
    const engine::ContainmentProblem& problem = instance.problem();
    FCFN_CHECK_CTX(containable_count(problem) <= kReferenceSolverEligibleLimit,
                   "reference bound: " + describe_instance(problem));
    compare_exact_to_reference(problem, instance_context(i, options.seed, problem));
  }
  std::printf("differential_larger instances=%zu (up to %zu eligible nodes)\n", iterations,
              kReferenceSolverEligibleLimit);
}

FCFN_TEST(solver, differential_shuffled_insertion_order) {
  const std::uint64_t base = fcfn::test::seed();
  Rng rng(base ^ 0x71a5c3e9ull);
  const std::size_t iterations = 80;
  for (std::size_t i = 0; i < iterations; ++i) {
    CaseGraph graph;
    graph.generation = 7 + i;
    const std::size_t node_count = 6 + static_cast<std::size_t>(rng.below(7));
    for (std::size_t n = 0; n < node_count; ++n) {
      const bool is_protected = n + 1 == node_count;
      add_node(graph, "n" + std::to_string(n), !is_protected, 1 + rng.below(5), is_protected);
    }
    const std::size_t edge_count = node_count + static_cast<std::size_t>(rng.below(2 * node_count));
    for (std::size_t e = 0; e < edge_count; ++e) {
      const std::size_t from = rng.below(node_count);
      const std::size_t to = rng.below(node_count);
      if (from == to) {
        continue;
      }
      const std::uint64_t roll = rng.below(100);
      const model::EdgeEvidence evidence = roll < 60   ? model::EdgeEvidence::Proven
                                           : roll < 85 ? model::EdgeEvidence::Unknown
                                                       : model::EdgeEvidence::Refuted;
      bool duplicate = false;
      for (const CaseEdge& existing : graph.edges) {
        if (existing.from == "n" + std::to_string(from) && existing.to == "n" + std::to_string(to)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        add_edge(graph, "n" + std::to_string(from), "n" + std::to_string(to), evidence);
      }
    }

    CaseGraph shuffled = graph;
    for (std::size_t n = shuffled.nodes.size(); n > 1; --n) {
      const std::size_t other = static_cast<std::size_t>(rng.below(n));
      std::swap(shuffled.nodes[n - 1], shuffled.nodes[other]);
    }
    for (std::size_t e = shuffled.edges.size(); e > 1; --e) {
      const std::size_t other = static_cast<std::size_t>(rng.below(e));
      std::swap(shuffled.edges[e - 1], shuffled.edges[other]);
    }

    SpecOptions spec_options;
    spec_options.source_count = 1;
    spec_options.require_source_inclusion = !rng.chance(25);
    spec_options.allow_protected_inclusion = rng.chance(15);

    model::Topology canonical = require_topology(to_spec(graph));
    model::Topology permuted = require_topology(to_spec(shuffled));
    const auto spec = make_spec(canonical, spec_options);
    Instance first(std::move(canonical), spec);
    Instance second(std::move(permuted), spec);
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                describe_instance(first.problem());
    FCFN_CHECK_CTX(first.problem().instance_digest() == second.problem().instance_digest(),
                   context + " (instance digest depends on insertion order)");
    FCFN_CHECK_CTX(first.problem().node_count() == second.problem().node_count(), context);

    SolveBudget budget;
    budget.max_explored_nodes = kDifferentialBudget;
    const CutSolution a = solve_exact(first.problem(), budget);
    const CutSolution b = solve_exact(second.problem(), budget);
    FCFN_CHECK_CTX(a.status == b.status, context + "\n  first: " + describe_solution(first.problem(), a) +
                                              "\n  second: " + describe_solution(second.problem(), b));
    FCFN_CHECK_CTX(a.members == b.members,
                   context + "\n  first members" + describe_members(first.problem(), a.members) +
                       "\n  second members" + describe_members(second.problem(), b.members));
    FCFN_CHECK_CTX(a.objective == b.objective,
                   context + "\n  first: " + describe_objective(a.objective) +
                       "\n  second: " + describe_objective(b.objective));
    FCFN_CHECK_CTX(a.counters.explored_nodes == b.counters.explored_nodes,
                   context + " explored=" + std::to_string(a.counters.explored_nodes) + " vs " +
                       std::to_string(b.counters.explored_nodes));
  }
}

FCFN_TEST(solver, differential_repeated_runs_are_identical) {
  const std::uint64_t base = fcfn::test::seed();
  Rng rng(base ^ 0x4b8f2c11ull);
  const std::size_t iterations = 60;
  for (std::size_t i = 0; i < iterations; ++i) {
    SyntheticGraphOptions options;
    options.nodes = 6 + static_cast<std::size_t>(rng.below(6));
    options.edges = options.nodes + 3 + static_cast<std::size_t>(rng.below(2 * options.nodes));
    options.containable_percent = 100;
    options.unknown_percent = static_cast<std::uint32_t>(rng.below(101));
    options.max_weight = 1 + rng.below(4);
    options.protected_count = 1;
    options.seed = base + i * 15485863ull;
    options.topology_generation = 500 + i;

    model::TopologySpec topology_spec = synthetic_topology(options);
    if (rng.chance(50) && topology_spec.nodes.size() > 1) {
      add_back_edge_to_source(topology_spec, 1 + static_cast<std::size_t>(rng.below(topology_spec.nodes.size() - 1)));
    }
    model::Topology topology = require_topology(std::move(topology_spec));
    SpecOptions spec_options;
    spec_options.source_count = 1 + static_cast<std::size_t>(rng.below(2));
    const engine::ContainmentInstanceSpec spec = make_spec(topology, spec_options);
    Instance instance(std::move(topology), spec);
    const engine::ContainmentProblem& problem = instance.problem();
    const std::string context = instance_context(i, options.seed, problem);

    SolveBudget budget;
    budget.max_explored_nodes = kDifferentialBudget;
    const CutSolution first = solve_exact(problem, budget);
    const CutSolution second = solve_exact(problem, budget);
    const CutSolution third = solve_exact(problem, budget);
    FCFN_CHECK_CTX(first.status == second.status && second.status == third.status,
                   context + " (exact status is not deterministic)");
    FCFN_CHECK_CTX(first.objective == second.objective && second.objective == third.objective,
                   context + " (exact objective is not deterministic)");
    FCFN_CHECK_CTX(first.members == second.members && second.members == third.members,
                   context + " (exact member set is not deterministic)");
    FCFN_CHECK_CTX(first.counters.explored_nodes == second.counters.explored_nodes &&
                       second.counters.explored_nodes == third.counters.explored_nodes,
                   context + " (exact explored-node count is not deterministic)");

    SolveBudget heuristic_budget;
    heuristic_budget.max_iterations = 100000;
    const CutSolution h1 = solve_heuristic(problem, heuristic_budget);
    const CutSolution h2 = solve_heuristic(problem, heuristic_budget);
    FCFN_CHECK_CTX(h1.status == h2.status, context + " (heuristic status is not deterministic)");
    FCFN_CHECK_CTX(h1.members == h2.members, context + " (heuristic member set is not deterministic)");
    FCFN_CHECK_CTX(h1.objective == h2.objective,
                   context + " (heuristic objective is not deterministic)");
  }
}

}  // namespace fcfn::test::solver_cases
