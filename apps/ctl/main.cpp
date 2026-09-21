// fcfnctl - offline planning, plan verification, and durable store inspection.
//
// An offline plan is a planning aid: it carries no authority and no generation
// binding. It is marked as such in its explanation document. Only a running
// coordinator can authorize containment.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config_text.hpp"
#include "fcfn/core/json.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/engine/planner.hpp"
#include "fcfn/engine/solver.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/store/store.hpp"
#include "fcfn/version.hpp"

namespace {

using namespace fcfn;  // NOLINT(google-build-using-namespace)

void usage() {
  std::fprintf(stderr,
               "usage: fcfnctl <command> [options]\n"
               "  plan    --topology <file> [--evidence <file>] [--policy <file>]\n"
               "          [--boundary-generation <n>] [--out <file>] [--json-out <file>]\n"
               "  verify  --topology <file> --plan <file> [--evidence <file>] [--policy <file>]\n"
               "  store-inspect --root <dir> [--repair]\n"
               "  version\n");
}

model::EvidenceVector load_evidence(const std::string& path, bool required) {
  if (path.empty()) {
    if (required) {
      std::fprintf(stderr, "fcfnctl: evidence file is required for this command\n");
      std::exit(2);
    }
    std::vector<model::EvidenceEntry> entries;
    auto empty = model::EvidenceVector::build(std::move(entries));
    if (!empty.ok()) {
      std::fprintf(stderr, "fcfnctl: %s\n", empty.status().to_string().c_str());
      std::exit(1);
    }
    return std::move(empty.value());
  }
  auto observations = apps::parse_evidence_file(path);
  if (!observations.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", observations.status().to_string().c_str());
    std::exit(1);
  }
  std::vector<model::EvidenceEntry> entries;
  for (const model::FailureObservation& observation : observations.value()) {
    model::EvidenceEntry entry;
    entry.resource = observation.resource;
    entry.state = observation.state;
    entry.generation = observation.generation;
    entry.source_digest = observation.source_digest;
    entries.push_back(std::move(entry));
  }
  auto vector = model::EvidenceVector::build(std::move(entries));
  if (!vector.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", vector.status().to_string().c_str());
    std::exit(1);
  }
  return std::move(vector.value());
}

model::ContainmentPolicy load_policy(const std::string& path) {
  if (path.empty()) {
    return model::default_policy();
  }
  auto policy = apps::parse_policy_file(path);
  if (!policy.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", policy.status().to_string().c_str());
    std::exit(1);
  }
  return policy.value();
}

model::Topology load_topology(const std::string& path) {
  auto spec = apps::parse_topology_file(path);
  if (!spec.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", spec.status().to_string().c_str());
    std::exit(1);
  }
  auto topology = model::Topology::build(std::move(spec.value()));
  if (!topology.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", topology.status().to_string().c_str());
    std::exit(1);
  }
  return std::move(topology.value());
}

int command_plan(const std::vector<std::string>& arguments) {
  std::string topology_path;
  std::string evidence_path;
  std::string policy_path;
  std::string out_path;
  std::string json_out_path;
  std::uint64_t boundary_generation = 1;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& flag = arguments[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= arguments.size()) {
        usage();
        std::exit(2);
      }
      return arguments[++i];
    };
    if (flag == "--topology") {
      topology_path = value();
    } else if (flag == "--evidence") {
      evidence_path = value();
    } else if (flag == "--policy") {
      policy_path = value();
    } else if (flag == "--out") {
      out_path = value();
    } else if (flag == "--json-out") {
      json_out_path = value();
    } else if (flag == "--boundary-generation") {
      boundary_generation = std::strtoull(value().c_str(), nullptr, 10);
    } else {
      usage();
      return 2;
    }
  }
  if (topology_path.empty()) {
    usage();
    return 2;
  }

  const model::Topology topology = load_topology(topology_path);
  const model::EvidenceVector evidence = load_evidence(evidence_path, false);
  const model::ContainmentPolicy policy = load_policy(policy_path);
  const engine::PropagationGraph graph = engine::PropagationGraph::build(topology);

  engine::PlanInputs inputs;
  inputs.topology = &topology;
  inputs.graph = &graph;
  inputs.evidence = &evidence;
  inputs.policy = &policy;
  inputs.freshness.require_current_boot_confirmation = false;
  inputs.boundary_generation = model::BoundaryGeneration{boundary_generation};

  auto planner_result = engine::compute_plan(inputs, model::AuthorityVector{}, model::PlanGeneration{1});
  if (!planner_result.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", planner_result.status().to_string().c_str());
    return 1;
  }
  model::ContainmentPlan plan = std::move(planner_result.value().plan);
  (void)plan.explanation.add(model::ReasonCode::AuthorizationRequired,
                             "offline planning aid: this plan carries no coordinator authority");
  plan.seal();

  std::printf("%s\n", plan.to_json().c_str());
  if (!out_path.empty()) {
    const std::vector<std::byte> encoded = plan.encode();
    const VoidResult written =
        store::write_file_atomic(out_path, std::span<const std::byte>(encoded.data(), encoded.size()), true);
    if (!written.ok()) {
      std::fprintf(stderr, "fcfnctl: %s\n", written.status().to_string().c_str());
      return 1;
    }
  }
  if (!json_out_path.empty()) {
    const std::string json = plan.to_json();
    const VoidResult written = store::write_file_atomic(
        json_out_path,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(json.data()), json.size()), true);
    if (!written.ok()) {
      std::fprintf(stderr, "fcfnctl: %s\n", written.status().to_string().c_str());
      return 1;
    }
  }
  const bool releasable = plan.is_releasable();
  return releasable || plan.claim == model::ContainmentClaim::ProvenContainment ||
                 plan.claim == model::ContainmentClaim::ProvenFeasibleNotMinimal ||
                 plan.claim == model::ContainmentClaim::ProvenInfeasible
             ? 0
             : 3;
}

