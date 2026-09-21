// FCFN property suite: necessity analysis.
//
// analyze_necessity may only claim PROVEN NECESSARY for a member that really is
// in every feasible containment set, and it may only call a member removable
// when a completed search found a containment set without it.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "property_support.hpp"
#include "fixtures.hpp"
#include "fcfn/engine/necessity.hpp"
#include "reference_solver.hpp"

namespace fcfn::test::property_cases {
namespace {

engine::ContainmentInstanceSpec single_source_spec(const model::Topology& topology,
                                                     bool containable_source) {
  SpecOptions options;
  options.source_count = 0;
  options.extra_sources = {"n0"};
  if (!containable_source) {
    options.forced_ineligible = {"n0"};
  }
  return solver_cases::make_spec(topology, options);
}

CaseGraph random_graph(Rng& rng, std::size_t node_count, bool source_containable) {
  CaseGraph graph;
  graph.generation = 1 + rng.below(1000);
  for (std::size_t i = 0; i < node_count; ++i) {
    const bool is_protected = i + 1 == node_count;
    const bool containable = !is_protected && !(i == 0 && !source_containable);
    solver_cases::add_node(graph, "n" + std::to_string(i), containable, 1 + rng.below(4),
                           is_protected);
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
                           rng.chance(35) ? model::EdgeEvidence::Unknown : model::EdgeEvidence::Proven);
  }
  return graph;
}

bool contains(const std::vector<model::NodeIndex>& nodes, model::NodeIndex node) {
  return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

}  // namespace

FCFN_TEST(property, necessity_agrees_with_the_forced_ineligible_oracle) {
  Rng rng(fcfn::test::seed() ^ 0x3c9a5e11ull);
  std::size_t iterations = 900;
  std::size_t subjects = 0;
  std::size_t necessary = 0;
  std::size_t policy_mandatory = 0;
  std::size_t policy_shortcut_observed = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const CaseGraph graph = random_graph(rng, 5 + static_cast<std::size_t>(rng.below(5)), true);
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    // The source stays containable so the instance carries no forced-ineligible
    // nodes: the forced-ineligible reconstruction below is then exact, and the
    // analysis is compared against a faithful oracle. Instances that do carry
    // barred nodes are covered by necessity_respects_forced_ineligible_nodes.
    Instance instance(topology, single_source_spec(topology, true));
    const ContainmentProblem& problem = instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                solver_cases::describe_instance(problem);

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    const engine::NecessityResult result = engine::analyze_necessity_exhaustive(problem, budget);
    FCFN_CHECK_CTX(result.indeterminate.empty(),
                   context + " (indeterminate with a 200k budget: " +
                       std::to_string(result.indeterminate.size()) + " subjects)");

    for (const model::NodeIndex node : problem.candidates()) {
      if (!problem.is_containable(node)) {
        continue;
      }
      ++subjects;
      const CutSolution oracle = solve_without(topology, problem, node, budget);
      const bool oracle_infeasible = oracle.status == SolveStatus::ProvenInfeasible;
      const bool reported_necessary = contains(result.proven_necessary, node);
      const bool reported_not_necessary = contains(result.not_necessary, node);
      FCFN_CHECK_CTX(reported_necessary || reported_not_necessary,
                     context + " (subject was classified neither way: " +
                         problem.resource_ids()[node].value() + ")");
      if (reported_necessary) {
        ++necessary;
      }
      const bool mandatory_source =
          problem.require_source_inclusion() && problem.is_source(node);
      if (mandatory_source) {
        ++policy_mandatory;
        // An eligible failure source is necessary by policy: every feasible set
        // contains it. That is checked against the enumeration in
        // proven_necessary_members_lie_in_every_feasible_set; the forced
        // ineligible instance is a different instance (the source is no longer
        // required), so it may well be feasible.
        if (!oracle_infeasible) {
          ++policy_shortcut_observed;
          FCFN_CHECK_CTX(reported_necessary, context + " (mandatory source not reported necessary)");
        }
        continue;
      }
      if (reported_necessary) {
        FCFN_CHECK_CTX(oracle_infeasible,
                       context + " (claimed proven necessary without a proof: " +
                           problem.resource_ids()[node].value() + ", oracle=" +
                           engine::to_string(oracle.status) + ")");
      }
      if (reported_not_necessary) {
        FCFN_CHECK_CTX(!oracle_infeasible,
                       context + " (claimed removable although barring it is infeasible: " +
                           problem.resource_ids()[node].value() + ")");
      }
    }
  }
  std::printf("necessity_oracle iterations=%zu subjects=%zu necessary=%zu mandatory_sources=%zu "
              "policy_shortcut=%zu\n",
              iterations, subjects, necessary, policy_mandatory, policy_shortcut_observed);
  FCFN_CHECK_CTX(subjects > 600, "expected a large subject population: " + std::to_string(subjects));
  FCFN_CHECK_CTX(policy_shortcut_observed > 0,
                 "expected the policy shortcut on eligible failure sources to be exercised");
}

