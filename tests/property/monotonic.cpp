// FCFN property suite: monotonic safety.
//
// Adding failure sources, promoting UNKNOWN evidence to PROVEN, and refuting
// UNKNOWN edges are the three evidence operations that can move the feasible
// set. The tests below assert the direction of each move against an independent
// enumeration of every feasible containment set.
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

struct SmallInstance {
  CaseGraph graph{};
  std::vector<std::string> sources{};
  std::vector<std::string> extended_sources{};
  /// Sources that are barred from containment: the interesting MCC-1 case is a
  /// failure source that cannot be contained.
  std::vector<std::string> forced_ineligible{};
};

/// Deterministic small instance: one protected obligation at the highest index,
/// a mix of PROVEN and UNKNOWN edges, and sources at the lowest indices.
SmallInstance random_small(Rng& rng) {
  SmallInstance instance;
  const std::size_t node_count = 5 + static_cast<std::size_t>(rng.below(5));  // 5..9
  instance.graph.generation = 1 + rng.below(1000);
  for (std::size_t i = 0; i < node_count; ++i) {
    const bool is_protected = i + 1 == node_count;
    solver_cases::add_node(instance.graph, "n" + std::to_string(i), !is_protected,
                           1 + rng.below(4), is_protected);
  }
  const std::size_t edge_count = node_count + static_cast<std::size_t>(rng.below(node_count));
  for (std::size_t e = 0; e < edge_count; ++e) {
    const std::size_t from = static_cast<std::size_t>(rng.below(node_count));
    const std::size_t to = static_cast<std::size_t>(rng.below(node_count));
    if (from == to) {
      continue;
    }
    const std::string from_id = "n" + std::to_string(from);
    const std::string to_id = "n" + std::to_string(to);
    bool duplicate = false;
    for (const solver_cases::CaseEdge& edge : instance.graph.edges) {
      if (edge.from == from_id && edge.to == to_id) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    const model::EdgeEvidence evidence = rng.chance(40) ? model::EdgeEvidence::Unknown
                                                        : model::EdgeEvidence::Proven;
    solver_cases::add_edge(instance.graph, from_id, to_id, evidence);
  }
  instance.sources = {"n0"};
  instance.extended_sources = node_count >= 3 ? std::vector<std::string>{"n0", "n1"}
                                              : std::vector<std::string>{"n0"};
  if (!rng.chance(30)) {
    instance.forced_ineligible = {"n0"};
  }
  return instance;
}

engine::ContainmentInstanceSpec spec_for(const model::Topology& topology,
                                         const std::vector<std::string>& sources,
                                         const std::vector<std::string>& forced_ineligible) {
  SpecOptions options;
  options.source_count = 0;
  options.extra_sources = sources;
  options.forced_ineligible = forced_ineligible;
  return solver_cases::make_spec(topology, options);
}

std::vector<model::NodeIndex> containable_nodes(const ContainmentProblem& problem) {
  std::vector<model::NodeIndex> nodes;
  for (model::NodeIndex node = 0; node < problem.node_count(); ++node) {
    if (problem.is_containable(node)) {
      nodes.push_back(node);
    }
  }
  return nodes;
}

}  // namespace

