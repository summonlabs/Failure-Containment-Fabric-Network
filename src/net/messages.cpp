// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/net/messages.hpp"

#include "fcfn/core/canonical.hpp"

namespace fcfn::net {
namespace {

constexpr std::uint8_t kMaxEffectObservation = 4;
constexpr std::uint8_t kMaxEvidenceState = 2;
constexpr std::uint8_t kMaxTransitionKind = 2;

}  // namespace

std::vector<std::byte> encode_detection_arguments(const model::FailureObservation& observation) {
  CanonicalWriter writer;
  writer.resource_id(observation.resource);
  writer.u8(static_cast<std::uint8_t>(observation.state));
  writer.strong(observation.generation);
  writer.digest(observation.source_digest);
  writer.u64(observation.observed_at_millis);
  return writer.data();
}

Result<model::FailureObservation> decode_detection_arguments(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::FailureObservation observation;
  auto resource = reader.resource_id();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!resource.ok()) {
    return resource.status();
  }
  observation.resource = std::move(resource.value());
  const std::uint8_t state = reader.u8();
  observation.generation = reader.strong<EvidenceGenerationTag>();
  observation.source_digest = reader.digest();
  observation.observed_at_millis = reader.u64();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (state > kMaxEvidenceState) {
    return Status{StatusCode::InvalidArgument, "invalid evidence state value"};
  }
  observation.state = static_cast<model::EvidenceState>(state);
  return observation;
}

std::vector<std::byte> encode_authorization_reference(runtime::AuthorizationId authorization) {
  CanonicalWriter writer;
  writer.strong(authorization);
  return writer.data();
}

Result<runtime::AuthorizationId> decode_authorization_reference(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const runtime::AuthorizationId authorization = reader.strong<model::AuthorizationIdTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (authorization.is_zero()) {
    return Status{StatusCode::Unauthorized, "authorization reference is zero"};
  }
  return authorization;
}

std::vector<std::byte> encode_plan_generation(PlanGeneration generation) {
  CanonicalWriter writer;
  writer.strong(generation);
  return writer.data();
}

Result<PlanGeneration> decode_plan_generation(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const PlanGeneration generation = reader.strong<PlanGenerationTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "plan generation is zero"};
  }
  return generation;
}

std::vector<std::byte> encode_submit_apply_arguments(const runtime::SubmitApplyRequest& request) {
  CanonicalWriter writer;
  writer.strong(request.authorization);
  writer.strong(request.plan);
  writer.digest(request.plan_digest);
  return writer.data();
}

Result<runtime::SubmitApplyRequest> decode_submit_apply_arguments(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  runtime::SubmitApplyRequest request;
  request.authorization = reader.strong<model::AuthorizationIdTag>();
  request.plan = reader.strong<PlanGenerationTag>();
  request.plan_digest = reader.digest();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (request.authorization.is_zero()) {
    return Status{StatusCode::Unauthorized, "authorization reference is zero"};
  }
  return request;
}

std::vector<std::byte> encode_transition_arguments(const model::TransitionRequest& request) {
  CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(request.kind));
  writer.strong(request.plan);
  writer.digest(request.plan_digest);
  writer.strong(request.expected_current_boundary);
  writer.boolean(request.expected_released);
  writer.strong(request.authorization);
  return writer.data();
}

Result<model::TransitionRequest> decode_transition_arguments(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint8_t kind = reader.u8();
  model::TransitionRequest request;
  request.plan = reader.strong<PlanGenerationTag>();
  request.plan_digest = reader.digest();
  request.expected_current_boundary = reader.strong<BoundaryGenerationTag>();
  request.expected_released = reader.boolean();
  request.authorization = reader.strong<model::AuthorizationIdTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (kind > kMaxTransitionKind) {
    return Status{StatusCode::InvalidArgument, "invalid transition kind value"};
  }
  if (request.authorization.is_zero()) {
    return Status{StatusCode::Unauthorized, "transition carries no authorization"};
  }
  request.kind = static_cast<model::TransitionKind>(kind);
  return request;
}

std::vector<std::byte> encode_acknowledgement_arguments(const model::ApplyAcknowledgement& ack) {
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

Result<model::ApplyAcknowledgement> decode_acknowledgement_arguments(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::ApplyAcknowledgement ack;
  ack.id.plan = reader.strong<PlanGenerationTag>();
  ack.id.sequence = reader.strong<AttemptSequenceTag>();
  ack.expected_epoch = reader.strong<CoordinatorEpochTag>();
  ack.expected_boot.id = BootId{reader.u64()};
  ack.expected_boot.process_id = reader.u32();
  ack.applier_epoch = reader.strong<ApplierEpochTag>();
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

std::vector<std::byte> encode_verification_arguments(const model::EffectVerification& report) {
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

Result<model::EffectVerification> decode_verification_arguments(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  model::EffectVerification report;
  report.id.plan = reader.strong<PlanGenerationTag>();
  report.id.sequence = reader.strong<AttemptSequenceTag>();
  report.expected_epoch = reader.strong<CoordinatorEpochTag>();
  report.expected_boot.id = BootId{reader.u64()};
  report.expected_boot.process_id = reader.u32();
  report.applier_epoch = reader.strong<ApplierEpochTag>();
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

std::vector<std::byte> encode_attempt_reference(const AttemptReference& reference) {
  CanonicalWriter writer;
  writer.strong(reference.plan);
  writer.strong(reference.sequence);
  return writer.data();
}

Result<AttemptReference> decode_attempt_reference(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  AttemptReference reference;
  reference.plan = reader.strong<PlanGenerationTag>();
  reference.sequence = reader.strong<AttemptSequenceTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return reference;
}

}  // namespace fcfn::net