FCFN_TEST(property, proven_necessary_members_lie_in_every_feasible_set) {
  Rng rng(fcfn::test::seed() ^ 0x6b1d2f43ull);
  std::size_t iterations = 400;
  std::size_t checked_members = 0;
  std::size_t removable_witnessed = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    // Natively uncontainable sources keep the reconstruction exact: the instance
    // bars nothing through forced_ineligible.
    const bool source_containable = rng.chance(30);
    const CaseGraph graph =
        random_graph(rng, 5 + static_cast<std::size_t>(rng.below(5)), source_containable);
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    Instance instance(topology, single_source_spec(topology, true));
    const ContainmentProblem& problem = instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                solver_cases::describe_instance(problem);

    const std::vector<std::uint64_t> feasible = enumerate_feasible(problem);
    if (feasible.empty()) {
      continue;
    }

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    const CutSolution solution = engine::solve_exact(problem, budget);
    FCFN_CHECK_CTX(solution.has_solution() || solution.status == SolveStatus::ProvenInfeasible,
                   context + " (unexpected exact outcome: " + engine::to_string(solution.status) + ")");
    if (!solution.has_solution()) {
      continue;
    }

    const engine::NecessityResult result =
        engine::analyze_necessity(problem, solution.members, budget);
    for (const model::NodeIndex node : result.proven_necessary) {
      ++checked_members;
      for (const std::uint64_t mask : feasible) {
        FCFN_CHECK_CTX((mask & (1ull << node)) != 0,
                       context + " (member " + problem.resource_ids()[node].value() +
                           " is claimed proven necessary but " + mask_text(mask) +
                           " is a feasible containment set without it)");
      }
    }
    for (const model::NodeIndex node : result.not_necessary) {
      bool witnessed = false;
      for (const std::uint64_t mask : feasible) {
        if ((mask & (1ull << node)) == 0) {
          witnessed = true;
          break;
        }
      }
      FCFN_CHECK_CTX(witnessed,
                     context + " (member " + problem.resource_ids()[node].value() +
                         " is claimed removable but every feasible containment set contains it)");
      ++removable_witnessed;
    }
  }
  std::printf("necessity_enumeration iterations=%zu necessary_members=%zu removable_members=%zu\n",
              iterations, checked_members, removable_witnessed);
  FCFN_CHECK_CTX(checked_members > 100, "expected a population of proven-necessary members");
}

