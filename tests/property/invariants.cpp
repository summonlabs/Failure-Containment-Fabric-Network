// FCFN property suite: determinism, independent hard-constraint checking, and
// plan digest stability.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fcfn/engine/planner.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/policy.hpp"
#include "property_support.hpp"

namespace fcfn::test::property_cases {
namespace {

CaseGraph random_graph(Rng& rng, std::size_t node_count) {
  CaseGraph graph;
  graph.generation = 1 + rng.below(1000);
  for (std::size_t i = 0; i < node_count; ++i) {
    const bool is_protected = i + 1 == node_count;
    solver_cases::add_node(graph, "n" + std::to_string(i), !is_protected, 1 + rng.below(4),
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

model::EvidenceVector evidence_of(const std::vector<std::string>& sources,
                                  std::uint64_t generation = 1) {
  std::vector<model::EvidenceEntry> entries;
  for (const std::string& id : sources) {
    model::EvidenceEntry entry;
    entry.resource = model::ResourceId::unchecked(id);
    entry.state = model::EvidenceState::Present;
    entry.generation = model::EvidenceGeneration{generation};
    entries.push_back(std::move(entry));
  }
  const Result<model::EvidenceVector> built = model::EvidenceVector::build(std::move(entries));
  FCFN_CHECK_CTX(built.ok(), "evidence vector rejected: " + built.status().to_string());
  return built.value();
}

}  // namespace

FCFN_TEST(property, every_solution_satisfies_the_five_mcc1_constraints) {
  Rng rng(fcfn::test::seed() ^ 0x2b8e44d1ull);
  std::size_t iterations = 500;
  std::size_t checked = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const CaseGraph graph = random_graph(rng, 5 + static_cast<std::size_t>(rng.below(6)));
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    SpecOptions options;
    options.source_count = 0;
    options.extra_sources = {"n0"};
    Instance instance(topology, solver_cases::make_spec(topology, options));
    const ContainmentProblem& problem = instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                solver_cases::describe_instance(problem);

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    budget.max_iterations = 200000;
    const CutSolution exact = engine::solve_exact(problem, budget);
    const CutSolution heuristic = engine::solve_heuristic(problem, budget);
    const CutSolution dispatched = engine::solve(problem, budget, 4);
    for (const CutSolution* solution : {&exact, &heuristic, &dispatched}) {
      if (!solution->has_solution()) {
        continue;
      }
      ++checked;
      const ConstraintReport report = check_constraints(problem, solution->members);
      FCFN_CHECK_CTX(report.ok(), context + " (" + report.describe() + ")\n  " +
                                     solver_cases::describe_solution(problem, *solution));
      FCFN_CHECK_CTX(engine::verify_cut(problem, solution->members).ok(),
                     context + " (verify_cut disagrees with the independent check)");
    }
  }
  std::printf("five_constraints iterations=%zu solutions_checked=%zu\n", iterations, checked);
  FCFN_CHECK_CTX(checked > 500, "expected a large population of returned solutions");
}

FCFN_TEST(property, repeated_solves_are_identical) {
  Rng rng(fcfn::test::seed() ^ 0x51d9c3a7ull);
  std::size_t iterations = 400;
  for (std::size_t i = 0; i < iterations; ++i) {
    const CaseGraph graph = random_graph(rng, 5 + static_cast<std::size_t>(rng.below(6)));
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    SpecOptions options;
    options.source_count = 0;
    options.extra_sources = {"n0"};
    Instance instance(topology, solver_cases::make_spec(topology, options));
    const ContainmentProblem& problem = instance.problem();
    const std::string context = "iteration=" + std::to_string(i) + " " +
                                solver_cases::describe_instance(problem);

    SolveBudget budget;
    budget.max_explored_nodes = 200000;
    const CutSolution first = engine::solve_exact(problem, budget);
    const CutSolution second = engine::solve_exact(problem, budget);
    FCFN_CHECK_CTX(first.status == second.status, context);
    FCFN_CHECK_CTX(first.members == second.members, context);
    FCFN_CHECK_CTX(first.objective == second.objective,
                   context + " (objective vector differs between runs)");
    FCFN_CHECK_CTX(first.objective.protected_inclusions == second.objective.protected_inclusions &&
                       first.objective.total_weight == second.objective.total_weight &&
                       first.objective.cardinality == second.objective.cardinality &&
                       first.objective.signature == second.objective.signature,
                   context + " (objective fields differ between runs)");
    FCFN_CHECK_CTX(first.counters.explored_nodes == second.counters.explored_nodes,
                   context + " (search accounting differs between runs)");
  }
}

FCFN_TEST(property, plan_digest_is_stable_and_covers_every_decision) {
  Rng rng(fcfn::test::seed() ^ 0x7e3b1a55ull);
  std::size_t iterations = 60;
  std::size_t containment_claims = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const CaseGraph graph = random_graph(rng, 6 + static_cast<std::size_t>(rng.below(4)));
    const model::Topology topology = require_topology(solver_cases::to_spec(graph));
    const engine::PropagationGraph propagation = engine::PropagationGraph::build(topology);
    const model::EvidenceVector evidence = evidence_of({"n0"});
    const model::ContainmentPolicy policy = model::default_policy();
    const std::string context = "iteration=" + std::to_string(i) + " nodes=" +
                                std::to_string(topology.node_count());

    engine::PlanInputs inputs;
    inputs.topology = &topology;
    inputs.graph = &propagation;
    inputs.evidence = &evidence;
    inputs.policy = &policy;
    inputs.freshness.require_current_boot_confirmation = false;
    inputs.boundary_generation = model::BoundaryGeneration{1};

    const model::AuthorityVector authority;
    const model::PlanGeneration generation{7};
    const Result<engine::PlanComputation> first =
        engine::compute_plan(inputs, authority, generation);
    const Result<engine::PlanComputation> second =
        engine::compute_plan(inputs, authority, generation);
    FCFN_CHECK_CTX(first.ok() && second.ok(), context + " (planner refused the request)");
    const model::ContainmentPlan& plan = first.value().plan;
    FCFN_CHECK_CTX(plan.digest() == plan.compute_digest(),
                   context + " (sealed digest is not reproducible)");
    FCFN_CHECK_CTX(plan.digest() == second.value().plan.digest(),
                   context + " (two identical computations produced different digests)");
    FCFN_CHECK_CTX(!plan.digest().is_zero(), context + " (digest is zero)");
    FCFN_CHECK_CTX(plan.member_count == plan.boundary.size(), context);
    FCFN_CHECK_CTX(plan.instance_digest == first.value().problem.instance_digest(), context);
    FCFN_CHECK_CTX(plan.failure_sources.size() == 1, context + " (failure sources are not bound)");

    if (plan.claim == model::ContainmentClaim::ProvenContainment) {
      ++containment_claims;
      FCFN_CHECK_CTX(plan.optimality == model::OptimalityStatus::ProvenOptimal, context);
      FCFN_CHECK_CTX(plan.feasibility == model::FeasibilityStatus::ProvenFeasible, context);
      std::vector<model::NodeIndex> boundary_members;
      for (const model::BoundaryMember& member : plan.boundary.members()) {
        const auto index = topology.find(member.resource);
        FCFN_CHECK_CTX(index.has_value(), context + " (boundary member is not in the topology)");
        boundary_members.push_back(*index);
      }
      const ConstraintReport boundary_report =
          check_constraints(first.value().problem, boundary_members);
      FCFN_CHECK_CTX(boundary_report.ok(),
                     context + " (a containment claim was made for an invalid boundary: " +
                         boundary_report.describe() + ")");
    }

    // A policy change must change the digest: the decision is bound to it.
    model::ContainmentPolicy changed = policy;
    changed.max_boundary_weight = policy.max_boundary_weight - 1;
    engine::PlanInputs changed_inputs = inputs;
    changed_inputs.policy = &changed;
    const Result<engine::PlanComputation> third =
        engine::compute_plan(changed_inputs, authority, generation);
    FCFN_CHECK_CTX(third.ok(), context);
    FCFN_CHECK_CTX(third.value().plan.digest() != plan.digest(),
                   context + " (the digest does not bind the policy)");
  }
  std::printf("plan_digest iterations=%zu containment_claims=%zu\n", iterations, containment_claims);
  FCFN_CHECK_CTX(containment_claims > iterations / 4,
                 "expected a population of proven containment claims");
}

FCFN_TEST(property, plans_bind_the_generations_that_made_them_legal) {
  Rng rng(fcfn::test::seed() ^ 0x6f1c3d27ull);
  std::size_t iterations = 40;
  std::size_t bindings = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    const CaseGraph graph = random_graph(rng, 6 + static_cast<std::size_t>(rng.below(3)));
    const model::TopologySpec base_spec = solver_cases::to_spec(graph);
    const model::Topology topology = require_topology(base_spec);
    const engine::PropagationGraph propagation = engine::PropagationGraph::build(topology);
    const model::EvidenceVector evidence = evidence_of({"n0"});
    const model::ContainmentPolicy policy = model::default_policy();
    const std::string context = "iteration=" + std::to_string(i) + " nodes=" +
                                std::to_string(topology.node_count());

    engine::PlanInputs inputs;
    inputs.topology = &topology;
    inputs.graph = &propagation;
    inputs.evidence = &evidence;
    inputs.policy = &policy;
    inputs.freshness.require_current_boot_confirmation = false;
    inputs.boundary_generation = model::BoundaryGeneration{1};

    model::AuthorityVector authority;
    authority.epoch = model::CoordinatorEpoch{4};
    authority.topology = topology.generation();
    authority.policy = policy.generation;

    const Result<engine::PlanComputation> computed =
        engine::compute_plan(inputs, authority, model::PlanGeneration{1});
    FCFN_CHECK_CTX(computed.ok(), context);
    const model::ContainmentPlan& plan = computed.value().plan;
    FCFN_CHECK_CTX(plan.topology_digest == topology.definition_digest(), context);
    FCFN_CHECK_CTX(plan.policy_digest == policy.digest(), context);
    FCFN_CHECK_CTX(plan.evidence_digest == evidence.digest(), context);
    FCFN_CHECK_CTX(plan.authority.digest() == authority.digest(), context);
    FCFN_CHECK_CTX(plan.instance_digest == computed.value().problem.instance_digest(), context);

    // 1. The topology generation is bound: the decision is identical, the digest
    //    is not.
    {
      model::TopologySpec moved = base_spec;
      moved.generation = model::TopologyGeneration{graph.generation + 1};
      const model::Topology other = require_topology(std::move(moved));
      const engine::PropagationGraph other_graph = engine::PropagationGraph::build(other);
      engine::PlanInputs other_inputs = inputs;
      other_inputs.topology = &other;
      other_inputs.graph = &other_graph;
      const Result<engine::PlanComputation> recomputed =
          engine::compute_plan(other_inputs, authority, model::PlanGeneration{1});
      FCFN_CHECK_CTX(recomputed.ok(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.topology_digest != plan.topology_digest,
                     context + " (the topology generation is not bound)");
      FCFN_CHECK_CTX(recomputed.value().plan.digest() != plan.digest(),
                     context + " (the plan digest ignores the topology generation)");
      FCFN_CHECK_CTX(recomputed.value().plan.boundary.same_members(plan.boundary),
                     context + " (the decision changed with the topology generation)");
      ++bindings;
    }
    // 2. The policy generation is bound.
    {
      model::ContainmentPolicy other_policy = policy;
      other_policy.generation = model::PolicyGeneration{policy.generation.value() + 1};
      engine::PlanInputs other_inputs = inputs;
      other_inputs.policy = &other_policy;
      const Result<engine::PlanComputation> recomputed =
          engine::compute_plan(other_inputs, authority, model::PlanGeneration{1});
      FCFN_CHECK_CTX(recomputed.ok(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.policy_digest != plan.policy_digest,
                     context + " (the policy generation is not bound)");
      FCFN_CHECK_CTX(recomputed.value().plan.digest() != plan.digest(),
                     context + " (the plan digest ignores the policy generation)");
      FCFN_CHECK_CTX(recomputed.value().plan.boundary.same_members(plan.boundary),
                     context + " (the decision changed with the policy generation)");
      ++bindings;
    }
    // 3. The evidence generation is bound.
    {
      const model::EvidenceVector other_evidence = evidence_of({"n0"}, 2);
      engine::PlanInputs other_inputs = inputs;
      other_inputs.evidence = &other_evidence;
      const Result<engine::PlanComputation> recomputed =
          engine::compute_plan(other_inputs, authority, model::PlanGeneration{1});
      FCFN_CHECK_CTX(recomputed.ok(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.evidence_digest != plan.evidence_digest,
                     context + " (the evidence generation is not bound)");
      FCFN_CHECK_CTX(recomputed.value().plan.digest() != plan.digest(),
                     context + " (the plan digest ignores the evidence generation)");
      ++bindings;
    }
    // 4. The authority vector is bound.
    {
      model::AuthorityVector other_authority = authority;
      other_authority.epoch = model::CoordinatorEpoch{authority.epoch.value() + 1};
      const Result<engine::PlanComputation> recomputed =
          engine::compute_plan(inputs, other_authority, model::PlanGeneration{1});
      FCFN_CHECK_CTX(recomputed.ok(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.authority.digest() != plan.authority.digest(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.digest() != plan.digest(),
                     context + " (the plan digest ignores the authority vector)");
      FCFN_CHECK_CTX(recomputed.value().plan.boundary.same_members(plan.boundary),
                     context + " (the decision changed with the authority vector)");
      ++bindings;
    }
    // 5. The boundary generation is bound.
    {
      engine::PlanInputs other_inputs = inputs;
      other_inputs.boundary_generation = model::BoundaryGeneration{2};
      const Result<engine::PlanComputation> recomputed =
          engine::compute_plan(other_inputs, authority, model::PlanGeneration{1});
      FCFN_CHECK_CTX(recomputed.ok(), context);
      FCFN_CHECK_CTX(recomputed.value().plan.boundary.generation() != plan.boundary.generation(),
                     context);
      FCFN_CHECK_CTX(recomputed.value().plan.digest() != plan.digest(),
                     context + " (the plan digest ignores the boundary generation)");
      ++bindings;
    }
  }
  std::printf("generation_binding iterations=%zu bindings=%zu\n", iterations, bindings);
  FCFN_CHECK_CTX(bindings == iterations * 5, "expected five generation bindings per instance");
}

}  // namespace fcfn::test::property_cases
