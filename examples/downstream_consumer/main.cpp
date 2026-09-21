// Independent FCFN consumer.
//
// This program exercises the installed package exactly as a downstream user
// would: it links fcfn::core and fcfn::runtime, computes a containment plan on a
// SYNTHETIC dependency graph, independently verifies the returned boundary, and
// drives a durable runtime through one containment decision.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/engine/planner.hpp"
#include "fcfn/engine/solver.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/version.hpp"

namespace {

fcfn::model::Topology build_topology() {
  fcfn::model::TopologySpec spec;
  spec.generation = fcfn::model::TopologyGeneration{1};
  auto add = [&spec](const char* id, bool containable, std::uint64_t weight, bool obligation) {
    fcfn::model::NodeSpec node;
    node.id = fcfn::model::ResourceId::unchecked(id);
    node.containable = containable;
    node.weight = weight;
    node.protected_obligation = obligation;
    spec.nodes.push_back(std::move(node));
  };
  add("edge-a", true, 4, false);
  add("spine-1", true, 1, false);
  add("spine-2", true, 1, false);
  add("tenant-payment", false, 1, true);
  auto link = [&spec](const char* from, const char* to) {
    fcfn::model::EdgeSpec edge;
    edge.from = fcfn::model::ResourceId::unchecked(from);
    edge.to = fcfn::model::ResourceId::unchecked(to);
    edge.evidence = fcfn::model::EdgeEvidence::Proven;
    edge.generation = fcfn::model::EvidenceGeneration{1};
    spec.edges.push_back(std::move(edge));
  };
  link("edge-a", "spine-1");
  link("spine-1", "tenant-payment");
  link("edge-a", "spine-2");
  link("spine-2", "tenant-payment");
  auto topology = fcfn::model::Topology::build(std::move(spec));
  if (!topology.ok()) {
    std::fprintf(stderr, "consumer: topology rejected: %s\n", topology.status().to_string().c_str());
    std::exit(1);
  }
  return std::move(topology.value());
}

fcfn::model::EvidenceVector build_evidence() {
  std::vector<fcfn::model::EvidenceEntry> entries;
  fcfn::model::EvidenceEntry entry;
  entry.resource = fcfn::model::ResourceId::unchecked("edge-a");
  entry.state = fcfn::model::EvidenceState::Present;
  entry.generation = fcfn::model::EvidenceGeneration{1};
  entries.push_back(std::move(entry));
  auto vector = fcfn::model::EvidenceVector::build(std::move(entries));
  if (!vector.ok()) {
    std::fprintf(stderr, "consumer: evidence rejected\n");
    std::exit(1);
  }
  return std::move(vector.value());
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("{\"consumer\":\"fcfn\",\"library_version\":\"%s\",\"abi\":%u}\n", fcfn::kVersionString,
              static_cast<unsigned>(fcfn::kAbiVersion));

  const fcfn::model::Topology topology = build_topology();
  const fcfn::engine::PropagationGraph graph = fcfn::engine::PropagationGraph::build(topology);
  const fcfn::model::EvidenceVector evidence = build_evidence();
  fcfn::model::ContainmentPolicy policy = fcfn::model::default_policy();

  fcfn::engine::PlanInputs inputs;
  inputs.topology = &topology;
  inputs.graph = &graph;
  inputs.evidence = &evidence;
  inputs.policy = &policy;
  inputs.freshness.require_current_boot_confirmation = false;
  inputs.boundary_generation = fcfn::model::BoundaryGeneration{1};

  auto computed = fcfn::engine::compute_plan(inputs, fcfn::model::AuthorityVector{},
                                             fcfn::model::PlanGeneration{1});
  if (!computed.ok()) {
    std::fprintf(stderr, "consumer: planning failed: %s\n", computed.status().to_string().c_str());
    return 1;
  }
  const fcfn::model::ContainmentPlan& plan = computed.value().plan;
  std::printf("{\"claim\":\"%s\",\"members\":%zu,\"weight\":%llu}\n",
              fcfn::model::to_string(plan.claim), plan.member_count,
              static_cast<unsigned long long>(plan.total_weight));
  if (plan.claim != fcfn::model::ContainmentClaim::ProvenContainment) {
    std::fprintf(stderr, "consumer: expected proven containment\n");
    return 1;
  }

  // Independent verification of the produced boundary using the public
  // verifier, not the solver's own bookkeeping.
  fcfn::engine::ContainmentInstanceSpec spec;
  spec.failure_sources = plan.failure_sources;
  spec.protected_obligations = plan.protected_obligations;
  auto problem = fcfn::engine::ContainmentProblem::build(graph, spec);
  if (!problem.ok()) {
    std::fprintf(stderr, "consumer: instance rejected\n");
    return 1;
  }
  std::vector<fcfn::model::NodeIndex> members;
  for (const fcfn::model::BoundaryMember& member : plan.boundary.members()) {
    const auto index = topology.find(member.resource);
    if (!index.has_value()) {
      std::fprintf(stderr, "consumer: boundary names an unknown resource\n");
      return 1;
    }
    members.push_back(*index);
  }
  const auto objective = fcfn::engine::verify_cut(problem.value(), members);
  if (!objective.ok()) {
    std::fprintf(stderr, "consumer: boundary rejected: %s\n", objective.status().to_string().c_str());
    return 1;
  }
  std::printf("{\"verified\":true,\"objective_weight\":%llu}\n",
              static_cast<unsigned long long>(objective.value().total_weight));

  // Drive a real runtime with durable state, then check the apply protocol.
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "fcfn-consumer-store";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  fcfn::ManualClock clock;
  fcfn::runtime::RuntimeConfig config;
  config.store_root = root;
  config.topology = fcfn::model::TopologySpec{};
  auto opened = fcfn::runtime::ContainmentRuntime::open(config, &clock);
  if (!opened.ok()) {
    std::fprintf(stderr, "consumer: runtime open failed: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  auto applied = opened.value()->apply_topology([&topology]() {
    fcfn::model::TopologySpec spec_out;
    spec_out.generation = topology.generation();
    for (std::size_t i = 0; i < topology.node_count(); ++i) {
      spec_out.nodes.push_back(topology.node(static_cast<fcfn::model::NodeIndex>(i)));
    }
    for (std::size_t i = 0; i < topology.edge_count(); ++i) {
      const fcfn::model::Topology::Edge& edge =
          topology.edge_by_index(static_cast<fcfn::model::EdgeIndex>(i));
      fcfn::model::EdgeSpec edge_spec;
      edge_spec.from = topology.resource(edge.from);
      edge_spec.to = topology.resource(edge.to);
      edge_spec.evidence = edge.evidence;
      edge_spec.generation = edge.generation;
      spec_out.edges.push_back(std::move(edge_spec));
    }
    return spec_out;
  }());
  if (!applied.ok()) {
    std::fprintf(stderr, "consumer: apply_topology failed: %s\n", applied.status().to_string().c_str());
    return 1;
  }
  fcfn::model::FailureObservation observation;
  observation.resource = fcfn::model::ResourceId::unchecked("edge-a");
  observation.state = fcfn::model::EvidenceState::Present;
  observation.generation = fcfn::model::EvidenceGeneration{1};
  if (!opened.value()->record_detection(observation).ok()) {
    std::fprintf(stderr, "consumer: detection rejected\n");
    return 1;
  }
  auto authorization = opened.value()->authorize();
  if (!authorization.ok()) {
    std::fprintf(stderr, "consumer: authorization failed\n");
    return 1;
  }
  fcfn::runtime::PlanRequest request;
  request.authorization = authorization.value().id;
  auto runtime_plan = opened.value()->plan(request);
  if (!runtime_plan.ok()) {
    std::fprintf(stderr, "consumer: runtime planning failed: %s\n",
                 runtime_plan.status().to_string().c_str());
    return 1;
  }
  std::printf("{\"runtime_claim\":\"%s\",\"epoch\":%llu}\n",
              fcfn::model::to_string(runtime_plan.value().claim),
              static_cast<unsigned long long>(opened.value()->authority().epoch.value()));

  fcfn::model::TransitionRequest transition;
  transition.kind = fcfn::model::TransitionKind::Expand;
  transition.plan = runtime_plan.value().generation;
  transition.plan_digest = runtime_plan.value().digest();
  transition.expected_current_boundary = fcfn::model::BoundaryGeneration{};
  transition.expected_released = true;
  transition.authorization = authorization.value().id;
  auto decision = opened.value()->transition(transition);
  if (!decision.ok() || decision.value().status != fcfn::model::TransitionStatus::Accepted) {
    std::fprintf(stderr, "consumer: transition was not accepted\n");
    return 1;
  }
  fcfn::runtime::SubmitApplyRequest apply_request;
  apply_request.authorization = authorization.value().id;
  apply_request.plan = runtime_plan.value().generation;
  apply_request.plan_digest = runtime_plan.value().digest();
  auto attempt = opened.value()->submit_apply(apply_request);
  if (!attempt.ok()) {
    std::fprintf(stderr, "consumer: apply submission failed\n");
    return 1;
  }
  fcfn::model::ApplyAcknowledgement ack;
  ack.id = attempt.value().id;
  ack.expected_epoch = attempt.value().epoch;
  ack.expected_boot = attempt.value().boot;
  ack.applier_epoch = fcfn::model::ApplierEpoch{1};
  ack.applier_boot = fcfn::BootIdentity{fcfn::BootId{7}, 7};
  ack.applier_sequence = fcfn::Sequence{1};
  ack.observed_boundary_digest = attempt.value().boundary_digest;
  ack.accepted = true;
  if (!opened.value()->acknowledge(ack).ok()) {
    std::fprintf(stderr, "consumer: acknowledgement rejected\n");
    return 1;
  }
  // Acknowledgement is not application: the effect is still unverified.
  std::printf("{\"effect_after_ack\":\"%s\"}\n",
              fcfn::model::to_string(opened.value()->current_effect_state()));
  fcfn::model::EffectVerification verification;
  verification.id = attempt.value().id;
  verification.expected_epoch = attempt.value().epoch;
  verification.expected_boot = attempt.value().boot;
  verification.applier_epoch = fcfn::model::ApplierEpoch{1};
  verification.applier_boot = fcfn::BootIdentity{fcfn::BootId{7}, 7};
  verification.applier_sequence = fcfn::Sequence{2};
  verification.observed_boundary_digest = attempt.value().boundary_digest;
  verification.observation = fcfn::model::EffectObservation::Applied;
  if (!opened.value()->verify_effect(verification).ok()) {
    std::fprintf(stderr, "consumer: effect verification rejected\n");
    return 1;
  }
  std::printf("{\"effect_after_verify\":\"%s\"}\n",
              fcfn::model::to_string(opened.value()->current_effect_state()));

  std::filesystem::remove_all(root, error);
  (void)argc;
  (void)argv;
  std::printf("{\"consumer\":\"ok\"}\n");
  return 0;
}
