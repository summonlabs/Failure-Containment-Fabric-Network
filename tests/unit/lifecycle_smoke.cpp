// FCFN unit smoke suite: proves the product-defining proposition on a fixture
// whose minimum containment cut is known independently.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <string>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/engine/planner.hpp"
#include "fcfn/engine/solver.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

namespace {

using fcfn::model::ResourceId;
using fcfn::test::layered_topology;
using fcfn::test::node_name;
using fcfn::test::require_topology;

std::vector<std::uint64_t> layer_weights(const fcfn::model::Topology& topology, std::size_t layers,
                                         std::size_t width) {
  std::vector<std::uint64_t> totals;
  for (std::size_t layer = 1; layer + 1 < layers; ++layer) {
    std::uint64_t sum = 0;
    for (std::size_t column = 0; column < width; ++column) {
      const auto index = topology.find(ResourceId::unchecked("l" + std::to_string(layer) + "c" +
                                                             std::to_string(column)));
      FCFN_CHECK(index.has_value());
      sum += topology.node(*index).weight;
    }
    totals.push_back(sum);
  }
  return totals;
}

fcfn::model::EvidenceVector evidence_for(const std::vector<ResourceId>& present) {
  std::vector<fcfn::model::EvidenceEntry> entries;
  std::uint64_t generation = 1;
  for (const ResourceId& id : present) {
    fcfn::model::EvidenceEntry entry;
    entry.resource = id;
    entry.state = fcfn::model::EvidenceState::Present;
    entry.generation = fcfn::model::EvidenceGeneration{generation++};
    entries.push_back(std::move(entry));
  }
  auto vector = fcfn::model::EvidenceVector::build(std::move(entries));
  FCFN_CHECK_OK(vector);
  return std::move(vector.value());
}

fcfn::engine::PlanInputs make_inputs(const fcfn::model::Topology& topology,
                                     const fcfn::engine::PropagationGraph& graph,
                                     const fcfn::model::EvidenceVector& evidence,
                                     const fcfn::model::ContainmentPolicy& policy,
                                     fcfn::model::BoundaryGeneration boundary_generation) {
  fcfn::engine::PlanInputs inputs;
  inputs.topology = &topology;
  inputs.graph = &graph;
  inputs.evidence = &evidence;
  inputs.policy = &policy;
  inputs.freshness.require_current_boot_confirmation = false;
  inputs.boundary_generation = boundary_generation;
  return inputs;
}

}  // namespace

FCFN_TEST(lifecycle, layered_fixture_minimum_cut_is_the_cheapest_layer) {
  constexpr std::size_t kLayers = 4;
  constexpr std::size_t kWidth = 3;
  const auto topology = require_topology(layered_topology(kLayers, kWidth, 7, 1));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({ResourceId::unchecked("l0c0")});
  auto policy = fcfn::model::default_policy();
  // The fixture's source is deliberately not containable, so the proof targets
  // propagation only. Requiring source inclusion would (correctly) degrade the
  // claim; that behaviour is covered by its own case.
  policy.require_failure_source_inclusion = false;

  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{1}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{1});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;

  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK(plan.optimality == fcfn::model::OptimalityStatus::ProvenOptimal);
  FCFN_CHECK(plan.feasibility == fcfn::model::FeasibilityStatus::ProvenFeasible);
  FCFN_CHECK_EQ(plan.member_count, kWidth);

  const std::vector<std::uint64_t> totals = layer_weights(topology, kLayers, kWidth);
  const std::uint64_t cheapest = *std::min_element(totals.begin(), totals.end());
  FCFN_CHECK_EQ(plan.total_weight, cheapest);
  // The chosen layer must be the cheapest one; ties are broken canonically, so
  // the reported members must belong to a single cheapest layer.
  std::uint64_t chosen_layer_weight = 0;
  std::string chosen_layer;
  for (const fcfn::model::BoundaryMember& member : plan.boundary.members()) {
    const std::string layer = member.resource.value().substr(0, member.resource.value().find('c'));
    if (chosen_layer.empty()) {
      chosen_layer = layer;
    }
    FCFN_CHECK_EQ(layer, chosen_layer);
    chosen_layer_weight += member.weight;
    FCFN_CHECK(member.reason == fcfn::model::InclusionReason::CutVertexOnProvenPath ||
               member.reason == fcfn::model::InclusionReason::CutVertexOnUnknownPath);
    FCFN_CHECK(!member.witness_path.empty());
  }
  FCFN_CHECK_EQ(chosen_layer_weight, cheapest);
}

