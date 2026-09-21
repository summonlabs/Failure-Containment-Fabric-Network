// FCFN - wire message payloads.
//
// The wire format is versioned independently of the durable format. Both are
// canonical and both are total decoders, but they are allowed to evolve
// separately: a durable record is forever, a wire message is per session.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_NET_MESSAGES_HPP
#define FCFN_NET_MESSAGES_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "fcfn/core/result.hpp"
#include "fcfn/model/attempt.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/policy.hpp"
#include "fcfn/model/topology.hpp"
#include "fcfn/model/transition.hpp"
#include "fcfn/runtime/coordinator.hpp"

namespace fcfn::net {

[[nodiscard]] std::vector<std::byte> encode_detection_arguments(const model::FailureObservation& observation);
[[nodiscard]] Result<model::FailureObservation> decode_detection_arguments(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_authorization_reference(runtime::AuthorizationId authorization);
[[nodiscard]] Result<runtime::AuthorizationId> decode_authorization_reference(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_plan_generation(PlanGeneration generation);
[[nodiscard]] Result<PlanGeneration> decode_plan_generation(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_submit_apply_arguments(const runtime::SubmitApplyRequest& request);
[[nodiscard]] Result<runtime::SubmitApplyRequest> decode_submit_apply_arguments(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_transition_arguments(const model::TransitionRequest& request);
[[nodiscard]] Result<model::TransitionRequest> decode_transition_arguments(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_acknowledgement_arguments(const model::ApplyAcknowledgement& ack);
[[nodiscard]] Result<model::ApplyAcknowledgement> decode_acknowledgement_arguments(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_verification_arguments(const model::EffectVerification& report);
[[nodiscard]] Result<model::EffectVerification> decode_verification_arguments(std::span<const std::byte> bytes);

/// Attempt reference used by the inspection operations.
struct AttemptReference {
  PlanGeneration plan{};
  AttemptSequence sequence{};
};

[[nodiscard]] std::vector<std::byte> encode_attempt_reference(const AttemptReference& reference);
[[nodiscard]] Result<AttemptReference> decode_attempt_reference(std::span<const std::byte> bytes);

}  // namespace fcfn::net

#endif  // FCFN_NET_MESSAGES_HPP
