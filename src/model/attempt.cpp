// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/attempt.hpp"

#include <cstdio>

namespace fcfn::model {
namespace {

struct EffectToken {
  EffectState value;
  const char* token;
};

constexpr EffectToken kEffectTokens[] = {
    {EffectState::NotRequested, "not_requested"},
    {EffectState::Submitted, "submitted"},
    {EffectState::Acknowledged, "acknowledged"},
    {EffectState::VerifiedApplied, "verified_applied"},
    {EffectState::VerifiedReleased, "verified_released"},
    {EffectState::Failed, "failed"},
    {EffectState::Ambiguous, "ambiguous"},
    {EffectState::Fenced, "fenced"},
    {EffectState::Superseded, "superseded"},
};

struct ObservationToken {
  EffectObservation value;
  const char* token;
};

constexpr ObservationToken kObservationTokens[] = {
    {EffectObservation::Applied, "applied"},
    {EffectObservation::Released, "released"},
    {EffectObservation::PartiallyApplied, "partially_applied"},
    {EffectObservation::NotApplied, "not_applied"},
    {EffectObservation::Unknown, "unknown"},
};

}  // namespace

const char* to_string(EffectState value) noexcept {
  for (const EffectToken& entry : kEffectTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_effect_state";
}

bool parse_effect_state(std::string_view token, EffectState& out) noexcept {
  for (const EffectToken& entry : kEffectTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool is_verified_effect(EffectState value) noexcept {
  return value == EffectState::VerifiedApplied || value == EffectState::VerifiedReleased;
}

const char* to_string(EffectObservation value) noexcept {
  for (const ObservationToken& entry : kObservationTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_effect_observation";
}

bool parse_effect_observation(std::string_view token, EffectObservation& out) noexcept {
  for (const ObservationToken& entry : kObservationTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string ApplyAttemptId::render() const {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "plan-%llu/attempt-%llu", static_cast<unsigned long long>(plan.value()),
                static_cast<unsigned long long>(sequence.value()));
  return std::string{buffer};
}

bool ApplyAttempt::terminal() const noexcept {
  switch (state) {
    case EffectState::NotRequested:
    case EffectState::Submitted:
    case EffectState::Acknowledged:
      return false;
    case EffectState::VerifiedApplied:
    case EffectState::VerifiedReleased:
    case EffectState::Failed:
    case EffectState::Ambiguous:
    case EffectState::Fenced:
    case EffectState::Superseded:
      return true;
  }
  return true;
}

}  // namespace fcfn::model
