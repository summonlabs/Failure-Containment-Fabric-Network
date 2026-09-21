// FCFN solver suite: infeasibility certificates.
//
// MCC-1 completeness requires that an infeasible answer is only returned with a
// checkable certificate: a path in the conservative graph from a failure source
// to a protected obligation whose nodes are all ineligible for containment.
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

/// The only source-to-obligation path runs through ineligible nodes.
CaseGraph ineligible_chain() {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);        // failure source, not containable
  add_node(graph, "m1", false, 1);        // interior, not containable
  add_node(graph, "m2", false, 1);        // interior, not containable
  add_node(graph, "x1", true, 1);         // eligible but off every path
  add_node(graph, "p0", false, 1, true);  // protected obligation
  add_edge(graph, "s0", "m1");
  add_edge(graph, "m1", "m2");
  add_edge(graph, "m2", "p0");
  return graph;
}

/// Same shape with the second interior node containable, used to show that the
/// verifier rejects a certificate whose nodes could have been contained.
CaseGraph containable_interior_chain() {
  CaseGraph graph;
  add_node(graph, "s0", false, 1);
  add_node(graph, "m1", false, 1);
  add_node(graph, "m2", true, 1);
  add_node(graph, "x1", true, 1);
  add_node(graph, "p0", false, 1, true);
  add_edge(graph, "s0", "m1");
  add_edge(graph, "m1", "m2");
  add_edge(graph, "m2", "p0");
  return graph;
}

}  // namespace

FCFN_TEST(solver, exact_solver_infeasibility_carries_a_verifiable_certificate) {
  const model::Topology topology = require_topology(to_spec(ineligible_chain()));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const std::string context = describe_instance(problem);

  const ReferenceResult reference = reference_solve(problem);
  FCFN_CHECK_CTX(reference.supported && !reference.feasible, context + " (reference is feasible)");

  SolveBudget budget;
  budget.max_explored_nodes = 200000;
  const CutSolution solution = solve_exact(problem, budget);
  FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenInfeasible,
                 context + "\n  " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.has_infeasibility_witness,
                 context + "\n  ProvenInfeasible without a checkable certificate\n  " +
                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, solution.infeasibility_path),
                 context + "\n  the reported certificate does not verify\n  " +
                     describe_solution(problem, solution));
}

FCFN_TEST(solver, heuristic_solver_infeasibility_carries_a_verifiable_certificate) {
  const model::Topology topology = require_topology(to_spec(ineligible_chain()));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const std::string context = describe_instance(problem);

  SolveBudget budget;
  const CutSolution solution = solve_heuristic(problem, budget);
  FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenInfeasible,
                 context + "\n  " + describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.has_infeasibility_witness,
                 context + "\n  ProvenInfeasible without a checkable certificate");
  FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, solution.infeasibility_path),
                 context + "\n  the reported certificate does not verify\n  " +
                     describe_solution(problem, solution));
  FCFN_CHECK_CTX(solution.infeasibility_path.size() == 4,
                 "expected the four-node chain as the certificate");
}

FCFN_TEST(solver, certificate_verifier_accepts_genuine_paths_and_rejects_bogus_ones) {
  const model::Topology topology = require_topology(to_spec(ineligible_chain()));
  SpecOptions spec_options;
  spec_options.source_count = 0;
  spec_options.extra_sources = {"s0"};
  Instance instance(topology, make_spec(topology, spec_options));
  const engine::ContainmentProblem& problem = instance.problem();
  const std::vector<model::NodeIndex> none;
  const std::vector<model::NodeIndex> genuine = engine::find_witness_path(problem, none);
  FCFN_CHECK_CTX(genuine.size() == 4, "expected the four-node chain: " +
                                          describe_members(problem, genuine));
  FCFN_CHECK_CTX(engine::verify_infeasibility_witness(problem, genuine),
                 "the canonical residual path must verify");

  const std::vector<std::vector<model::NodeIndex>> bogus = {
      {},
      {genuine[0]},
      {genuine[0], genuine[1], genuine[2]},              // terminal node is not protected
      {genuine[1], genuine[2], genuine[3]},              // first node is not a failure source
      {genuine[3], genuine[2], genuine[1], genuine[0]},  // reversed: edges do not exist
      {genuine[0], genuine[2], genuine[3]},              // skipped node: no such edge
      {genuine[0], genuine[1], genuine[2], genuine[3], genuine[3]},  // repeated node
  };
  for (const std::vector<model::NodeIndex>& path : bogus) {
    FCFN_CHECK_CTX(!engine::verify_infeasibility_witness(problem, path),
                   "the verifier accepted a bogus certificate: " + describe_members(problem, path));
  }

  // The same node sequence is not a certificate when the interior node could
  // have been contained: eligibility of the path is what makes it a proof.
  const model::Topology containable = require_topology(to_spec(containable_interior_chain()));
  Instance permissive(containable, make_spec(containable, spec_options));
  FCFN_CHECK_CTX(permissive.problem().sources().size() == 1 &&
                     permissive.problem().resource_ids()[permissive.problem().sources().front()]
                             .value() == "s0",
                 "the variant must use the same failure source");
  const engine::ContainmentProblem& other = permissive.problem();
  FCFN_CHECK_CTX(!engine::verify_infeasibility_witness(other, genuine),
                 "the verifier accepted a certificate whose nodes are containable");
  const CutSolution solution = solve_exact(other, SolveBudget{});
  FCFN_CHECK_CTX(solution.status == SolveStatus::ProvenOptimal,
                 "the containable interior node must make the instance feasible: " +
                     describe_solution(other, solution));
  FCFN_CHECK_CTX(solution.members.size() == 1 && solution.members.front() == genuine[2],
                 "expected m2 to be the single cheapest containment member: " +
                     describe_members(other, solution.members));
}

}  // namespace fcfn::test::solver_cases
