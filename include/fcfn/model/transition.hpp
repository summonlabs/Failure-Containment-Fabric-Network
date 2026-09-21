// FCFN - generation-bound containment transitions.
//
// Expansion, contraction, and release are decisions, not commands. Each one is
// bound to the exact boundary generation it supersedes and to the authority
// that authorizes it, and each one carries an explicit outcome. Release and
// contraction require verified effect state for the boundary being changed;
// unverified effect is reported as indeterminate, never assumed.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_TRANSITION_HPP
#define FCFN_MODEL_TRANSITION_HPP

#include <cstdint>
#include <string>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/model/attempt.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/boundary.hpp"
#include "fcfn/model/explanation.hpp"
#include "fcfn/model/plan.hpp"

namespace fcfn::model {

enum class TransitionKind : std::uint8_t {
  Expand = 0,
  Contract = 1,
  Release = 2,
};

[[nodiscard]] const char* to_string(TransitionKind value) noexcept;

enum class TransitionStatus : std::uint8_t {
  /// The transition was accepted and durably recorded.
  Accepted = 0,
  /// Refused by policy, authority, or effect state.
  Denied = 1,
  /// Cannot be decided: effect state or evidence is not provable.
  Indeterminate = 2,
  /// The referenced generation is no longer current.
  Stale = 3,
  /// The request itself is malformed.
  Invalid = 4,
};

[[nodiscard]] const char* to_string(TransitionStatus value) noexcept;

/// Request for a containment transition.
struct TransitionRequest {
  TransitionKind kind{TransitionKind::Expand};
  /// Plan that authorizes the resulting boundary.
  PlanGeneration plan{};
  Digest plan_digest{};
  /// Boundary generation the caller believes is current (fencing check).
  BoundaryGeneration expected_current_boundary{};
  /// True when the caller believes no boundary is currently applied.
  bool expected_released{false};
  /// Authorization presented by the caller. Its bound authority vector must
  /// match the runtime's current authority for the transition to be legal.
  AuthorizationId authorization{};
};

/// Recorded transition decision.
struct TransitionDecision {
  TransitionGeneration generation{};
  TransitionKind kind{TransitionKind::Expand};
  TransitionStatus status{TransitionStatus::Invalid};
  StatusCode code{StatusCode::InvalidArgument};

  BoundaryGeneration from_generation{};
  BoundaryGeneration to_generation{};
  bool to_released{false};
  Digest from_digest{};
  Digest to_digest{};
  Digest plan_digest{};

  EffectState required_effect_state{EffectState::NotRequested};
  EffectState observed_effect_state{EffectState::NotRequested};
  bool effect_verified{false};

  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  Sequence sequence{};
  Explanation explanation{};

  [[nodiscard]] Digest compute_digest() const;
  [[nodiscard]] std::string to_json() const;
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_TRANSITION_HPP
