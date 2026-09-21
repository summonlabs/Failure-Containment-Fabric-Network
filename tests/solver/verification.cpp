// FCFN solver suite: every returned solution passes the independent verifier.
//
// verify_cut re-checks all five MCC-1 hard constraints (eligibility, mandatory
// sources, protected policy, bounds, disconnection) and cut_is_valid re-checks
// disconnection; both are used here as independent arbiters of the solvers.
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

namespace fcfn::test::solver_cases {
namespace {

using engine::CutSolution;
using engine::SolveBudget;
using engine::SolveStatus;

struct Verdict {
  std::size_t feasible{0};
  std::size_t infeasible{0};
  std::size_t limited{0};
};

/// Check one solution against the public verifiers.
void check_solution_is_verified(const engine::ContainmentProblem& problem, const CutSolution& solution,
                                const std::string& context) {
  if (!solution.has_solution()) {
    return;
  }
  const Result<engine::ObjectiveVector> verified = engine::verify_cut(problem, solution.members);
  FCFN_CHECK_CTX(verified.ok(), context + "\n  verify_cut refused the returned cut: " +
                                     verified.status().to_string() + "\n  " +
                                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(engine::cut_is_valid(problem, solution.members),
                 context + "\n  cut_is_valid refused the returned cut\n  " +
                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(engine::find_witness_path(problem, solution.members).empty(),
                 context + "\n  a residual source-to-obligation path survived the cut\n  " +
                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.objective == verified.value(),
                 context + "\n  reported objective " + describe_objective(solution.objective) +
                     " differs from verified " + describe_objective(verified.value()));
  const Result<engine::ObjectiveVector> recomputed = engine::objective_of(problem, solution.members);
  FCFN_CHECK_CTX(recomputed.ok() && recomputed.value() == solution.objective,
                 context + "\n  objective_of disagrees with the reported objective");
  std::vector<model::NodeIndex> sorted = solution.members;
  std::sort(sorted.begin(), sorted.end());
  FCFN_CHECK_CTX(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
                 context + "\n  returned members contain a duplicate\n  " +
                     describe_solution(problem, solution));
  for (const model::NodeIndex member : solution.members) {
    FCFN_CHECK_CTX(problem.is_containable(member),
                   context + "\n  returned member is not eligible for containment: " +
                       problem.resource_ids()[member].value());
  }
}

Verdict sweep(const std::uint64_t tag, const std::size_t iterations, bool use_heuristic) {
  const std::uint64_t base = fcfn::test::seed();
  Rng rng(base ^ tag);
  Verdict verdict;
  for (std::size_t i = 0; i < iterations; ++i) {
    SyntheticGraphOptions options;
    options.nodes = 5 + static_cast<std::size_t>(rng.below(8));
    options.edges = options.nodes + static_cast<std::size_t>(rng.below(2 * options.nodes + 1));
    options.containable_percent = rng.chance(70) ? 100u : static_cast<std::uint32_t>(50 + rng.below(51));
    options.unknown_percent = static_cast<std::uint32_t>(rng.below(101));
    options.max_weight = 1 + rng.below(4);
    options.protected_count = 1 + static_cast<std::size_t>(rng.below(2));
    options.seed = base + i * 2654435761ull;
    options.topology_generation = 900 + i;

    model::TopologySpec topology_spec = synthetic_topology(options);
    if (rng.chance(35) && topology_spec.nodes.size() > 1) {
      add_back_edge_to_source(topology_spec, 1 + static_cast<std::size_t>(rng.below(topology_spec.nodes.size() - 1)));
    }
    model::Topology topology = require_topology(std::move(topology_spec));
    SpecOptions spec_options;
    spec_options.source_count = 1 + static_cast<std::size_t>(rng.below(2));
    spec_options.require_source_inclusion = !rng.chance(20);
    spec_options.allow_protected_inclusion = rng.chance(20);
    if (rng.chance(20)) {
      spec_options.max_members = 1 + static_cast<std::size_t>(rng.below(4));
    }
    const engine::ContainmentInstanceSpec spec = make_spec(topology, spec_options);
    Instance instance(std::move(topology), spec);
    const engine::ContainmentProblem& problem = instance.problem();
    const std::string context =
        "iteration=" + std::to_string(i) + " case_seed=" + std::to_string(options.seed) + " " +
        describe_instance(problem) + " seed=" + std::to_string(fcfn::test::seed());

    SolveBudget budget;
    budget.max_explored_nodes = 4000000;
    budget.max_iterations = 200000;
    const CutSolution solution =
        use_heuristic ? solve_heuristic(problem, budget) : solve_exact(problem, budget);
    check_solution_is_verified(problem, solution, context);

    if (use_heuristic) {
      FCFN_CHECK_CTX(solution.status != SolveStatus::ProvenOptimal,
                     context + "\n  the scalable solver claimed optimality\n  " +
                         describe_solution(problem, solution));
      FCFN_CHECK_CTX(solution.counters.heuristic_used,
                     context + "\n  heuristic counter was not set\n  " +
                         describe_solution(problem, solution));
    } else {
      FCFN_CHECK_CTX(solution.status != SolveStatus::FeasibleNotProvenOptimal,
                     context + "\n  exact search ended bounded with a 4M node budget\n  " +
                         describe_solution(problem, solution));
    }

    if (solution.has_solution()) {
      ++verdict.feasible;
    } else if (solution.status == SolveStatus::ProvenInfeasible) {
      ++verdict.infeasible;
    } else {
      ++verdict.limited;
      if (!use_heuristic) {
        FCFN_CHECK_CTX(false, context + "\n  unexpected bounded outcome from the exact solver\n  " +
                                  describe_solution(problem, solution));
      }
    }

    if (solution.status == SolveStatus::ProvenInfeasible && solution.has_infeasibility_witness) {
      FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, solution.infeasibility_path),
                     context + "\n  the reported infeasibility certificate does not verify\n  " +
                         describe_solution(problem, solution));
    }
  }
  return verdict;
}

}  // namespace

FCFN_TEST(solver, exact_solutions_pass_engine_verifier) {
  const Verdict verdict = sweep(0x1c3d5f7bull, 500, false);
  std::printf("exact_solutions_verified feasible=%zu infeasible=%zu limited=%zu\n", verdict.feasible,
              verdict.infeasible, verdict.limited);
  FCFN_CHECK_CTX(verdict.feasible > 100, "expected a large feasible population");
}

FCFN_TEST(solver, heuristic_solutions_pass_engine_verifier) {
  const Verdict verdict = sweep(0x2d4e6a8cull, 500, true);
  std::printf("heuristic_solutions_verified feasible=%zu infeasible=%zu limited=%zu\n",
              verdict.feasible, verdict.infeasible, verdict.limited);
  FCFN_CHECK_CTX(verdict.feasible > 100, "expected a large feasible population");
}

FCFN_TEST(solver, infeasibility_certificates_from_search_verify) {
  // Every node on the only source-to-obligation path is ineligible, so the
  // instance is infeasible and the certificate is a checkable path witness.
  CaseGraph graph;
  add_node(graph, "s0", false, 1);         // uncontainable failure source
  add_node(graph, "a1", false, 1);         // uncontainable interior node
  add_node(graph, "b1", false, 1);         // uncontainable interior node
  add_node(graph, "p0", false, 1, true);   // protected obligation
  add_edge(graph, "s0", "a1");
  add_edge(graph, "a1", "b1");
  add_edge(graph, "b1", "p0");
  const model::Topology topology = require_topology(to_spec(graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const std::vector<model::NodeIndex> none;
  const std::vector<model::NodeIndex> witness = engine::find_witness_path(problem, none);
  FCFN_CHECK_CTX(!witness.empty(), "expected a residual path");
  FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, witness),
                 "the canonical residual path is not a valid certificate");

  SolveBudget budget;
  const CutSolution solution = solve_heuristic(problem, budget);
  FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenInfeasible,
                 "heuristic: " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.has_infeasibility_witness, "heuristic reported no certificate");
  FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, solution.infeasibility_path),
                 "heuristic certificate does not verify");
  FCFN_CHECK_CTX(solution.infeasibility_path == witness,
                 "the certificate is not the canonical residual path");
}

}  // namespace fcfn::test::solver_cases
