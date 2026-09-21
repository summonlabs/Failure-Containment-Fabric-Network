// FCFN - apply protocol: intent, acknowledgement, verified effect.
//
// FCFN owns containment intent. It does not enforce isolation and never claims
// that intent took effect. Only an effect verification bound to the same
// attempt, applier incarnation, and identical boundary digest moves an attempt
// to VERIFIED_APPLIED. Acknowledgement is not application; a duplicate or late
// completion is rejected rather than replayed as success.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_ATTEMPT_HPP
#define FCFN_MODEL_ATTEMPT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/explanation.hpp"

namespace fcfn::model {

/// Lifecycle state of a containment application attempt.
enum class EffectState : std::uint8_t {
  NotRequested = 0,
  Submitted = 1,
  Acknowledged = 2,
  VerifiedApplied = 3,
  VerifiedReleased = 4,
  Failed = 5,
  /// The attempt may or may not have taken effect. Never treated as success.
  Ambiguous = 6,
  /// Superseded by a later incarnation/epoch; must be revalidated before reuse.
  Fenced = 7,
  /// Replaced by a newer boundary decision.
  Superseded = 8,
};

[[nodiscard]] const char* to_string(EffectState value) noexcept;
[[nodiscard]] bool parse_effect_state(std::string_view token, EffectState& out) noexcept;
[[nodiscard]] bool is_verified_effect(EffectState value) noexcept;

/// Composite attempt identity: the plan it applies plus a monotonic sequence.
struct ApplyAttemptId {
  PlanGeneration plan{};
  AttemptSequence sequence{};

  friend bool operator==(const ApplyAttemptId& a, const ApplyAttemptId& b) noexcept {
    return a.plan == b.plan && a.sequence == b.sequence;
  }
  friend bool operator<(const ApplyAttemptId& a, const ApplyAttemptId& b) noexcept {
    return a.plan != b.plan ? a.plan < b.plan : a.sequence < b.sequence;
  }
  [[nodiscard]] std::string render() const;
};

/// Observation reported by the enforcement plane.
enum class EffectObservation : std::uint8_t {
  Applied = 0,
  Released = 1,
  PartiallyApplied = 2,
  NotApplied = 3,
  Unknown = 4,
};

[[nodiscard]] const char* to_string(EffectObservation value) noexcept;
[[nodiscard]] bool parse_effect_observation(std::string_view token, EffectObservation& out) noexcept;

/// Recorded attempt with its lifecycle evidence.
struct ApplyAttempt {
  ApplyAttemptId id{};
  Digest plan_digest{};
  Digest boundary_digest{};
  BoundaryGeneration boundary_generation{};
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  EffectState state{EffectState::Submitted};
  Sequence submitted_sequence{};
  ApplierEpoch applier_epoch{};
  BootIdentity applier_boot{};
  /// Monotone per-attempt sequence reported by the enforcement plane.
  Sequence applier_sequence{};
  Digest observed_boundary_digest{};
  std::uint64_t submitted_at_millis{0};
  std::uint64_t acknowledged_at_millis{0};
  std::uint64_t verified_at_millis{0};
  Explanation explanation{};

  [[nodiscard]] bool terminal() const noexcept;
};

/// Enforcement-plane acknowledgement that the request was received.
struct ApplyAcknowledgement {
  ApplyAttemptId id{};
  CoordinatorEpoch expected_epoch{};
  BootIdentity expected_boot{};
  ApplierEpoch applier_epoch{};
  BootIdentity applier_boot{};
  Sequence applier_sequence{};
  Digest observed_boundary_digest{};
  bool accepted{false};
};

/// Enforcement-plane report of what it believes is in effect.
struct EffectVerification {
  ApplyAttemptId id{};
  CoordinatorEpoch expected_epoch{};
  BootIdentity expected_boot{};
  ApplierEpoch applier_epoch{};
  BootIdentity applier_boot{};
  Sequence applier_sequence{};
  Digest observed_boundary_digest{};
  EffectObservation observation{EffectObservation::Unknown};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_ATTEMPT_HPP
