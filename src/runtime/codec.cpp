// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "runtime_state.hpp"

#include <utility>

#include "fcfn/core/canonical.hpp"

namespace fcfn::runtime::codec {
namespace {

constexpr std::uint8_t kMaxEffectState = 8;
constexpr std::uint8_t kMaxEffectObservation = 4;
constexpr std::uint8_t kMaxTransitionKind = 2;
constexpr std::uint8_t kMaxTransitionStatus = 4;

void write_explanation(CanonicalWriter& writer, const model::Explanation& explanation) {
  writer.u32(static_cast<std::uint32_t>(explanation.items().size()));
  for (const model::ExplanationItem& item : explanation.items()) {
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
}

Result<model::Explanation> read_explanation(CanonicalReader& reader) {
  model::Explanation explanation;
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > kMaxExplanationItems) {
    return Status{StatusCode::LimitExceeded, "encoded explanation exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    model::ExplanationItem item;
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
    if (code > model::kMaxReasonCodeValue) {
      return Status{StatusCode::InvalidArgument, "invalid explanation reason code"};
    }
    item.code = static_cast<model::ReasonCode>(code);
    (void)explanation.add_item(item);
  }
  if (reader.boolean()) {
    explanation.mark_truncated();
  }
  if (!reader.ok()) {
    return reader.status();
  }
  return explanation;
}

void write_ids(CanonicalWriter& writer, const std::vector<model::ResourceId>& ids) {
  writer.u32(static_cast<std::uint32_t>(ids.size()));
  for (const model::ResourceId& id : ids) {
    writer.resource_id(id);
  }
}

Result<std::vector<model::ResourceId>> read_ids(CanonicalReader& reader, std::size_t max_entries) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > max_entries) {
    return Status{StatusCode::LimitExceeded, "encoded identifier list exceeds the bound"};
  }
  std::vector<model::ResourceId> ids;
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

std::vector<std::byte> encode_detection(const model::DetectionRecord& record) {
  CanonicalWriter writer;
  writer.resource_id(record.resource);
  writer.u8(static_cast<std::uint8_t>(record.state));
  writer.strong(record.generation);
  writer.digest(record.source_digest);
  writer.strong(record.epoch);
  writer.u64(record.boot.id.value());
  writer.u32(record.boot.process_id);
  writer.strong(record.sequence);
  writer.digest(record.record_digest);
  return writer.data();
}

Result<model::DetectionRecord> decode_detection(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::DetectionRecord record;
  auto resource = reader.resource_id();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!resource.ok()) {
    return resource.status();
  }
  record.resource = std::move(resource.value());
  const std::uint8_t state = reader.u8();
  record.generation = reader.strong<model::EvidenceGenerationTag>();
  record.source_digest = reader.digest();
  record.epoch = reader.strong<CoordinatorEpochTag>();
  record.boot.id = BootId{reader.u64()};
  record.boot.process_id = reader.u32();
  record.sequence = reader.strong<SequenceTag>();
  record.record_digest = reader.digest();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (state > static_cast<std::uint8_t>(model::EvidenceState::Unknown)) {
    return Status{StatusCode::InvalidArgument, "invalid evidence state value"};
  }
  record.state = static_cast<model::EvidenceState>(state);
  return record;
}

std::vector<std::byte> encode_attempt(const model::ApplyAttempt& attempt) {
  CanonicalWriter writer;
  writer.strong(attempt.id.plan);
  writer.strong(attempt.id.sequence);
  writer.digest(attempt.plan_digest);
  writer.digest(attempt.boundary_digest);
  writer.strong(attempt.boundary_generation);
  writer.strong(attempt.epoch);
  writer.u64(attempt.boot.id.value());
  writer.u32(attempt.boot.process_id);
  writer.u8(static_cast<std::uint8_t>(attempt.state));
  writer.strong(attempt.submitted_sequence);
  writer.strong(attempt.applier_epoch);
  writer.u64(attempt.applier_boot.id.value());
  writer.u32(attempt.applier_boot.process_id);
  writer.strong(attempt.applier_sequence);
  writer.digest(attempt.observed_boundary_digest);
  writer.u64(attempt.submitted_at_millis);
  writer.u64(attempt.acknowledged_at_millis);
  writer.u64(attempt.verified_at_millis);
  write_explanation(writer, attempt.explanation);
  return writer.data();
}

Result<model::ApplyAttempt> decode_attempt(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::ApplyAttempt attempt;
  attempt.id.plan = reader.strong<model::PlanGenerationTag>();
  attempt.id.sequence = reader.strong<model::AttemptSequenceTag>();
  attempt.plan_digest = reader.digest();
  attempt.boundary_digest = reader.digest();
  attempt.boundary_generation = reader.strong<model::BoundaryGenerationTag>();
  attempt.epoch = reader.strong<CoordinatorEpochTag>();
  attempt.boot.id = BootId{reader.u64()};
  attempt.boot.process_id = reader.u32();
  const std::uint8_t state = reader.u8();
  attempt.submitted_sequence = reader.strong<SequenceTag>();
  attempt.applier_epoch = reader.strong<model::ApplierEpochTag>();
  attempt.applier_boot.id = BootId{reader.u64()};
  attempt.applier_boot.process_id = reader.u32();
  attempt.applier_sequence = reader.strong<SequenceTag>();
  attempt.observed_boundary_digest = reader.digest();
  attempt.submitted_at_millis = reader.u64();
  attempt.acknowledged_at_millis = reader.u64();
  attempt.verified_at_millis = reader.u64();
  if (!reader.ok()) {
    return reader.status();
  }
  if (state > kMaxEffectState) {
    return Status{StatusCode::InvalidArgument, "invalid effect state value"};
  }
  attempt.state = static_cast<model::EffectState>(state);
  auto explanation = read_explanation(reader);
  if (!explanation.ok()) {
    return explanation.status();
  }
  attempt.explanation = std::move(explanation.value());
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return attempt;
}

std::vector<std::byte> encode_acknowledgement(const model::ApplyAcknowledgement& ack) {
  CanonicalWriter writer;
  writer.strong(ack.id.plan);
  writer.strong(ack.id.sequence);
  writer.strong(ack.expected_epoch);
  writer.u64(ack.expected_boot.id.value());
  writer.u32(ack.expected_boot.process_id);
  writer.strong(ack.applier_epoch);
  writer.u64(ack.applier_boot.id.value());
  writer.u32(ack.applier_boot.process_id);
  writer.strong(ack.applier_sequence);
  writer.digest(ack.observed_boundary_digest);
  writer.boolean(ack.accepted);
  return writer.data();
}

Result<model::ApplyAcknowledgement> decode_acknowledgement(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::ApplyAcknowledgement ack;
  ack.id.plan = reader.strong<model::PlanGenerationTag>();
  ack.id.sequence = reader.strong<model::AttemptSequenceTag>();
  ack.expected_epoch = reader.strong<CoordinatorEpochTag>();
  ack.expected_boot.id = BootId{reader.u64()};
  ack.expected_boot.process_id = reader.u32();
  ack.applier_epoch = reader.strong<model::ApplierEpochTag>();
  ack.applier_boot.id = BootId{reader.u64()};
  ack.applier_boot.process_id = reader.u32();
  ack.applier_sequence = reader.strong<SequenceTag>();
  ack.observed_boundary_digest = reader.digest();
  ack.accepted = reader.boolean();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return ack;
}

std::vector<std::byte> encode_verification(const model::EffectVerification& report) {
  CanonicalWriter writer;
  writer.strong(report.id.plan);
  writer.strong(report.id.sequence);
  writer.strong(report.expected_epoch);
  writer.u64(report.expected_boot.id.value());
  writer.u32(report.expected_boot.process_id);
  writer.strong(report.applier_epoch);
  writer.u64(report.applier_boot.id.value());
  writer.u32(report.applier_boot.process_id);
  writer.strong(report.applier_sequence);
  writer.digest(report.observed_boundary_digest);
  writer.u8(static_cast<std::uint8_t>(report.observation));
  return writer.data();
}

Result<model::EffectVerification> decode_verification(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::EffectVerification report;
  report.id.plan = reader.strong<model::PlanGenerationTag>();
  report.id.sequence = reader.strong<model::AttemptSequenceTag>();
  report.expected_epoch = reader.strong<CoordinatorEpochTag>();
  report.expected_boot.id = BootId{reader.u64()};
  report.expected_boot.process_id = reader.u32();
  report.applier_epoch = reader.strong<model::ApplierEpochTag>();
  report.applier_boot.id = BootId{reader.u64()};
  report.applier_boot.process_id = reader.u32();
  report.applier_sequence = reader.strong<SequenceTag>();
  report.observed_boundary_digest = reader.digest();
  const std::uint8_t observation = reader.u8();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (observation > kMaxEffectObservation) {
    return Status{StatusCode::InvalidArgument, "invalid effect observation value"};
  }
  report.observation = static_cast<model::EffectObservation>(observation);
  return report;
}

std::vector<std::byte> encode_transition(const model::TransitionDecision& decision) {
  CanonicalWriter writer;
  writer.strong(decision.generation);
  writer.u8(static_cast<std::uint8_t>(decision.kind));
  writer.u8(static_cast<std::uint8_t>(decision.status));
  writer.u16(static_cast<std::uint16_t>(decision.code));
  writer.strong(decision.from_generation);
  writer.strong(decision.to_generation);
  writer.boolean(decision.to_released);
  writer.digest(decision.from_digest);
  writer.digest(decision.to_digest);
  writer.digest(decision.plan_digest);
  writer.u8(static_cast<std::uint8_t>(decision.required_effect_state));
  writer.u8(static_cast<std::uint8_t>(decision.observed_effect_state));
  writer.boolean(decision.effect_verified);
  writer.strong(decision.epoch);
  writer.u64(decision.boot.id.value());
  writer.u32(decision.boot.process_id);
  writer.strong(decision.sequence);
  write_explanation(writer, decision.explanation);
  return writer.data();
}

Result<model::TransitionDecision> decode_transition(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::TransitionDecision decision;
  decision.generation = reader.strong<model::TransitionGenerationTag>();
  const std::uint8_t kind = reader.u8();
  const std::uint8_t status = reader.u8();
  const std::uint16_t code = reader.u16();
  decision.from_generation = reader.strong<model::BoundaryGenerationTag>();
  decision.to_generation = reader.strong<model::BoundaryGenerationTag>();
  decision.to_released = reader.boolean();
  decision.from_digest = reader.digest();
  decision.to_digest = reader.digest();
  decision.plan_digest = reader.digest();
  const std::uint8_t required = reader.u8();
  const std::uint8_t observed = reader.u8();
  decision.effect_verified = reader.boolean();
  decision.epoch = reader.strong<CoordinatorEpochTag>();
  decision.boot.id = BootId{reader.u64()};
  decision.boot.process_id = reader.u32();
  decision.sequence = reader.strong<SequenceTag>();
  if (!reader.ok()) {
    return reader.status();
  }
  if (kind > kMaxTransitionKind || status > kMaxTransitionStatus || required > kMaxEffectState ||
      observed > kMaxEffectState) {
    return Status{StatusCode::InvalidArgument, "invalid transition enum value"};
  }
  if (code > static_cast<std::uint16_t>(StatusCode::Internal)) {
    return Status{StatusCode::InvalidArgument, "invalid transition status code"};
  }
  decision.kind = static_cast<model::TransitionKind>(kind);
  decision.status = static_cast<model::TransitionStatus>(status);
  decision.code = static_cast<StatusCode>(code);
  decision.required_effect_state = static_cast<model::EffectState>(required);
  decision.observed_effect_state = static_cast<model::EffectState>(observed);
  auto explanation = read_explanation(reader);
  if (!explanation.ok()) {
    return explanation.status();
  }
  decision.explanation = std::move(explanation.value());
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return decision;
}

std::vector<std::byte> encode_boundary_decision(PlanGeneration plan, const Digest& plan_digest,
                                               bool released,
                                               std::span<const std::byte> boundary_bytes) {
  CanonicalWriter writer;
  writer.strong(plan);
  writer.digest(plan_digest);
  writer.boolean(released);
  writer.blob(boundary_bytes);
  return writer.data();
}

Result<BoundaryDecision> decode_boundary_decision(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  BoundaryDecision decision;
  decision.plan = reader.strong<model::PlanGenerationTag>();
  decision.plan_digest = reader.digest();
  decision.released = reader.boolean();
  const std::span<const std::byte> boundary = reader.blob(kMaxRecordPayloadBytes);
  if (!reader.ok()) {
    return reader.status();
  }
  decision.boundary_bytes.assign(boundary.begin(), boundary.end());
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return decision;
}

std::vector<std::byte> encode_epoch_advance(CoordinatorEpoch epoch, const BootIdentity& boot) {
  CanonicalWriter writer;
  writer.strong(epoch);
  writer.u64(boot.id.value());
  writer.u32(boot.process_id);
  return writer.data();
}

Result<std::pair<CoordinatorEpoch, BootIdentity>> decode_epoch_advance(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const CoordinatorEpoch epoch = reader.strong<CoordinatorEpochTag>();
  BootIdentity boot;
  boot.id = BootId{reader.u64()};
  boot.process_id = reader.u32();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (epoch.is_zero()) {
    return Status{StatusCode::InvalidArgument, "epoch advance carries a zero epoch"};
  }
  return std::make_pair(epoch, boot);
}

std::vector<std::byte> encode_fence(const std::vector<BootId>& boots) {
  CanonicalWriter writer;
  writer.u32(static_cast<std::uint32_t>(boots.size()));
  for (const BootId boot : boots) {
    writer.strong(boot);
  }
  return writer.data();
}

Result<std::vector<BootId>> decode_fence(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > kMaxFencedBoots) {
    return Status{StatusCode::LimitExceeded, "encoded fence list exceeds the bound"};
  }
  std::vector<BootId> boots;
  boots.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const BootId boot = reader.strong<BootIdTag>();
    if (!reader.ok()) {
      return reader.status();
    }
    boots.push_back(boot);
  }
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return boots;
}

