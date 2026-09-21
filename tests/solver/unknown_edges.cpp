// FCFN solver suite: UNKNOWN edges are never assumed absent.
//
// The conservative graph contains PROVEN and UNKNOWN edges; REFUTED edges are in
// neither view. An UNKNOWN edge that would otherwise provide a bypass must still
// be cut, even when cutting it is impossible and the instance is infeasible.
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

/// s0 -> a(10) -> p0 plus a direct s0 -> p0 edge whose evidence is a parameter.
CaseGraph bypass_graph(model::EdgeEvidence direct_evidence) {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);
  add_node(graph, "a", true, 10);
  add_node(graph, "p0", false, 1, true);
  add_edge(graph, "s0", "a");
  add_edge(graph, "a", "p0");
  add_edge(graph, "s0", "p0", direct_evidence);
  return graph;
}

/// s0 -> a(1) -> p0 and s0 -> x(1) -> p0 with the second edge's evidence set by
/// the caller: with UNKNOWN both members are required, with REFUTED only one.
CaseGraph parallel_graph(model::EdgeEvidence second_evidence) {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);
  add_node(graph, "a", true, 1);
  add_node(graph, "x", true, 1);
  add_node(graph, "p0", false, 1, true);
  add_edge(graph, "s0", "a");
  add_edge(graph, "a", "p0");
  add_edge(graph, "s0", "x");
  add_edge(graph, "x", "p0", second_evidence);
  return graph;
}

Instance build(const CaseGraph& graph) {
  const model::Topology topology = require_topology(to_spec(graph));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  return Instance(topology, make_spec(topology, spec_options));
}

}  // namespace

FCFN_TEST(solver, unknown_bypass_makes_the_instance_infeasible) {
  for (const model::EdgeEvidence evidence :
       {model::EdgeEvidence::Unknown, model::EdgeEvidence::Proven}) {
    const Instance subject = build(bypass_graph(evidence));
    const engine::ContainmentProblem& problem = subject.problem();
    const std::string context = std::string("evidence=") + model::to_string(evidence) + " " +
                                describe_instance(problem);
    FCFN_CHECK_CTX(problem.graph().conservative_edge_count() == 3, context);
    FCFN_CHECK_CTX(problem.graph().unknown_edge_count() ==
                       (evidence == model::EdgeEvidence::Unknown ? 1u : 0u),
                   context);
    SolveBudget budget;
    const CutSolution solution = solve_exact(problem, budget);
    FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenInfeasible,
                   context + "\n  the direct bypass must be cut, but it cannot be\n  " +
                       describe_solution(problem, solution));
    const ReferenceResult reference = reference_solve(problem);
    FCFN_CHECK_CTX(reference.supported && !reference.feasible, context + " (reference is feasible)");

    const CutSolution heuristic = solve_heuristic(problem, budget);
    FCFN_CHECK_CTX(heuristic.status == SolveStatus::ProvenInfeasible,
                   context + " (heuristic): " + describe_solution(problem, heuristic));
    FCFN_CHECK_CTX(heuristic.has_infeasibility_witness &&
                       engine::verify_infeasibility_witness(problem, heuristic.infeasibility_path),
                   context + " (heuristic certificate does not verify)");
    FCFN_CHECK_CTX(heuristic.infeasibility_path.size() == 2,
                   context + " (expected the direct bypass as the certificate)");
  }
}

FCFN_TEST(solver, refuted_bypass_is_not_a_path) {
  const Instance subject = build(bypass_graph(model::EdgeEvidence::Refuted));
  const engine::ContainmentProblem& problem = subject.problem();
  const std::string context = describe_instance(problem);
  FCFN_CHECK_CTX(problem.graph().conservative_edge_count() == 2,
                 context + " (a REFUTED edge must not enter the conservative view)");
  FCFN_CHECK_CTX(problem.graph().proven_edge_count() == 2, context);

  SolveBudget budget;
  const CutSolution solution = solve_exact(problem, budget);
  FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenOptimal,
                 context + "\n  " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.members.size() == 1 &&
                     problem.resource_ids()[solution.members.front()].value() == "a",
                 context + " (expected containment of a only)");
  FCFN_CHECK_CTX(solution.objective.total_weight == 10,
                 context + " (expected weight 10): " + describe_objective(solution.objective));

  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && reference.feasible, context + " (reference infeasible)");
  FCFN_CHECK_CTX(reference.objective == solution.objective, context);
}

