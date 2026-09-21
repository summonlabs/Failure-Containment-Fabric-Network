// FCFN adversarial suite: hostile identifiers and definitions.
//
// Product proposition proved here: identities and definitions that are outside
// the canonical domain can never be laundered into a containment claim. An
// identifier that the canonical codec refuses is refused at the domain gate; a
// source that is not a node of the known topology degrades every claim to
// INDETERMINATE; and a definition that the durable codec cannot decode is
// reported here as a defect at the exact point where it silently disappears.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adversarial_support.hpp"

namespace {

using namespace fcfn::test::adversarial;

using fcfn::model::AdjacencyCompleteness;
using fcfn::model::ContainmentClaim;
using fcfn::model::EdgeEvidence;
using fcfn::model::EdgeSpec;
using fcfn::model::EvidenceCurrency;
using fcfn::model::EvidenceGeneration;
using fcfn::model::NodeSpec;
using fcfn::model::PolicyGeneration;
using fcfn::model::ReasonCode;
using fcfn::model::Topology;
using fcfn::model::TopologyGeneration;
using fcfn::runtime::PlanRequest;

/// Render an identifier so control bytes and separators are visible on failure.
std::string describe_id(const std::string& id) {
  static const char* kDigits = "0123456789abcdef";
  std::string out = "'";
  for (const char raw : id) {
    const auto value = static_cast<unsigned char>(raw);
    if (value >= 0x20 && value < 0x7f) {
      out.push_back(static_cast<char>(value));
    } else {
      out.push_back('\\');
      out.push_back('x');
      out.push_back(kDigits[value >> 4]);
      out.push_back(kDigits[value & 0x0f]);
    }
  }
  out.push_back('\'');
  return out;
}

TopologySpec single_node(std::uint64_t generation, const char* id, std::uint64_t weight = 1) {
  TopologySpec spec;
  spec.generation = TopologyGeneration{generation};
  NodeSpec node;
  node.id = ResourceId::unchecked(id);
  node.containable = true;
  node.weight = weight;
  spec.nodes.push_back(std::move(node));
  return spec;
}

TopologySpec two_nodes(std::uint64_t generation) {
  TopologySpec spec;
  spec.generation = TopologyGeneration{generation};
  for (const char* id : {"alpha", "beta"}) {
    NodeSpec node;
    node.id = ResourceId::unchecked(id);
    node.containable = true;
    node.weight = 1;
    spec.nodes.push_back(std::move(node));
  }
  return spec;
}

void add_edge(TopologySpec& spec, const char* from, const char* to, std::uint64_t generation) {
  EdgeSpec edge;
  edge.from = ResourceId::unchecked(from);
  edge.to = ResourceId::unchecked(to);
  edge.evidence = EdgeEvidence::Proven;
  edge.generation = EvidenceGeneration{generation};
  spec.edges.push_back(std::move(edge));
}

}  // namespace

FCFN_TEST(adversarial, the_canonical_resource_id_gate_refuses_every_hostile_form) {
  for (const std::string& id : hostile_ids()) {
    const Result<ResourceId> parsed = ResourceId::parse(id);
    if (parsed.ok()) {
      fail_here("ResourceId::parse", "accepted " + describe_id(id));
    }
    if (fcfn::is_valid_resource_id(id)) {
      fail_here("is_valid_resource_id", "accepted " + describe_id(id));
    }
  }
  // Control: the canonical form itself is accepted, so the gate is not vacuous.
  const Result<ResourceId> ordinary = ResourceId::parse("rack-1.leaf_2:port3");
  FCFN_CHECK_OK(ordinary);
  FCFN_CHECK_EQ(ordinary.value().value(), std::string("rack-1.leaf_2:port3"));
  FCFN_CHECK(fcfn::is_valid_resource_id("l0c0"));
}