int command_verify(const std::vector<std::string>& arguments) {
  std::string topology_path;
  std::string evidence_path;
  std::string policy_path;
  std::string plan_path;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& flag = arguments[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= arguments.size()) {
        usage();
        std::exit(2);
      }
      return arguments[++i];
    };
    if (flag == "--topology") {
      topology_path = value();
    } else if (flag == "--evidence") {
      evidence_path = value();
    } else if (flag == "--policy") {
      policy_path = value();
    } else if (flag == "--plan") {
      plan_path = value();
    } else {
      usage();
      return 2;
    }
  }
  if (topology_path.empty() || plan_path.empty()) {
    usage();
    return 2;
  }

  const model::Topology topology = load_topology(topology_path);
  const model::EvidenceVector evidence = load_evidence(evidence_path, false);
  const model::ContainmentPolicy policy = load_policy(policy_path);

  auto plan_bytes = store::read_file(plan_path, kMaxRecordPayloadBytes);
  if (!plan_bytes.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", plan_bytes.status().to_string().c_str());
    return 1;
  }
  auto plan = model::ContainmentPlan::decode(
      std::span<const std::byte>(plan_bytes.value().data(), plan_bytes.value().size()));
  if (!plan.ok()) {
    std::fprintf(stderr, "fcfnctl: %s\n", plan.status().to_string().c_str());
    return 1;
  }

  engine::ContainmentInstanceSpec spec;
  for (const model::ResourceId& id : plan.value().failure_sources) {
    spec.failure_sources.push_back(id);
  }
  for (const model::ResourceId& id : plan.value().protected_obligations) {
    spec.protected_obligations.push_back(id);
  }
  spec.require_source_inclusion = policy.require_failure_source_inclusion;
  spec.allow_protected_inclusion = policy.allow_protected_inclusion;
  spec.minimize_protected_inclusions = policy.minimize_protected_inclusions;
  spec.max_total_weight = policy.max_boundary_weight;
  spec.max_members = policy.max_boundary_members;

  JsonWriter writer;
  writer.begin_object();
  writer.member("plan_generation", plan.value().generation.value());
  writer.member("plan_digest", plan.value().digest().hex());
  writer.member("plan_claim", model::to_string(plan.value().claim));

  if (spec.failure_sources.empty() || spec.protected_obligations.empty()) {
    // Degenerate instances are verified structurally only.
    writer.member("verified", plan.value().boundary.empty());
    writer.member("reason", "instance has no failure sources or no protected obligations");
    writer.end_object();
    std::printf("%s\n", writer.take().c_str());
    return plan.value().boundary.empty() ? 0 : 1;
  }

  const engine::PropagationGraph graph = engine::PropagationGraph::build(topology);
  auto problem = engine::ContainmentProblem::build(graph, spec);
  if (!problem.ok()) {
    writer.member("verified", false);
    writer.member("reason", problem.status().to_string());
    writer.end_object();
    std::printf("%s\n", writer.take().c_str());
    return 1;
  }

  std::vector<model::NodeIndex> members;
  bool members_resolved = true;
  for (const model::BoundaryMember& member : plan.value().boundary.members()) {
    const auto index = topology.find(member.resource);
    if (!index.has_value()) {
      members_resolved = false;
      break;
    }
    members.push_back(*index);
  }
  if (!members_resolved) {
    writer.member("verified", false);
    writer.member("reason", "boundary names a resource that is not in the topology");
    writer.end_object();
    std::printf("%s\n", writer.take().c_str());
    return 1;
  }

  const Result<engine::ObjectiveVector> objective = engine::verify_cut(problem.value(), members);
  writer.member("verified", objective.ok());
  if (objective.ok()) {
    writer.member("objective_protected_inclusions", objective.value().protected_inclusions);
    writer.member("objective_total_weight", objective.value().total_weight);
    writer.member("objective_cardinality", objective.value().cardinality);
    writer.member("plan_total_weight", plan.value().total_weight);
    writer.member("plan_member_count", static_cast<std::uint64_t>(plan.value().member_count));
  } else {
    writer.member("reason", objective.status().to_string());
  }
  writer.end_object();
  std::printf("%s\n", writer.take().c_str());
  return objective.ok() ? 0 : 1;
}