FCFN_TEST(solver, unknown_parallel_path_must_be_cut) {
  const Instance unknown_case = build(parallel_graph(model::EdgeEvidence::Unknown));
  const engine::ContainmentProblem& unknown_problem = unknown_case.problem();
  const std::string unknown_context = describe_instance(unknown_problem);
  SolveBudget budget;
  const CutSolution unknown_solution = solve_exact(unknown_problem, budget);
  FCFN_CHECK_CTX(unknown_solution.status == SolveStatus::ProvenOptimal,
                 unknown_context + "\n  " + describe_solution(unknown_problem, unknown_solution));
  FCFN_CHECK_CTX(unknown_solution.members.size() == 2,
                 unknown_context + " (both parallel paths must be cut): " +
                     describe_solution(unknown_problem, unknown_solution));
  FCFN_CHECK_CTX(unknown_solution.objective.total_weight == 2,
                 unknown_context + " (expected weight 2): " +
                     describe_objective(unknown_solution.objective));

  const Instance proven_case = build(parallel_graph(model::EdgeEvidence::Proven));
  const CutSolution proven_solution = solve_exact(proven_case.problem(), budget);
  FCFN_CHECK_CTX(proven_solution.objective == unknown_solution.objective,
                 "promoting UNKNOWN to PROVEN changed the conservative optimum: " +
                     describe_objective(proven_solution.objective) + " vs " +
                     describe_objective(unknown_solution.objective));
  FCFN_CHECK_CTX(proven_solution.members == unknown_solution.members,
                 "promoting UNKNOWN to PROVEN changed the containment members");

  const Instance refuted_case = build(parallel_graph(model::EdgeEvidence::Refuted));
  const CutSolution refuted_solution = solve_exact(refuted_case.problem(), budget);
  FCFN_CHECK_CTX(refuted_solution.status == SolveStatus::ProvenOptimal,
                 "refuted: " + describe_solution(refuted_case.problem(), refuted_solution));
  FCFN_CHECK_CTX(refuted_solution.objective.total_weight == 1,
                 "a REFUTED edge must not create a second path: " +
                     describe_objective(refuted_solution.objective));
  FCFN_CHECK_CTX(refuted_solution.members.size() == 1,
                 "a REFUTED edge must not create a second path");
}

FCFN_TEST(solver, witness_paths_classify_unknown_evidence) {
  const Instance unknown_case = build(parallel_graph(model::EdgeEvidence::Unknown));
  const engine::ContainmentProblem& unknown_problem = unknown_case.problem();
  const std::vector<model::NodeIndex> none;
  bool unrestricted_uses_unknown = true;
  const std::vector<model::NodeIndex> unrestricted =
      engine::find_witness_path(unknown_problem, none, &unrestricted_uses_unknown);
  FCFN_CHECK_CTX(!unrestricted.empty(), "expected a residual path");
  FCFN_CHECK_CTX(!unrestricted_uses_unknown,
                 "the canonical path prefers the fully PROVEN route: " +
                     describe_members(unknown_problem, unrestricted));

  // Excluding the PROVEN route forces the witness through the UNKNOWN edge.
  const std::vector<model::NodeIndex> exclude_proven{unrestricted[1]};
  bool uses_unknown = false;
  const std::vector<model::NodeIndex> unknown_path =
      engine::find_witness_path(unknown_problem, exclude_proven, &uses_unknown);
  FCFN_CHECK_CTX(!unknown_path.empty(), "expected a residual path through the UNKNOWN edge");
  FCFN_CHECK_CTX(uses_unknown, "the residual path uses an UNKNOWN edge: " +
                                   describe_members(unknown_problem, unknown_path));

  const Instance proven_case = build(parallel_graph(model::EdgeEvidence::Proven));
  const engine::ContainmentProblem& proven_problem = proven_case.problem();
  bool proven_uses_unknown = true;
  const std::vector<model::NodeIndex> proven_path =
      engine::find_witness_path(proven_problem, none, &proven_uses_unknown);
  FCFN_CHECK_CTX(!proven_path.empty(), "expected a residual path");
  FCFN_CHECK_CTX(!proven_uses_unknown,
                 "a PROVEN-only path must not be classified as UNKNOWN evidence");

  const Instance refuted_case = build(parallel_graph(model::EdgeEvidence::Refuted));
  const engine::ContainmentProblem& refuted_problem = refuted_case.problem();
  bool refuted_uses_unknown = true;
  const std::vector<model::NodeIndex> refuted_path =
      engine::find_witness_path(refuted_problem, none, &refuted_uses_unknown);
  FCFN_CHECK_CTX(!refuted_path.empty(), "expected a residual path through the PROVEN edge");
  FCFN_CHECK_CTX(!refuted_uses_unknown, "the REFUTED edge must not be traversed");
}

}  // namespace fcfn::test::solver_cases
