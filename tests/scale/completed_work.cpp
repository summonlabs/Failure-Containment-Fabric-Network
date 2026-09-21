// FCFN scale suite: completed work on synthetic graphs.
//
// Two things are measured and asserted here. First, that a solve of a graph with
// tens of thousands of nodes completes and that the cost grows roughly with the
// graph rather than with its square. Second, that the governed scope stays
// bounded: the funnel's minimum containment scope is its waist, a constant, no
// matter how large the graph becomes.
//
// No timeouts and no sleeping: the timings are measurements, never limits.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/engine/solver.hpp"
#include "scale_generator.hpp"
#include "solver/case_builder.hpp"

namespace fcfn::test::scale_cases {
namespace {

using engine::CutSolution;
using engine::SolveBudget;
using engine::SolveStatus;
using solver_cases::describe_solution;
using solver_cases::Instance;
using solver_cases::SpecOptions;

/// One measured size.
struct Measurement {
  FunnelShape shape{};
  double build_millis{0.0};
  double solve_millis{0.0};
  std::size_t explored{0};
  std::size_t members{0};
  std::uint64_t weight{0};
  std::uint64_t candidates{0};
  bool verified{false};
};

engine::ContainmentInstanceSpec funnel_spec(const model::Topology& topology) {
  SpecOptions options;
  options.source_count = 0;
  options.extra_sources = {"s0"};
  return solver_cases::make_spec(topology, options);
}

double millis_between(const std::chrono::steady_clock::time_point& start,
                      const std::chrono::steady_clock::time_point& end) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
         1000000.0;
}

/// Build and solve one funnel of the requested size.
Measurement measure(std::size_t nodes, std::size_t waist, std::uint64_t solve_budget,
                    std::uint64_t iteration_budget) {
  Measurement result;
  FunnelOptions options;
  options.nodes = nodes;
  options.waist = waist;
  const model::TopologySpec spec = funnel_topology(options, result.shape);

  const auto build_start = std::chrono::steady_clock::now();
  const model::Topology topology = require_topology(spec);
  const engine::PropagationGraph graph = engine::PropagationGraph::build(topology);
  FCFN_CHECK_CTX(graph.node_count() == topology.node_count(),
                 "the indexed graph does not cover the topology");
  Instance instance(topology, funnel_spec(topology));
  const auto build_end = std::chrono::steady_clock::now();
  result.build_millis = millis_between(build_start, build_end);

  const engine::ContainmentProblem& problem = instance.problem();
  result.candidates = static_cast<std::uint64_t>(problem.candidates().size());
  SolveBudget budget;
  budget.max_explored_nodes = solve_budget;
  budget.max_iterations = iteration_budget;
  const auto solve_start = std::chrono::steady_clock::now();
  const CutSolution solution = engine::solve_heuristic(problem, budget);
  const auto solve_end = std::chrono::steady_clock::now();
  result.solve_millis = millis_between(solve_start, solve_end);
  result.explored = static_cast<std::size_t>(solution.counters.explored_nodes);
  result.verified = engine::verify_cut(problem, solution.members).ok();
  result.members = solution.members.size();
  if (solution.has_solution()) {
    result.weight = solution.objective.total_weight;
  }
  FCFN_CHECK_CTX(solution.status == SolveStatus::FeasibleNotProvenOptimal ||
                     solution.status == SolveStatus::LimitReachedNoSolution,
                 "unexpected status at size " + std::to_string(nodes) + ": " +
                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.status != SolveStatus::ProvenOptimal,
                 "the scalable solver claimed optimality at size " + std::to_string(nodes));
  return result;
}

}  // namespace

FCFN_TEST(scale, completed_work_grows_with_the_graph_not_its_square) {
  const std::size_t sizes[] = {2000, 8000, 32000};
  std::vector<Measurement> measurements;
  std::printf("scale table: size  nodes  edges  build_ms  solve_ms  total_ms  explored  candidates  members  weight  verified\n");
  for (const std::size_t size : sizes) {
    const Measurement measurement = measure(size, 4, 200000, 200000);
    measurements.push_back(measurement);
    const double total = measurement.build_millis + measurement.solve_millis;
    std::printf("scale        %6zu %6zu %6zu %8.3f %9.3f %9.3f %9zu %11llu %8zu %7llu  %s\n", size,
                measurement.shape.nodes, measurement.shape.edges, measurement.build_millis,
                measurement.solve_millis, total, measurement.explored,
                static_cast<unsigned long long>(measurement.candidates), measurement.members,
                static_cast<unsigned long long>(measurement.weight),
                measurement.verified ? "yes" : "NO");
    FCFN_CHECK_CTX(measurement.verified, "the returned cut did not verify at size " + std::to_string(size));
    FCFN_CHECK_CTX(measurement.shape.edges <= 262144, "the fixture exceeded the edge bound");
    FCFN_CHECK_CTX(measurement.shape.nodes <= 65536, "the fixture exceeded the node bound");
  }
  FCFN_CHECK_CTX(measurements.size() == 3, "expected three measured sizes");

  for (std::size_t i = 1; i < measurements.size(); ++i) {
    const double previous = measurements[i - 1].build_millis + measurements[i - 1].solve_millis;
    const double current = measurements[i].build_millis + measurements[i].solve_millis;
    const double ratio = previous > 0.0 ? current / previous : 0.0;
    std::printf("scale ratio  %zu -> %zu nodes: %.2fx (size increase %.2fx)\n",
                sizes[i - 1], sizes[i], ratio,
                static_cast<double>(sizes[i]) / static_cast<double>(sizes[i - 1]));
    // Loose sanity bound only: a 4x size increase must never cost 60x the time.
    FCFN_CHECK_CTX(ratio < 60.0,
                   "non-linear blow-up: " + std::to_string(sizes[i - 1]) + " -> " +
                       std::to_string(sizes[i]) + " nodes cost " + std::to_string(ratio) + "x");
  }

  // The largest measurement must still have produced a verified cut.
  const Measurement& largest = measurements.back();
  FCFN_CHECK_CTX(largest.members == 4, "expected the four-node waist as the scope");
  FCFN_CHECK_CTX(largest.weight == 4, "expected the waist weight of four");
}