int command_store_inspect(const std::vector<std::string>& arguments) {
  std::string root;
  bool repair = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& flag = arguments[i];
    if (flag == "--root" && i + 1 < arguments.size()) {
      root = arguments[++i];
    } else if (flag == "--repair") {
      repair = true;
    } else {
      usage();
      return 2;
    }
  }
  if (root.empty()) {
    usage();
    return 2;
  }
  store::DurableStore::Options options;
  options.root = root;
  options.allow_torn_tail_recovery = repair;
  auto opened = store::DurableStore::open(options);
  if (!opened.ok()) {
    JsonWriter writer;
    writer.begin_object();
    writer.member("opened", false);
    writer.member("error", opened.status().to_string());
    writer.end_object();
    std::printf("%s\n", writer.take().c_str());
    return 1;
  }
  const store::DurableStore::RecoveryReport& report = opened.value()->recovery();
  JsonWriter writer;
  writer.begin_object();
  writer.member("opened", true);
  writer.member("fresh", report.fresh);
  writer.member("torn_tail", report.torn_tail);
  writer.member("torn_tail_bytes", report.torn_tail_bytes);
  writer.member("replayed_records", static_cast<std::uint64_t>(report.replayed_records));
  writer.member("skipped_records", static_cast<std::uint64_t>(report.skipped_records));
  writer.member("snapshot_sequence", report.snapshot_sequence.value());
  writer.member("wal_segment", report.wal_segment.value());
  writer.member("last_sequence", report.last_sequence.value());
  writer.member("snapshot_payload_bytes", static_cast<std::uint64_t>(report.snapshot_payload.size()));
  writer.member("detail", report.detail);
  writer.key("record_types");
  writer.begin_object();
  for (std::size_t i = 0; i < 17; ++i) {
    const auto type = static_cast<store::RecordType>(i);
    std::uint64_t count = 0;
    for (const store::Frame& frame : opened.value()->replayed_frames()) {
      if (frame.type == type) {
        ++count;
      }
    }
    writer.member(store::to_string(type), count);
  }
  writer.end_object();
  writer.end_object();
  std::printf("%s\n", writer.take().c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 2; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  const std::string command = argc > 1 ? argv[1] : "";
  if (command == "plan") {
    return command_plan(arguments);
  }
  if (command == "verify") {
    return command_verify(arguments);
  }
  if (command == "store-inspect") {
    return command_store_inspect(arguments);
  }
  if (command == "version") {
    std::printf("{\"version\":\"%s\",\"semantic_version\":%u}\n", kVersionString,
                static_cast<unsigned>(kSemanticVersion));
    return 0;
  }
  usage();
  return 2;
}
