// FCFN unit suite: canonical round trips for every model document.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "fcfn/engine/graph.hpp"
#include "fcfn/engine/planner.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

namespace {

using fcfn::model::ResourceId;

fcfn::model::Topology sample_topology() {
  fcfn::model::TopologySpec spec;
  spec.generation = fcfn::model::TopologyGeneration{4};
  auto node = [&spec](const char* id, bool containable, std::uint64_t weight, bool obligation) {
    fcfn::model::NodeSpec entry;
    entry.id = ResourceId::unchecked(id);
    entry.containable = containable;
    entry.weight = weight;
    entry.protected_obligation = obligation;
    spec.nodes.push_back(std::move(entry));
  };
  node("src", true, 3, false);
  node("mid-a", true, 2, false);
  node("mid-b", true, 5, false);
  node("obligation", false, 1, true);
  auto edge = [&spec](const char* from, const char* to, fcfn::model::EdgeEvidence evidence) {
    fcfn::model::EdgeSpec entry;
    entry.from = ResourceId::unchecked(from);
    entry.to = ResourceId::unchecked(to);
    entry.evidence = evidence;
    entry.generation = fcfn::model::EvidenceGeneration{2};
    spec.edges.push_back(std::move(entry));
  };
  edge("src", "mid-a", fcfn::model::EdgeEvidence::Proven);
  edge("mid-a", "obligation", fcfn::model::EdgeEvidence::Proven);
  edge("src", "mid-b", fcfn::model::EdgeEvidence::Unknown);
  edge("mid-b", "obligation", fcfn::model::EdgeEvidence::Unknown);
  return fcfn::test::require_topology(std::move(spec));
}

fcfn::model::EvidenceVector sample_evidence() {
  std::vector<fcfn::model::EvidenceEntry> entries;
  fcfn::model::EvidenceEntry entry;
  entry.resource = ResourceId::unchecked("src");
  entry.state = fcfn::model::EvidenceState::Present;
  entry.generation = fcfn::model::EvidenceGeneration{7};
  entry.source_digest = fcfn::Digest{0x0123456789abcdefull, 0xfedcba9876543210ull};
  entries.push_back(std::move(entry));
  auto vector = fcfn::model::EvidenceVector::build(std::move(entries));
  FCFN_CHECK_OK(vector);
  return std::move(vector.value());
}

fcfn::model::ContainmentPlan sample_plan() {
  const fcfn::model::Topology topology = sample_topology();
  const fcfn::engine::PropagationGraph graph = fcfn::engine::PropagationGraph::build(topology);
  const fcfn::model::EvidenceVector evidence = sample_evidence();
  const fcfn::model::ContainmentPolicy policy = fcfn::model::default_policy();
  fcfn::engine::PlanInputs inputs;
  inputs.topology = &topology;
  inputs.graph = &graph;
  inputs.evidence = &evidence;
  inputs.policy = &policy;
  inputs.freshness.require_current_boot_confirmation = false;
  inputs.boundary_generation = fcfn::model::BoundaryGeneration{11};
  fcfn::model::AuthorityVector authority;
  authority.epoch = fcfn::CoordinatorEpoch{3};
  authority.boot = fcfn::BootIdentity{fcfn::BootId{0x1111222233334444ull}, 4242};
  authority.policy = fcfn::model::PolicyGeneration{1};
  authority.topology = fcfn::model::TopologyGeneration{4};
  authority.evidence_revision = fcfn::model::EvidenceRevision{2};
  authority.evidence_digest = evidence.digest();
  authority.issued_sequence = fcfn::Sequence{9};
  auto computed = fcfn::engine::compute_plan(inputs, authority, fcfn::model::PlanGeneration{5});
  FCFN_CHECK_OK(computed);
  return std::move(computed.value().plan);
}

}  // namespace

FCFN_TEST(codec, plan_round_trip_is_exact) {
  const fcfn::model::ContainmentPlan plan = sample_plan();
  const std::vector<std::byte> encoded = plan.encode();
  auto decoded = fcfn::model::ContainmentPlan::decode(
      std::span<const std::byte>(encoded.data(), encoded.size()));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == plan.digest());
  FCFN_CHECK(decoded.value().boundary.digest() == plan.boundary.digest());
  FCFN_CHECK_EQ(decoded.value().member_count, plan.member_count);
  FCFN_CHECK_EQ(decoded.value().total_weight, plan.total_weight);
  FCFN_CHECK(decoded.value().claim == plan.claim);
  FCFN_CHECK(decoded.value().to_json() == plan.to_json());
  FCFN_CHECK(decoded.value().encode() == encoded);
}

FCFN_TEST(codec, boundary_round_trip_is_exact) {
  const fcfn::model::ContainmentPlan plan = sample_plan();
  const std::vector<std::byte> encoded = plan.boundary.encode();
  auto decoded = fcfn::model::ContainmentBoundary::decode(
      std::span<const std::byte>(encoded.data(), encoded.size()));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == plan.boundary.digest());
  FCFN_CHECK(decoded.value().to_json() == plan.boundary.to_json());
}

FCFN_TEST(codec, every_truncation_prefix_of_a_plan_is_refused) {
  const fcfn::model::ContainmentPlan plan = sample_plan();
  const std::vector<std::byte> encoded = plan.encode();
  FCFN_CHECK(encoded.size() > 8);
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    auto decoded = fcfn::model::ContainmentPlan::decode(
        std::span<const std::byte>(encoded.data(), length));
    if (decoded.ok()) {
      throw fcfn::test::TestFailure("truncated plan at length " + std::to_string(length) +
                                    " decoded successfully");
    }
  }
}

FCFN_TEST(codec, topology_and_evidence_round_trip) {
  const fcfn::model::Topology topology = sample_topology();
  const std::vector<std::byte> encoded = topology.encode();
  auto decoded =
      fcfn::model::Topology::decode(std::span<const std::byte>(encoded.data(), encoded.size()));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().definition_digest() == topology.definition_digest());

  const fcfn::model::EvidenceVector evidence = sample_evidence();
  const std::vector<std::byte> evidence_bytes = evidence.encode();
  auto decoded_evidence = fcfn::model::EvidenceVector::decode(
      std::span<const std::byte>(evidence_bytes.data(), evidence_bytes.size()));
  FCFN_CHECK_OK(decoded_evidence);
  FCFN_CHECK(decoded_evidence.value().digest() == evidence.digest());
}

FCFN_TEST(codec, trailing_bytes_are_refused) {
  const fcfn::model::Topology topology = sample_topology();
  std::vector<std::byte> encoded = topology.encode();
  encoded.push_back(std::byte{0x7f});
  auto decoded =
      fcfn::model::Topology::decode(std::span<const std::byte>(encoded.data(), encoded.size()));
  FCFN_CHECK_STATUS(decoded, fcfn::StatusCode::TrailingGarbage);
}