FCFN_TEST(scale, governed_scope_stays_bounded_as_the_graph_grows) {
  // The waist is the only cut: every path leaves the source through it, so the
  // governed scope is exactly the waist and it does not grow with the graph.
  for (const std::size_t size : {2000u, 8000u, 32000u}) {
    const Measurement measurement = measure(size, 4, 200000, 200000);
    const std::string context = "size=" + std::to_string(size) + " nodes=" +
                                std::to_string(measurement.shape.nodes) + " edges=" +
                                std::to_string(measurement.shape.edges);
    FCFN_CHECK_CTX(measurement.verified, context + " (the cut does not verify)");
    FCFN_CHECK_CTX(measurement.members == measurement.shape.waist,
                   context + " (scope is " + std::to_string(measurement.members) +
                       " members, expected the waist of " + std::to_string(measurement.shape.waist) + ")");
    FCFN_CHECK_CTX(measurement.weight == measurement.shape.waist,
                   context + " (scope weight " + std::to_string(measurement.weight) + ")");
    FCFN_CHECK_CTX(measurement.explored <= measurement.shape.waist + 1,
                   context + " (explored " + std::to_string(measurement.explored) +
                       " iterations for a waist of " + std::to_string(measurement.shape.waist) + ")");
    FCFN_CHECK_CTX(measurement.candidates >= measurement.shape.nodes / 2,
                   context + " (the solver did not face the whole graph: " +
                       std::to_string(measurement.candidates) + " candidates)");
  }
  std::printf("scale bounded: scope stayed at the waist for sizes 2000/8000/32000\n");
}

FCFN_TEST(scale, tiny_budget_at_scale_is_an_explicit_bounded_outcome) {
  FunnelOptions options;
  options.nodes = 8000;
  options.waist = 4;
  FunnelShape shape;
  const model::TopologySpec spec = funnel_topology(options, shape);
  const model::Topology topology = require_topology(spec);
  Instance instance(topology, funnel_spec(topology));
  const engine::ContainmentProblem& problem = instance.problem();

  SolveBudget starved;
  starved.max_iterations = 1;
  starved.max_explored_nodes = 200000;
  const CutSolution solution = engine::solve_heuristic(problem, starved);
  const std::string context = "nodes=" + std::to_string(shape.nodes) + " edges=" +
                              std::to_string(shape.edges) + "\n  " +
                              describe_solution(problem, solution);
  FCFN_CHECK_CTX(solution.status == SolveStatus::LimitReachedNoSolution,
                 "a one-iteration budget must be reported as bounded: " + context);
  FCFN_CHECK_CTX(solution.counters.budget_exhausted, "the bounded outcome was not flagged: " + context);
  FCFN_CHECK_CTX(solution.status != SolveStatus::ProvenOptimal,
                 "a bounded search claimed optimality: " + context);
  FCFN_CHECK_CTX(solution.status != SolveStatus::ProvenInfeasible,
                 "a bounded search claimed infeasibility: " + context);
  FCFN_CHECK_CTX(!solution.has_solution(), "a partial construction was returned as a solution: " + context);

  // The same instance completes once the iteration budget is adequate.
  SolveBudget ample;
  ample.max_iterations = 200000;
  const CutSolution completed = engine::solve_heuristic(problem, ample);
  FCFN_CHECK_CTX(completed.has_solution(), "the instance must be solvable: " +
                                               describe_solution(problem, completed));
  FCFN_CHECK_CTX(engine::verify_cut(problem, completed.members).ok(),
                 "the completed cut does not verify");
  std::printf("scale tiny_budget: bounded status=%s, completed status=%s members=%zu\n",
              engine::to_string(solution.status), engine::to_string(completed.status),
              completed.members.size());
}

}  // namespace fcfn::test::scale_cases