FCFN_TEST(adversarial, a_hostile_identifier_never_becomes_a_containment_claim) {
  fcfn::ManualClock clock(1000);
  for (const std::string& id : non_empty_hostile_ids()) {
    const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
    const Result<fcfn::model::DetectionRecord> recorded =
        runtime->record_detection(observation(id.c_str(), EvidenceState::Present, 1, 1, 2));
    // DEFECT (reported): EvidenceVector::build validates only emptiness
    // (src/model/evidence.cpp:67) and Topology::build validates only emptiness
    // and duplicates (src/model/topology.cpp:99-110), so an identifier outside the
    // canonical domain is accepted in memory even though the canonical codec
    // refuses it. The safe property - such a source can never appear in a
    // containment claim - is asserted below.
    if (!recorded.ok()) {
      expect_code(("ingestion of " + describe_id(id)).c_str(), recorded.status().code(),
                  StatusCode::InvalidArgument);
      continue;
    }
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    const Result<ContainmentPlan> planned = runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    if (planned.value().claim != ContainmentClaim::Indeterminate) {
      fail_here("hostile source",
                describe_id(id) + " produced claim " +
                    fcfn::model::to_string(planned.value().claim) + " instead of indeterminate");
    }
    FCFN_CHECK(planned.value().boundary.empty());
    FCFN_CHECK(planned.value().currency == EvidenceCurrency::IncompleteFrontier);
    if (!planned.value().explanation.contains(ReasonCode::FailureSourceNotInTopology)) {
      fail_here("hostile source", describe_id(id) + " was not reported as outside the topology");
    }
  }
}

FCFN_TEST(adversarial, an_empty_identifier_is_refused_by_every_ingestion_path) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
  expect_result("empty detection resource",
                runtime->record_detection(observation("", EvidenceState::Present, 1, 1, 2)),
                StatusCode::InvalidArgument);
  expect_result("zero detection generation",
                runtime->record_detection(observation("l0c0", EvidenceState::Present, 0, 1, 2)),
                StatusCode::InvalidArgument);
  // A refusal left no trace: the runtime still has no evidence at all.
  FCFN_CHECK(runtime->evidence().value().empty());
  FCFN_CHECK_EQ(runtime->stats().detections_accepted, static_cast<std::uint64_t>(0));
  // The empty-resource refusal is counted as rejected; the zero-generation
  // refusal is answered before the merge is attempted at all.
  FCFN_CHECK(runtime->stats().detections_rejected >= 1);
}