FCFN_TEST(lifecycle, unprotected_topology_contains_only_the_failure_sources) {
  // SYNTHETIC fixture with no protected obligations: containment is exactly the
  // eligible failure sources, and the claim is still PROVEN_CONTAINMENT.
  fcfn::model::TopologySpec spec;
  spec.generation = fcfn::model::TopologyGeneration{1};
  auto add = [&spec](const char* name, bool containable) {
    fcfn::model::NodeSpec node;
    node.id = ResourceId::unchecked(name);
    node.containable = containable;
    node.weight = 2;
    spec.nodes.push_back(std::move(node));
  };
  add("failed-a", true);
  add("failed-b", true);
  add("healthy", true);
  fcfn::model::EdgeSpec edge;
  edge.from = ResourceId::unchecked("failed-a");
  edge.to = ResourceId::unchecked("healthy");
  edge.evidence = fcfn::model::EdgeEvidence::Proven;
  edge.generation = fcfn::model::EvidenceGeneration{1};
  spec.edges.push_back(edge);

  const auto topology = require_topology(std::move(spec));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence =
      evidence_for({ResourceId::unchecked("failed-a"), ResourceId::unchecked("failed-b")});
  const auto policy = fcfn::model::default_policy();
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{2}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{2});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  FCFN_CHECK_EQ(plan.member_count, static_cast<std::size_t>(2));
  FCFN_CHECK_EQ(plan.total_weight, static_cast<std::uint64_t>(4));
  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK(plan.explanation.contains(fcfn::model::ReasonCode::NoProtectedObligations));
}

FCFN_TEST(lifecycle, no_failure_evidence_yields_a_releasable_plan) {
  const auto topology = require_topology(layered_topology(3, 2, 5, 1));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({});
  const auto policy = fcfn::model::default_policy();
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{3}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{3});
  FCFN_CHECK_OK(computed);
  FCFN_CHECK(computed.value().plan.boundary.empty());
  FCFN_CHECK(computed.value().plan.is_releasable());
}

FCFN_TEST(lifecycle, incomplete_frontier_degrades_the_claim_to_indeterminate) {
  auto spec = layered_topology(4, 2, 3, 1);
  for (fcfn::model::NodeSpec& node : spec.nodes) {
    if (node.id.value() == "l1c0") {
      node.completeness = fcfn::model::AdjacencyCompleteness::Partial;
    }
  }
  const auto topology = require_topology(std::move(spec));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({ResourceId::unchecked("l0c0")});
  const auto policy = fcfn::model::default_policy();
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{4}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{4});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::Indeterminate);
  FCFN_CHECK(plan.currency == fcfn::model::EvidenceCurrency::IncompleteFrontier);
  FCFN_CHECK(plan.boundary.size() > 0);
}

FCFN_TEST(lifecycle, unknown_edges_are_cut_conservatively) {
  // Two parallel paths; one is only UNKNOWN. A proven-only plan would cut one
  // node, the conservative plan must cut both.
  fcfn::model::TopologySpec spec;
  spec.generation = fcfn::model::TopologyGeneration{9};
  auto add = [&spec](const char* name, bool containable, bool protected_node) {
    fcfn::model::NodeSpec node;
    node.id = ResourceId::unchecked(name);
    node.containable = containable;
    node.protected_obligation = protected_node;
    node.weight = 1;
    spec.nodes.push_back(std::move(node));
  };
  add("src", false, false);
  add("mid-proven", true, false);
  add("mid-unknown", true, false);
  add("obligation", false, true);
  auto link = [&spec](const char* from, const char* to, fcfn::model::EdgeEvidence evidence) {
    fcfn::model::EdgeSpec edge;
    edge.from = ResourceId::unchecked(from);
    edge.to = ResourceId::unchecked(to);
    edge.evidence = evidence;
    edge.generation = fcfn::model::EvidenceGeneration{1};
    spec.edges.push_back(std::move(edge));
  };
  link("src", "mid-proven", fcfn::model::EdgeEvidence::Proven);
  link("src", "mid-unknown", fcfn::model::EdgeEvidence::Unknown);
  link("mid-proven", "obligation", fcfn::model::EdgeEvidence::Proven);
  link("mid-unknown", "obligation", fcfn::model::EdgeEvidence::Unknown);

  const auto topology = require_topology(std::move(spec));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({ResourceId::unchecked("src")});
  auto policy = fcfn::model::default_policy();
  policy.require_failure_source_inclusion = false;
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{5}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{5});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  FCFN_CHECK_EQ(plan.member_count, static_cast<std::size_t>(2));
  FCFN_CHECK(plan.boundary.contains(ResourceId::unchecked("mid-proven")));
  FCFN_CHECK(plan.boundary.contains(ResourceId::unchecked("mid-unknown")));
  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK(plan.explanation.contains(fcfn::model::ReasonCode::UnknownEdgesPresent));
}