FCFN_TEST(property, necessity_is_indeterminate_when_the_budget_ends) {
  Rng rng(fcfn::test::seed() ^ 0x0d5c7a91ull);
  std::size_t iterations = 120;
  std::size_t indeterminate_seen = 0;
  std::size_t starved_analyses = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    // A natively uncontainable source cannot be resurrected by the necessity
    // rebuild, so a one-node budget genuinely cannot decide these subjects.
    const CaseGraph graph = random_graph(rng, 6 + static_cast<std::size_t>(rng.below(4)), false);
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    Instance instance(topology, single_source_spec(topology, true));
    const ContainmentProblem& problem = instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                solver_cases::describe_instance(problem);

    SolveBudget starved;
    starved.max_explored_nodes = 1;
    std::vector<model::NodeIndex> subjects;
    std::size_t solvable_subjects = 0;
    for (const model::NodeIndex node : problem.candidates()) {
      if (!problem.is_containable(node)) {
        continue;
      }
      subjects.push_back(node);
      if (!(problem.require_source_inclusion() && problem.is_source(node))) {
        ++solvable_subjects;
      }
    }
    if (solvable_subjects == 0) {
      // Only policy-mandated sources are candidates: no solve is required, so
      // there is nothing bounded to observe.
      continue;
    }
    const engine::NecessityResult result = engine::analyze_necessity(problem, subjects, starved);
    indeterminate_seen += result.indeterminate.size();
    if (result.counters.budget_exhausted) {
      ++starved_analyses;
    }
    FCFN_CHECK_CTX(result.indeterminate.size() + result.proven_necessary.size() +
                           result.not_necessary.size() ==
                       subjects.size(),
                   context + " (a starved analysis must classify every subject)");

    SolveBudget oracle_budget;
    oracle_budget.max_explored_nodes = 1;
    for (const model::NodeIndex node : subjects) {
      if (problem.require_source_inclusion() && problem.is_source(node)) {
        continue;
      }
      const CutSolution oracle = solve_without(topology, problem, node, oracle_budget);
      if (oracle.status == SolveStatus::ProvenInfeasible) {
        FCFN_CHECK_CTX(contains(result.proven_necessary, node),
                       context + " (barring " + problem.resource_ids()[node].value() +
                           " is infeasible but it was not reported necessary)");
      } else if (contains(result.not_necessary, node)) {
        FCFN_CHECK_CTX(oracle.status == SolveStatus::ProvenOptimal ||
                           oracle.status == SolveStatus::FeasibleNotProvenOptimal,
                       context + " (member reported removable but no containment was found: " +
                           problem.resource_ids()[node].value() + " oracle=" +
                           engine::to_string(oracle.status) + ")");
      }
    }
  }
  std::printf("necessity_bounded iterations=%zu indeterminate=%zu starved=%zu\n", iterations,
              indeterminate_seen, starved_analyses);
  FCFN_CHECK_CTX(indeterminate_seen > 0, "expected indeterminate necessity under a tiny budget");
  FCFN_CHECK_CTX(starved_analyses > 0, "expected the starved budget to be reported");
}