FCFN_TEST(adversarial, hostile_topology_definitions_are_refused_without_mutating_state) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
  const Result<TopologyGeneration> baseline = runtime->topology_generation();
  FCFN_CHECK_OK(baseline);
  FCFN_CHECK_EQ(baseline.value().value(), static_cast<std::uint64_t>(1));

  const auto expect_refusal = [&runtime](const char* name, TopologySpec spec, StatusCode expected) {
    const Result<TopologyGeneration> result = runtime->apply_topology(std::move(spec));
    if (result.status().code() != expected) {
      fail_here(name, std::string("expected ") + to_string(expected) + " but got " +
                          result.status().to_string());
    }
    const Result<TopologyGeneration> current = runtime->topology_generation();
    if (!current.ok() || current.value().value() != 1) {
      fail_here(name, "the refused definition mutated runtime state");
    }
  };

  expect_refusal("zero-length topology", TopologySpec{TopologyGeneration{2}},
                 StatusCode::InvalidArgument);
  {
    TopologySpec spec = single_node(0, "alpha");
    expect_refusal("zero topology generation", std::move(spec), StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec;
    spec.generation = TopologyGeneration{2};
    for (int index = 0; index < 2; ++index) {
      NodeSpec node;
      node.id = ResourceId::unchecked("alpha");
      node.containable = true;
      node.weight = 1;
      spec.nodes.push_back(std::move(node));
    }
    expect_refusal("duplicate node id", std::move(spec), StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec = single_node(2, "alpha");
    add_edge(spec, "alpha", "alpha", 1);
    expect_refusal("self-referential edge", std::move(spec), StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec = two_nodes(2);
    add_edge(spec, "alpha", "missing", 1);
    expect_refusal("edge with an unknown endpoint", std::move(spec), StatusCode::NotFound);
  }
  {
    TopologySpec spec = two_nodes(2);
    add_edge(spec, "alpha", "beta", 0);
    expect_refusal("zero edge evidence generation", std::move(spec), StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec = two_nodes(2);
    add_edge(spec, "alpha", "beta", 1);
    add_edge(spec, "alpha", "beta", 2);
    expect_refusal("duplicate edge", std::move(spec), StatusCode::AlreadyExists);
  }
  {
    TopologySpec spec = single_node(2, "alpha", 0);
    expect_refusal("zero node weight", std::move(spec), StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec = single_node(2, "alpha", 0xffffffffffffffffull);
    expect_refusal("node weight at the 64-bit extreme", std::move(spec),
                   StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec = single_node(2, "alpha");
    spec.nodes[0].protected_obligation = true;
    expect_refusal("protected obligation marked containable", std::move(spec),
                   StatusCode::InvalidArgument);
  }
  {
    TopologySpec spec;
    spec.generation = TopologyGeneration{2};
    spec.nodes.reserve(kMaxTopologyNodes + 1);
    for (std::size_t index = 0; index <= kMaxTopologyNodes; ++index) {
      NodeSpec node;
      node.id = ResourceId::unchecked("n" + std::to_string(index));
      node.weight = 1;
      spec.nodes.push_back(std::move(node));
    }
    expect_refusal("node count above kMaxTopologyNodes", std::move(spec), StatusCode::LimitExceeded);
  }
  {
    TopologySpec spec = single_node(2, "alpha");
    spec.edges.resize(262145);
    expect_refusal("edge count above kMaxTopologyEdges", std::move(spec), StatusCode::LimitExceeded);
  }
  {
    TopologySpec spec = single_node(1, "alpha");
    expect_refusal("topology generation that does not advance", std::move(spec),
                   StatusCode::StaleGeneration);
  }
  // The loaded definition is untouched after every refusal above.
  const Result<TopologyGeneration> final_generation = runtime->topology_generation();
  FCFN_CHECK_OK(final_generation);
  FCFN_CHECK_EQ(final_generation.value().value(), static_cast<std::uint64_t>(1));
}

FCFN_TEST(adversarial, a_topology_outside_the_canonical_domain_is_refused_at_build_and_by_the_codec) {
  // The canonical resource-id grammar is enforced by the builder and by the
  // codec, so a definition can never be accepted in memory and then fail to be
  // read back from durable storage.
  TopologySpec spec = single_node(1, "rack/leaf");
  const Result<Topology> built = Topology::build(std::move(spec));
  FCFN_CHECK(!built.ok());
  expect_code("topology with a hostile id at build", built.status().code(),
              StatusCode::InvalidArgument);
}

FCFN_TEST(adversarial, a_runtime_refuses_to_load_a_topology_outside_the_canonical_domain) {
  const fcfn::test::TempDir dir("adv-hostile-topology");
  fcfn::ManualClock clock(1000);
  RuntimeConfig config = durable_config(dir.path());
  config.topology = single_node(1, "rack/leaf");
  // Refused at startup: an unreadable definition can never become durable state.
  const Result<std::unique_ptr<ContainmentRuntime>> runtime =
      ContainmentRuntime::open(config, &clock);
  FCFN_CHECK(!runtime.ok());
  expect_code("runtime open with a hostile topology", runtime.status().code(),
              StatusCode::InvalidArgument);
  // Nothing was committed: a restart that supplies no topology of its own finds
  // no durable definition either.
  RuntimeConfig second = durable_config(dir.path());
  second.topology = TopologySpec{};
  const std::unique_ptr<ContainmentRuntime> clean = open_or_fail(second, &clock);
  const Result<TopologyGeneration> generation = clean->topology_generation();
  FCFN_CHECK(!generation.ok());
  expect_code("topology generation after a refused load", generation.status().code(),
              StatusCode::Unsupported);
}

FCFN_TEST(adversarial, a_policy_outside_the_canonical_domain_is_refused_before_it_is_committed) {
  const fcfn::test::TempDir dir("adv-hostile-policy");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());
  {
    const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
    ContainmentPolicy hostile = fcfn::model::default_policy();
    hostile.generation = PolicyGeneration{2};
    hostile.max_boundary_members = 0;  // outside the canonical domain
    // The runtime applies the same domain rules the durable decoder enforces, so
    // a policy that could never be read back is refused before it is committed.
    const Result<PolicyGeneration> applied = runtime->apply_policy(hostile);
    FCFN_CHECK(!applied.ok());
    expect_code("hostile policy at apply", applied.status().code(), StatusCode::InvalidArgument);
    FCFN_CHECK_EQ(runtime->policy().generation.value(), static_cast<std::uint64_t>(1));
    const Result<ContainmentPolicy> decoded = ContainmentPolicy::decode(hostile.encode());
    FCFN_CHECK(!decoded.ok());
    expect_code("hostile policy through the codec", decoded.status().code(),
                StatusCode::InvalidArgument);
  }
  // The refused policy was never durable: the restart keeps the configured one.
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
  FCFN_CHECK(runtime->startup().recovered);
  FCFN_CHECK_EQ(runtime->policy().generation.value(), static_cast<std::uint64_t>(1));
}