FCFN_TEST(property, extra_failure_sources_only_shrink_the_feasible_set) {
  Rng rng(fcfn::test::seed() ^ 0x5a17c0deull);
  std::size_t iterations = 700;
  std::size_t shrunk = 0;
  std::size_t necessity_checks = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const SmallInstance subject = random_small(rng);
    const model::Topology topology = require_topology(solver_cases::to_spec(subject.graph));
    const engine::ContainmentInstanceSpec base_spec = spec_for(topology, subject.sources, subject.forced_ineligible);
    const engine::ContainmentInstanceSpec extended_spec = spec_for(topology, subject.extended_sources, subject.forced_ineligible);
    Instance base(topology, base_spec);
    Instance extended(topology, extended_spec);
    const ContainmentProblem& base_problem = base.problem();
    const ContainmentProblem& extended_problem = extended.problem();
    const std::string context = "iteration=" + std::to_string(i) + " seed=" + std::to_string(fcfn::test::seed()) +
                                " nodes=" + std::to_string(base_problem.node_count()) + " seed_case=" +
                                std::to_string(subject.graph.generation);

    const std::vector<std::uint64_t> base_feasible = enumerate_feasible(base_problem);
    const std::vector<std::uint64_t> extended_feasible = enumerate_feasible(extended_problem);
    FCFN_CHECK_CTX(is_subset(extended_feasible, base_feasible),
                   context + " (adding a failure source grew the feasible set by " +
                       std::to_string(extended_feasible.size() - base_feasible.size()) + " sets)");
    if (extended_feasible.size() < base_feasible.size()) {
      ++shrunk;
    }

    const ReferenceResult base_reference = reference_solve(base_problem);
    const ReferenceResult extended_reference = reference_solve(extended_problem);
    FCFN_CHECK_CTX(base_reference.supported && extended_reference.supported,
                   context + " (reference solver bound exceeded)");
    if (base_reference.feasible) {
      FCFN_CHECK_CTX(extended_reference.feasible,
                     context + " (extended instance lost feasibility)");
      FCFN_CHECK_CTX(!engine::objective_less(extended_reference.objective, base_reference.objective,
                                             base_problem.minimize_protected_inclusions()),
                     context + "\n  base:     " + solver_cases::describe_objective(base_reference.objective) +
                         "\n  extended: " + solver_cases::describe_objective(extended_reference.objective));
    } else {
      FCFN_CHECK_CTX(!extended_reference.feasible,
                     context + " (extended instance became feasible)");
    }

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    for (const model::NodeIndex node : containable_nodes(base_problem)) {
      const CutSolution without = solve_without(topology, base_problem, node, budget);
      if (without.status != SolveStatus::ProvenInfeasible) {
        continue;
      }
      ++necessity_checks;
      const CutSolution extended_without = solve_without(topology, extended_problem, node, budget);
      FCFN_CHECK_CTX(extended_without.status == SolveStatus::ProvenInfeasible,
                     context + " (a member proven necessary in the base instance is removable once a "
                               "failure source is added: " +
                         base_problem.resource_ids()[node].value() + ", extended status=" +
                         engine::to_string(extended_without.status) + ")");
    }
  }
  std::printf("monotonic_sources iterations=%zu shrunk=%zu necessary_members_checked=%zu\n", iterations,
              shrunk, necessity_checks);
  FCFN_CHECK_CTX(shrunk > iterations / 20, "expected the extra source to shrink many feasible sets");
  FCFN_CHECK_CTX(necessity_checks > 20,
                 "expected a population of proven-necessary members: " +
                     std::to_string(necessity_checks));
}