std::vector<std::byte> encode_durable_state(const DurableState& state) {
  CanonicalWriter writer;
  writer.u32(1);  // durable state format version
  writer.strong(state.epoch);
  writer.u64(state.boot.id.value());
  writer.u32(state.boot.process_id);
  writer.strong(state.sequence);
  writer.strong(state.evidence_revision);
  writer.strong(state.plan_counter);
  writer.strong(state.transition_counter);
  writer.strong(state.attempt_counter);
  writer.strong(state.boundary_counter);
  writer.strong(state.authorization_counter);
  writer.blob(state.topology_bytes);
  writer.blob(state.policy_bytes);
  writer.blob(state.evidence_bytes);
  writer.blob(state.boundary_bytes);
  writer.strong(state.boundary_plan);
  writer.digest(state.boundary_plan_digest);
  writer.u8(state.effect_state);
  writer.u32(static_cast<std::uint32_t>(state.plans.size()));
  for (const model::ContainmentPlan& plan : state.plans) {
    writer.blob(plan.encode());
  }
  writer.u32(static_cast<std::uint32_t>(state.attempts.size()));
  for (const model::ApplyAttempt& attempt : state.attempts) {
    writer.blob(encode_attempt(attempt));
  }
  writer.u32(static_cast<std::uint32_t>(state.transitions.size()));
  for (const model::TransitionDecision& decision : state.transitions) {
    writer.blob(encode_transition(decision));
  }
  writer.u32(static_cast<std::uint32_t>(state.detections.size()));
  for (const model::DetectionRecord& record : state.detections) {
    writer.blob(encode_detection(record));
  }
  writer.u32(static_cast<std::uint32_t>(state.fenced_boots.size()));
  for (const BootId boot : state.fenced_boots) {
    writer.strong(boot);
  }
  writer.u32(state.active_authorizations);
  return writer.data();
}

