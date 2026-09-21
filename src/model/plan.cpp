// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/plan.hpp"

#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/json.hpp"

namespace fcfn::model {
namespace {

struct ClaimToken {
  ContainmentClaim value;
  const char* token;
};

constexpr ClaimToken kClaimTokens[] = {
    {ContainmentClaim::ProvenContainment, "proven_containment"},
    {ContainmentClaim::ProvenFeasibleNotMinimal, "proven_feasible_not_minimal"},
    {ContainmentClaim::Indeterminate, "indeterminate"},
    {ContainmentClaim::ProvenInfeasible, "proven_infeasible"},
    {ContainmentClaim::Invalid, "invalid"},
    {ContainmentClaim::Unsupported, "unsupported"},
};

struct FeasibilityToken {
  FeasibilityStatus value;
  const char* token;
};

constexpr FeasibilityToken kFeasibilityTokens[] = {
    {FeasibilityStatus::ProvenFeasible, "proven_feasible"},
    {FeasibilityStatus::ProvenInfeasible, "proven_infeasible"},
    {FeasibilityStatus::Unknown, "unknown"},
};

struct OptimalityToken {
  OptimalityStatus value;
  const char* token;
};

constexpr OptimalityToken kOptimalityTokens[] = {
    {OptimalityStatus::ProvenOptimal, "proven_optimal"},
    {OptimalityStatus::BoundedNotProven, "bounded_not_proven"},
    {OptimalityStatus::NotAttempted, "not_attempted"},
    {OptimalityStatus::NotApplicable, "not_applicable"},
};

struct CurrencyToken {
  EvidenceCurrency value;
  const char* token;
};

constexpr CurrencyToken kCurrencyTokens[] = {
    {EvidenceCurrency::Current, "current"},
    {EvidenceCurrency::IncompleteFrontier, "incomplete_frontier"},
    {EvidenceCurrency::Stale, "stale"},
    {EvidenceCurrency::Conflicting, "conflicting"},
};

}  // namespace

const char* to_string(ContainmentClaim value) noexcept {
  for (const ClaimToken& entry : kClaimTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_claim";
}

const char* to_string(FeasibilityStatus value) noexcept {
  for (const FeasibilityToken& entry : kFeasibilityTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_feasibility";
}

const char* to_string(OptimalityStatus value) noexcept {
  for (const OptimalityToken& entry : kOptimalityTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_optimality";
}

const char* to_string(EvidenceCurrency value) noexcept {
  for (const CurrencyToken& entry : kCurrencyTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_currency";
}

Digest ContainmentPlan::compute_digest() const {
  CanonicalWriter writer;
  writer.strong(generation);
  writer.digest(boundary.digest());
  writer.u8(static_cast<std::uint8_t>(claim));
  writer.u8(static_cast<std::uint8_t>(feasibility));
  writer.u8(static_cast<std::uint8_t>(optimality));
  writer.u8(static_cast<std::uint8_t>(currency));
  writer.digest(authority.digest());
  writer.digest(topology_digest);
  writer.digest(policy_digest);
  writer.digest(evidence_digest);
  writer.digest(instance_digest);
  writer.u32(static_cast<std::uint32_t>(failure_sources.size()));
  for (const ResourceId& id : failure_sources) {
    writer.resource_id(id);
  }
  writer.u32(static_cast<std::uint32_t>(protected_obligations.size()));
  for (const ResourceId& id : protected_obligations) {
    writer.resource_id(id);
  }
  writer.u32(static_cast<std::uint32_t>(uncontainable_sources.size()));
  for (const ResourceId& id : uncontainable_sources) {
    writer.resource_id(id);
  }
  writer.u32(static_cast<std::uint32_t>(proven_necessary_members.size()));
  for (const ResourceId& id : proven_necessary_members) {
    writer.resource_id(id);
  }
  writer.u32(static_cast<std::uint32_t>(indeterminate_necessity.size()));
  for (const ResourceId& id : indeterminate_necessity) {
    writer.resource_id(id);
  }
  writer.u64(total_weight);
  writer.u64(static_cast<std::uint64_t>(member_count));
  writer.u64(counters.candidate_nodes);
  writer.u64(counters.explored_nodes);
  writer.u64(counters.pruned_branches);
  writer.u64(counters.residual_paths_checked);
  writer.u64(counters.budget);
  writer.boolean(counters.budget_exhausted);
  writer.boolean(counters.heuristic_used);
  writer.boolean(infeasibility.present);
  writer.u32(static_cast<std::uint32_t>(infeasibility.path.size()));
  for (const ResourceId& id : infeasibility.path) {
    writer.resource_id(id);
  }
  writer.u32(static_cast<std::uint32_t>(explanation.items().size()));
  for (const ExplanationItem& item : explanation.items()) {
    writer.u16(static_cast<std::uint16_t>(item.code));
    writer.resource_id(item.subject);
    writer.boolean(item.has_subject);
    writer.u64(item.value);
    writer.boolean(item.has_value);
    writer.text(item.text);
  }
  writer.boolean(explanation.truncated());
  return writer.digest();
}

bool ContainmentPlan::is_releasable() const noexcept {
  return claim == ContainmentClaim::ProvenContainment && boundary.empty();
}

std::string ContainmentPlan::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.member("plan_generation", generation.value());
  writer.member("claim", to_string(claim));
  writer.member("feasibility", to_string(feasibility));
  writer.member("optimality", to_string(optimality));
  writer.member("evidence_currency", to_string(currency));
  writer.member("plan_digest", digest_.hex());
  writer.member("topology_digest", topology_digest.hex());
  writer.member("policy_digest", policy_digest.hex());
  writer.member("evidence_digest", evidence_digest.hex());
  writer.member("instance_digest", instance_digest.hex());
  writer.key("authority");
  writer.begin_object();
  writer.member("epoch", authority.epoch.value());
  writer.member("boot_id", authority.boot.id.value());
  writer.member("boot_pid", static_cast<std::uint64_t>(authority.boot.process_id));
  writer.member("policy_generation", authority.policy.value());
  writer.member("topology_generation", authority.topology.value());
  writer.member("evidence_revision", authority.evidence_revision.value());
  writer.member("issued_sequence", authority.issued_sequence.value());
  writer.member("authority_digest", authority.digest().hex());
  writer.end_object();
  auto write_ids = [&writer](const char* name, const std::vector<ResourceId>& ids) {
    writer.key(name);
    writer.begin_array();
    for (const ResourceId& id : ids) {
      writer.value(id.value());
    }
    writer.end_array();
  };
  write_ids("failure_sources", failure_sources);
  write_ids("protected_obligations", protected_obligations);
  write_ids("uncontainable_sources", uncontainable_sources);
  write_ids("proven_necessary_members", proven_necessary_members);
  write_ids("indeterminate_necessity", indeterminate_necessity);
  writer.member("total_weight", total_weight);
  writer.member("member_count", static_cast<std::uint64_t>(member_count));
  writer.key("counters");
  writer.begin_object();
  writer.member("candidate_nodes", counters.candidate_nodes);
  writer.member("explored_nodes", counters.explored_nodes);
  writer.member("pruned_branches", counters.pruned_branches);
  writer.member("residual_paths_checked", counters.residual_paths_checked);
  writer.member("budget", counters.budget);
  writer.member("budget_exhausted", counters.budget_exhausted);
  writer.member("heuristic_used", counters.heuristic_used);
  writer.end_object();
  writer.key("infeasibility_witness");
  writer.begin_object();
  writer.member("present", infeasibility.present);
  write_ids("path", infeasibility.path);
  writer.end_object();
  writer.key("boundary");
  writer.begin_object();
  writer.member("generation", boundary.generation().value());
  writer.member("digest", boundary.digest().hex());
  writer.member("member_count", static_cast<std::uint64_t>(boundary.size()));
  writer.member("total_weight", boundary.total_weight());
  writer.member("protected_member_count", static_cast<std::uint64_t>(boundary.protected_member_count()));
  writer.key("members");
  writer.begin_array();
  for (const BoundaryMember& member : boundary.members()) {
    writer.begin_object();
    writer.member("resource", member.resource.value());
    writer.member("reason", to_string(member.reason));
    writer.member("protected_obligation", member.protected_obligation);
    writer.member("weight", member.weight);
    writer.member("witness_uses_unknown", member.witness_uses_unknown);
    writer.key("witness_path");
    writer.begin_array();
    for (const ResourceId& step : member.witness_path) {
      writer.value(step.value());
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  writer.key("explanation");
  writer.begin_array();
  for (const ExplanationItem& item : explanation.items()) {
    writer.begin_object();
    writer.member("code", model::to_string(item.code));
    writer.member("subject", item.subject.value());
    writer.member("has_subject", item.has_subject);
    writer.member("value", item.value);
    writer.member("has_value", item.has_value);
    writer.member("text", item.text);
    writer.end_object();
  }
  writer.end_array();
  writer.member("explanation_truncated", explanation.truncated());
  writer.end_object();
  return writer.take();
}

namespace {

void write_ids(CanonicalWriter& writer, const std::vector<ResourceId>& ids) {
  writer.u32(static_cast<std::uint32_t>(ids.size()));
  for (const ResourceId& id : ids) {
    writer.resource_id(id);
  }
}

Result<std::vector<ResourceId>> read_ids(CanonicalReader& reader, std::size_t max_entries) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > max_entries) {
    return Status{StatusCode::LimitExceeded, "encoded identifier list exceeds bound"};
  }
  std::vector<ResourceId> ids;
  ids.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto id = reader.resource_id();
    if (!reader.ok()) {
      return reader.status();
    }
    if (!id.ok()) {
      return id.status();
    }
    ids.push_back(std::move(id.value()));
  }
  return ids;
}

}  // namespace

std::vector<std::byte> ContainmentPlan::encode() const {
  CanonicalWriter writer;
  writer.strong(generation);
  const std::vector<std::byte> boundary_bytes = boundary.encode();
  writer.blob(std::span<const std::byte>(boundary_bytes.data(), boundary_bytes.size()));
  writer.u8(static_cast<std::uint8_t>(claim));
  writer.u8(static_cast<std::uint8_t>(feasibility));
  writer.u8(static_cast<std::uint8_t>(optimality));
  writer.u8(static_cast<std::uint8_t>(currency));
  const std::vector<std::byte> authority_bytes = authority.encode();
  writer.blob(std::span<const std::byte>(authority_bytes.data(), authority_bytes.size()));
  writer.digest(topology_digest);
  writer.digest(policy_digest);
  writer.digest(evidence_digest);
  writer.digest(instance_digest);
  write_ids(writer, failure_sources);
  write_ids(writer, protected_obligations);
  write_ids(writer, uncontainable_sources);
  write_ids(writer, proven_necessary_members);
  write_ids(writer, indeterminate_necessity);
  writer.u64(total_weight);
  writer.u64(static_cast<std::uint64_t>(member_count));
  writer.u64(counters.candidate_nodes);
  writer.u64(counters.explored_nodes);
  writer.u64(counters.pruned_branches);
  writer.u64(counters.residual_paths_checked);
  writer.u64(counters.budget);
  writer.boolean(counters.budget_exhausted);
  writer.boolean(counters.heuristic_used);
  writer.boolean(infeasibility.present);
  write_ids(writer, infeasibility.path);
  writer.u32(static_cast<std::uint32_t>(explanation.items().size()));
  for (const ExplanationItem& item : explanation.items()) {
    writer.u16(static_cast<std::uint16_t>(item.code));
    writer.boolean(item.has_subject);
    if (item.has_subject) {
      writer.resource_id(item.subject);
    }
    writer.u64(item.value);
    writer.boolean(item.has_value);
    writer.text(item.text);
  }
  writer.boolean(explanation.truncated());
  writer.digest(digest_);
  return writer.data();
}

Result<ContainmentPlan> ContainmentPlan::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  ContainmentPlan plan;
  plan.generation = reader.strong<PlanGenerationTag>();
  const std::span<const std::byte> boundary_bytes = reader.blob(kMaxRecordPayloadBytes);
  const std::uint8_t claim = reader.u8();
  const std::uint8_t feasibility = reader.u8();
  const std::uint8_t optimality = reader.u8();
  const std::uint8_t currency = reader.u8();
  const std::span<const std::byte> authority_bytes = reader.blob(1024);
  plan.topology_digest = reader.digest();
  plan.policy_digest = reader.digest();
  plan.evidence_digest = reader.digest();
  plan.instance_digest = reader.digest();
  if (!reader.ok()) {
    return reader.status();
  }
  if (claim > static_cast<std::uint8_t>(ContainmentClaim::Unsupported) ||
      feasibility > static_cast<std::uint8_t>(FeasibilityStatus::Unknown) ||
      optimality > static_cast<std::uint8_t>(OptimalityStatus::NotApplicable) ||
      currency > static_cast<std::uint8_t>(EvidenceCurrency::Conflicting)) {
    return Status{StatusCode::InvalidArgument, "invalid plan enum value"};
  }
  plan.claim = static_cast<ContainmentClaim>(claim);
  plan.feasibility = static_cast<FeasibilityStatus>(feasibility);
  plan.optimality = static_cast<OptimalityStatus>(optimality);
  plan.currency = static_cast<EvidenceCurrency>(currency);

  auto boundary = ContainmentBoundary::decode(boundary_bytes);
  if (!boundary.ok()) {
    return boundary.status();
  }
  plan.boundary = std::move(boundary.value());
  auto authority = AuthorityVector::decode(authority_bytes);
  if (!authority.ok()) {
    return authority.status();
  }
  plan.authority = std::move(authority.value());

  auto sources = read_ids(reader, kMaxFailureSources);
  if (!sources.ok()) {
    return sources.status();
  }
  plan.failure_sources = std::move(sources.value());
  auto protected_ids = read_ids(reader, kMaxProtectedObligations);
  if (!protected_ids.ok()) {
    return protected_ids.status();
  }
  plan.protected_obligations = std::move(protected_ids.value());
  auto uncontainable = read_ids(reader, kMaxFailureSources);
  if (!uncontainable.ok()) {
    return uncontainable.status();
  }
  plan.uncontainable_sources = std::move(uncontainable.value());
  auto necessary = read_ids(reader, kMaxBoundaryMembers);
  if (!necessary.ok()) {
    return necessary.status();
  }
  plan.proven_necessary_members = std::move(necessary.value());
  auto indefinite = read_ids(reader, kMaxBoundaryMembers);
  if (!indefinite.ok()) {
    return indefinite.status();
  }
  plan.indeterminate_necessity = std::move(indefinite.value());

  plan.total_weight = reader.u64();
  plan.member_count = static_cast<std::size_t>(reader.u64());
  plan.counters.candidate_nodes = reader.u64();
  plan.counters.explored_nodes = reader.u64();
  plan.counters.pruned_branches = reader.u64();
  plan.counters.residual_paths_checked = reader.u64();
  plan.counters.budget = reader.u64();
  plan.counters.budget_exhausted = reader.boolean();
  plan.counters.heuristic_used = reader.boolean();
  plan.infeasibility.present = reader.boolean();
  auto path = read_ids(reader, kMaxCutCertificatePathNodes);
  if (!path.ok()) {
    return path.status();
  }
  plan.infeasibility.path = std::move(path.value());

  const std::uint32_t item_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (item_count > kMaxExplanationItems) {
    return Status{StatusCode::LimitExceeded, "encoded explanation exceeds bound"};
  }
  for (std::uint32_t i = 0; i < item_count; ++i) {
    ExplanationItem item;
    const std::uint16_t code = reader.u16();
    item.has_subject = reader.boolean();
    if (!reader.ok()) {
      return reader.status();
    }
    if (item.has_subject) {
      auto subject = reader.resource_id();
      if (!reader.ok()) {
        return reader.status();
      }
      if (!subject.ok()) {
        return subject.status();
      }
      item.subject = std::move(subject.value());
    }
    item.value = reader.u64();
    item.has_value = reader.boolean();
    item.text = reader.text(kMaxExplanationTextLength);
    if (!reader.ok()) {
      return reader.status();
    }
    if (code > kMaxReasonCodeValue) {
      return Status{StatusCode::InvalidArgument, "invalid explanation reason code"};
    }
    item.code = static_cast<ReasonCode>(code);
    (void)plan.explanation.add_item(item);
  }
  if (reader.boolean()) {
    plan.explanation.mark_truncated();
  }
  plan.digest_ = reader.digest();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!(plan.digest_ == plan.compute_digest())) {
    return Status{StatusCode::Corrupt, "plan digest does not match its contents"};
  }
  return plan;
}

}  // namespace fcfn::model