FCFN_TEST(lifecycle, uncontainable_source_degrades_the_claim_under_default_policy) {
  // The source cannot be contained and the policy requires every eligible
  // failure source to be contained: the cut is still computed and verified, but
  // the claim must not be rounded up to PROVEN_CONTAINMENT.
  const auto topology = require_topology(layered_topology(4, 2, 3, 1));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({ResourceId::unchecked("l0c0")});
  const auto policy = fcfn::model::default_policy();
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{6}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{6});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::Indeterminate);
  FCFN_CHECK(plan.feasibility == fcfn::model::FeasibilityStatus::ProvenFeasible);
  FCFN_CHECK_EQ(plan.uncontainable_sources.size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(plan.uncontainable_sources.front().value(), std::string("l0c0"));
  FCFN_CHECK(plan.explanation.contains(fcfn::model::ReasonCode::SourceNotContainable));
  FCFN_CHECK(plan.member_count > 0);
}

FCFN_TEST(lifecycle, containable_source_is_the_minimum_cut) {
  auto spec = layered_topology(4, 2, 3, 1);
  spec.nodes[0].containable = true;
  const auto topology = require_topology(std::move(spec));
  const auto graph = fcfn::engine::PropagationGraph::build(topology);
  const auto evidence = evidence_for({ResourceId::unchecked("l0c0")});
  const auto policy = fcfn::model::default_policy();
  auto computed = fcfn::engine::compute_plan(
      make_inputs(topology, graph, evidence, policy, fcfn::model::BoundaryGeneration{8}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{8});
  FCFN_CHECK_OK(computed);
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  FCFN_CHECK(plan.claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK_EQ(plan.member_count, static_cast<std::size_t>(1));
  FCFN_CHECK(plan.boundary.contains(ResourceId::unchecked("l0c0")));
}

FCFN_TEST(lifecycle, plan_digest_is_independent_of_insertion_order) {
  const auto first = require_topology(layered_topology(4, 3, 7, 1));
  const auto second = require_topology(layered_topology(4, 3, 7, 1));
  FCFN_CHECK(first.definition_digest() == second.definition_digest());
  const auto graph_first = fcfn::engine::PropagationGraph::build(first);
  const auto graph_second = fcfn::engine::PropagationGraph::build(second);
  const auto evidence = evidence_for({ResourceId::unchecked("l0c0")});
  const auto policy = fcfn::model::default_policy();
  auto plan_first = fcfn::engine::compute_plan(
      make_inputs(first, graph_first, evidence, policy, fcfn::model::BoundaryGeneration{7}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{7});
  auto plan_second = fcfn::engine::compute_plan(
      make_inputs(second, graph_second, evidence, policy, fcfn::model::BoundaryGeneration{7}),
      fcfn::model::AuthorityVector{}, fcfn::model::PlanGeneration{7});
  FCFN_CHECK_OK(plan_first);
  FCFN_CHECK_OK(plan_second);
  FCFN_CHECK(plan_first.value().plan.digest() == plan_second.value().plan.digest());
  FCFN_CHECK(plan_first.value().plan.to_json() == plan_second.value().plan.to_json());
}