FCFN_TEST(property, promoting_unknown_to_proven_preserves_the_answer) {
  Rng rng(fcfn::test::seed() ^ 0x77aa11bbull);
  std::size_t iterations = 400;
  std::size_t promoted_cases = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const SmallInstance subject = random_small(rng);
    CaseGraph promoted = subject.graph;
    std::size_t promoted_edges = 0;
    for (solver_cases::CaseEdge& edge : promoted.edges) {
      if (edge.evidence == model::EdgeEvidence::Unknown) {
        edge.evidence = model::EdgeEvidence::Proven;
        ++promoted_edges;
      }
    }
    const model::Topology topology = require_topology(solver_cases::to_spec(subject.graph));
    const model::Topology promoted_topology = require_topology(solver_cases::to_spec(promoted));
    const engine::ContainmentInstanceSpec spec = spec_for(topology, subject.sources, subject.forced_ineligible);
    Instance base(topology, spec);
    Instance after(promoted_topology, spec_for(promoted_topology, subject.sources, subject.forced_ineligible));
    const std::string context = "iteration=" + std::to_string(i) + " promoted_edges=" +
                                std::to_string(promoted_edges);

    // The conservative graph is unchanged by the promotion, so the feasible set
    // is identical, not merely nested.
    const std::vector<std::uint64_t> base_feasible = enumerate_feasible(base.problem());
    const std::vector<std::uint64_t> after_feasible = enumerate_feasible(after.problem());
    FCFN_CHECK_CTX(base_feasible == after_feasible,
                   context + " (UNKNOWN->PROVEN changed the feasible set: " +
                       std::to_string(base_feasible.size()) + " vs " +
                       std::to_string(after_feasible.size()) + ")");
    FCFN_CHECK_CTX(base.problem().graph().conservative_edge_count() ==
                       after.problem().graph().conservative_edge_count(),
                   context + " (the conservative edge set changed)");
    if (promoted_edges > 0) {
      ++promoted_cases;
    }

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    const CutSolution before_solution = engine::solve_exact(base.problem(), budget);
    const CutSolution after_solution = engine::solve_exact(after.problem(), budget);
    FCFN_CHECK_CTX(before_solution.status == after_solution.status,
                   context + " (status changed: " + engine::to_string(before_solution.status) + " -> " +
                       engine::to_string(after_solution.status) + ")");
    FCFN_CHECK_CTX(before_solution.members == after_solution.members,
                   context + " (member set changed: " +
                       solver_cases::describe_members(base.problem(), before_solution.members) + " -> " +
                       solver_cases::describe_members(after.problem(), after_solution.members) + ")");
    FCFN_CHECK_CTX(before_solution.objective == after_solution.objective,
                   context + " (objective changed)");
  }
  std::printf("monotonic_promotion iterations=%zu cases_with_unknown_edges=%zu\n", iterations,
              promoted_cases);
  FCFN_CHECK_CTX(promoted_cases > iterations / 2, "expected most instances to contain UNKNOWN edges");
}

FCFN_TEST(property, refuting_unknown_edges_never_grows_the_requirement) {
  Rng rng(fcfn::test::seed() ^ 0x1f2e3d4cull);
  std::size_t iterations = 400;
  std::size_t relaxed = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const SmallInstance subject = random_small(rng);
    CaseGraph refuted = subject.graph;
    for (solver_cases::CaseEdge& edge : refuted.edges) {
      if (edge.evidence == model::EdgeEvidence::Unknown) {
        edge.evidence = model::EdgeEvidence::Refuted;
      }
    }
    const model::Topology topology = require_topology(solver_cases::to_spec(subject.graph));
    const model::Topology refuted_topology = require_topology(solver_cases::to_spec(refuted));
    Instance base(topology, spec_for(topology, subject.sources, subject.forced_ineligible));
    Instance after(refuted_topology, spec_for(refuted_topology, subject.sources, subject.forced_ineligible));
    const std::string context = "iteration=" + std::to_string(i) + " seed=" + std::to_string(fcfn::test::seed());

    const std::vector<std::uint64_t> base_feasible = enumerate_feasible(base.problem());
    const std::vector<std::uint64_t> after_feasible = enumerate_feasible(after.problem());
    FCFN_CHECK_CTX(is_subset(base_feasible, after_feasible),
                   context + " (refuting UNKNOWN edges removed feasible containment sets)");
    if (after_feasible.size() > base_feasible.size()) {
      ++relaxed;
    }
    FCFN_CHECK_CTX(after.problem().graph().conservative_edge_count() <=
                       base.problem().graph().conservative_edge_count(),
                   context);

    const ReferenceResult base_reference = reference_solve(base.problem());
    const ReferenceResult after_reference = reference_solve(after.problem());
    FCFN_CHECK_CTX(base_reference.supported && after_reference.supported, context);
    if (base_reference.feasible) {
      FCFN_CHECK_CTX(after_reference.feasible, context + " (refutation removed all containment)");
      FCFN_CHECK_CTX(!engine::objective_less(base_reference.objective, after_reference.objective,
                                             base.problem().minimize_protected_inclusions()),
                     context + "\n  conservative: " +
                         solver_cases::describe_objective(base_reference.objective) +
                         "\n  refuted:      " +
                         solver_cases::describe_objective(after_reference.objective));
    }
  }
  std::printf("monotonic_refutation iterations=%zu relaxed=%zu\n", iterations, relaxed);
}

}  // namespace fcfn::test::property_cases