FCFN_TEST(property, necessity_uses_the_policy_rule_and_the_forced_ineligible_proof) {
  // s0 is the failure source and is containable; a1 is the only cut downstream.
  CaseGraph graph;
  solver_cases::add_node(graph, "s0", true, 1);
  solver_cases::add_node(graph, "a1", true, 1);
  solver_cases::add_node(graph, "p0", false, 1, true);
  solver_cases::add_edge(graph, "s0", "a1");
  solver_cases::add_edge(graph, "a1", "p0");
  const model::Topology topology = require_topology(solver_cases::to_spec(graph));
  SpecOptions options;
  options.source_count = 0;
  options.extra_sources = {"s0"};
  Instance instance(topology, solver_cases::make_spec(topology, options));
  const ContainmentProblem& problem = instance.problem();
  const std::string context = "s0(containable source) -> a1 -> p0";
  const auto source_index = topology.find(model::ResourceId::unchecked("s0"));
  const auto cut_index = topology.find(model::ResourceId::unchecked("a1"));
  FCFN_CHECK_CTX(source_index.has_value() && cut_index.has_value(), context);
  FCFN_CHECK_CTX(problem.is_containable(*source_index), context + " (the source must be containable)");

  SolveBudget budget;
  budget.max_explored_nodes = 200000;
  const std::vector<model::NodeIndex> source_subject{*source_index};
  const engine::NecessityResult policy = engine::analyze_necessity(problem, source_subject, budget);
  FCFN_CHECK_CTX(contains(policy.proven_necessary, *source_index),
                 context + " (an eligible failure source is necessary by policy)");
  FCFN_CHECK_CTX(policy.counters.explored_nodes == 0 && !policy.counters.budget_exhausted,
                 context + " (the policy rule must not run a solve)");

  // A non-source member is decided by the forced-ineligible solve alone.
  const std::vector<model::NodeIndex> cut_subject{*cut_index};
  const engine::NecessityResult decided = engine::analyze_necessity(problem, cut_subject, budget);
  const CutSolution oracle = solve_without(topology, problem, *cut_index, budget);
  FCFN_CHECK_CTX(oracle.status == SolveStatus::ProvenOptimal ||
                     oracle.status == SolveStatus::FeasibleNotProvenOptimal,
                 context + " (the source alone contains the failure once a1 is barred)");
  FCFN_CHECK_CTX(contains(decided.not_necessary, *cut_index),
                 context + " (a member the forced-ineligible solve can replace is not necessary)");
  FCFN_CHECK_CTX(decided.counters.explored_nodes > 0,
                 context + " (a non-source member must be decided by a solve)");

  // s0 -> a1 -> p0 with the source natively uncontainable: a1 is the only cut.
  CaseGraph strict;
  solver_cases::add_node(strict, "s0", false, 1);
  solver_cases::add_node(strict, "a1", true, 1);
  solver_cases::add_node(strict, "p0", false, 1, true);
  solver_cases::add_edge(strict, "s0", "a1");
  solver_cases::add_edge(strict, "a1", "p0");
  const model::Topology strict_topology = require_topology(solver_cases::to_spec(strict));
  Instance strict_instance(strict_topology, solver_cases::make_spec(strict_topology, options));
  const ContainmentProblem& strict_problem = strict_instance.problem();
  const auto only_cut = strict_topology.find(model::ResourceId::unchecked("a1"));
  FCFN_CHECK_CTX(only_cut.has_value(), context);
  const std::vector<model::NodeIndex> only_cut_subject{*only_cut};

  const engine::NecessityResult proven =
      engine::analyze_necessity(strict_problem, only_cut_subject, budget);
  FCFN_CHECK_CTX(contains(proven.proven_necessary, *only_cut),
                 context + " (barring the only cut must prove it necessary)");

  // A two-node chain: barring one member leaves the other, so the proof needs a
  // completed search, and a one-node budget cannot supply one.
  CaseGraph chain;
  solver_cases::add_node(chain, "s0", false, 1);
  solver_cases::add_node(chain, "a1", true, 1);
  solver_cases::add_node(chain, "a2", true, 1);
  solver_cases::add_node(chain, "p0", false, 1, true);
  solver_cases::add_edge(chain, "s0", "a1");
  solver_cases::add_edge(chain, "a1", "a2");
  solver_cases::add_edge(chain, "a2", "p0");
  const model::Topology chain_topology = require_topology(solver_cases::to_spec(chain));
  Instance chain_instance(chain_topology, solver_cases::make_spec(chain_topology, options));
  const ContainmentProblem& chain_problem = chain_instance.problem();
  const auto first_cut = chain_topology.find(model::ResourceId::unchecked("a1"));
  FCFN_CHECK_CTX(first_cut.has_value(), context);
  const std::vector<model::NodeIndex> first_cut_subject{*first_cut};

  const engine::NecessityResult replaceable =
      engine::analyze_necessity(chain_problem, first_cut_subject, budget);
  FCFN_CHECK_CTX(contains(replaceable.not_necessary, *first_cut),
                 context + " (the second chain member replaces the first)");

  SolveBudget starved;
  starved.max_explored_nodes = 1;
  const engine::NecessityResult bounded =
      engine::analyze_necessity(chain_problem, first_cut_subject, starved);
  FCFN_CHECK_CTX(!contains(bounded.proven_necessary, *first_cut),
                 context + " (a starved solve must not produce a necessity proof)");
  FCFN_CHECK_CTX(!contains(bounded.not_necessary, *first_cut),
                 context + " (a starved solve must not call a member removable)");
  FCFN_CHECK_CTX(contains(bounded.indeterminate, *first_cut),
                 context + " (a starved solve must report indeterminate necessity)");
  FCFN_CHECK_CTX(bounded.counters.budget_exhausted,
                 context + " (the starved necessity solve must report exhaustion)");
}