Result<DurableState> decode_durable_state(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint32_t version = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (version != 1) {
    return Status{StatusCode::VersionUnsupported, "durable state version is not supported"};
  }
  DurableState state;
  state.epoch = reader.strong<CoordinatorEpochTag>();
  state.boot.id = BootId{reader.u64()};
  state.boot.process_id = reader.u32();
  state.sequence = reader.strong<SequenceTag>();
  state.evidence_revision = reader.strong<model::EvidenceRevisionTag>();
  state.plan_counter = reader.strong<model::PlanGenerationTag>();
  state.transition_counter = reader.strong<model::TransitionGenerationTag>();
  state.attempt_counter = reader.strong<model::AttemptSequenceTag>();
  state.boundary_counter = reader.strong<model::BoundaryGenerationTag>();
  state.authorization_counter = reader.strong<SequenceTag>();
  const std::span<const std::byte> topology = reader.blob(kMaxSnapshotPayloadBytes);
  const std::span<const std::byte> policy = reader.blob(kMaxSnapshotPayloadBytes);
  const std::span<const std::byte> evidence = reader.blob(kMaxSnapshotPayloadBytes);
  const std::span<const std::byte> boundary = reader.blob(kMaxSnapshotPayloadBytes);
  if (!reader.ok()) {
    return reader.status();
  }
  state.topology_bytes.assign(topology.begin(), topology.end());
  state.policy_bytes.assign(policy.begin(), policy.end());
  state.evidence_bytes.assign(evidence.begin(), evidence.end());
  state.boundary_bytes.assign(boundary.begin(), boundary.end());
  state.boundary_plan = reader.strong<model::PlanGenerationTag>();
  state.boundary_plan_digest = reader.digest();
  state.effect_state = reader.u8();
  if (!reader.ok()) {
    return reader.status();
  }
  if (state.effect_state > kMaxEffectState) {
    return Status{StatusCode::InvalidArgument, "invalid durable effect state value"};
  }

  const std::uint32_t plan_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (plan_count > kMaxRetainedPlans) {
    return Status{StatusCode::LimitExceeded, "snapshot plan table exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < plan_count; ++i) {
    const std::span<const std::byte> encoded = reader.blob(kMaxRecordPayloadBytes);
    if (!reader.ok()) {
      return reader.status();
    }
    auto plan = model::ContainmentPlan::decode(encoded);
    if (!plan.ok()) {
      return plan.status();
    }
    state.plans.push_back(std::move(plan.value()));
  }

  const std::uint32_t attempt_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (attempt_count > kMaxRetainedAttempts) {
    return Status{StatusCode::LimitExceeded, "snapshot attempt table exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < attempt_count; ++i) {
    const std::span<const std::byte> encoded = reader.blob(kMaxRecordPayloadBytes);
    if (!reader.ok()) {
      return reader.status();
    }
    auto attempt = decode_attempt(encoded);
    if (!attempt.ok()) {
      return attempt.status();
    }
    state.attempts.push_back(std::move(attempt.value()));
  }

  const std::uint32_t transition_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (transition_count > kMaxRetainedTransitions) {
    return Status{StatusCode::LimitExceeded, "snapshot transition table exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < transition_count; ++i) {
    const std::span<const std::byte> encoded = reader.blob(kMaxRecordPayloadBytes);
    if (!reader.ok()) {
      return reader.status();
    }
    auto decision = decode_transition(encoded);
    if (!decision.ok()) {
      return decision.status();
    }
    state.transitions.push_back(std::move(decision.value()));
  }

  const std::uint32_t detection_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (detection_count > kMaxRetainedDetections) {
    return Status{StatusCode::LimitExceeded, "snapshot detection table exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < detection_count; ++i) {
    const std::span<const std::byte> encoded = reader.blob(kMaxRecordPayloadBytes);
    if (!reader.ok()) {
      return reader.status();
    }
    auto record = decode_detection(encoded);
    if (!record.ok()) {
      return record.status();
    }
    state.detections.push_back(std::move(record.value()));
  }

  const std::uint32_t fence_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (fence_count > kMaxFencedBoots) {
    return Status{StatusCode::LimitExceeded, "snapshot fence list exceeds the bound"};
  }
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    const BootId boot = reader.strong<BootIdTag>();
    if (!reader.ok()) {
      return reader.status();
    }
    state.fenced_boots.push_back(boot);
  }

  state.active_authorizations = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }

  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return state;
}

}  // namespace fcfn::runtime::codec