FCFN_TEST(property, necessity_respects_the_instances_own_forced_ineligible_nodes) {
  // s0 is the failure source but the instance bars it from containment, so the
  // only containment that disconnects s0 from p0 is {cut}. Making "cut"
  // ineligible must therefore make the instance infeasible: it is PROVEN
  // NECESSARY. Rebuilding the instance for the necessity test without the
  // instance's own barred set would silently make s0 containable again, and the
  // member would be reported removable.
  CaseGraph graph;
  solver_cases::add_node(graph, "s0", true, 1);
  solver_cases::add_node(graph, "cut", true, 1);
  solver_cases::add_node(graph, "p0", false, 1, true);
  solver_cases::add_edge(graph, "s0", "cut");
  solver_cases::add_edge(graph, "cut", "p0");
  const std::string context = "instance: s0(source, barred) -> cut -> p0(protected), all weights 1";

  const model::Topology topology = require_topology(solver_cases::to_spec(graph));
  SpecOptions options;
  options.source_count = 0;
  options.extra_sources = {"s0"};
  options.forced_ineligible = {"s0"};
  Instance instance(topology, solver_cases::make_spec(topology, options));
  const ContainmentProblem& problem = instance.problem();
  FCFN_CHECK_CTX(problem.sources().size() == 1 && problem.uncontainable_sources().size() == 1,
                 context + " (" + solver_cases::describe_instance(problem) + ")");
  const auto barred_source = topology.find(model::ResourceId::unchecked("s0"));
  FCFN_CHECK_CTX(barred_source.has_value(), context);
  FCFN_CHECK_CTX(problem.barred_nodes().size() == 1 &&
                     problem.barred_nodes().front() == *barred_source,
                 context + " (the instance does not expose the node it barred)");

  const std::vector<std::uint64_t> feasible = enumerate_feasible(problem);
  FCFN_CHECK_CTX(feasible.size() == 1, context + " (expected exactly one feasible set, got " +
                                        std::to_string(feasible.size()) + ")");

  SolveBudget budget;
  budget.max_explored_nodes = 200000;
  const auto cut_index = topology.find(model::ResourceId::unchecked("cut"));
  FCFN_CHECK_CTX(cut_index.has_value(), context);
  const model::NodeIndex cut = *cut_index;
  FCFN_CHECK_CTX((feasible.front() & (1ull << cut)) != 0,
                 context + " (the enumeration must contain the cut node)");

  const CutSolution oracle = solve_without(topology, problem, cut, budget);
  FCFN_CHECK_CTX(oracle.status == SolveStatus::ProvenInfeasible,
                 context + " (barring the cut node must be infeasible: oracle=" +
                     engine::to_string(oracle.status) + ")");

  const std::vector<model::NodeIndex> subjects{cut};
  const engine::NecessityResult result = engine::analyze_necessity(problem, subjects, budget);
  FCFN_CHECK_CTX(contains(result.proven_necessary, cut),
                 context + " (analyze_necessity reported the only containment member as removable " +
                     "(not_necessary); it rebuilt the instance without the instance's own "
                     "forced-ineligible source, which made the source containable and the instance "
                     "feasible again)");
}

}  // namespace fcfn::test::property_cases