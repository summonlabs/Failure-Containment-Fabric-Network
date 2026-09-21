// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/transition.hpp"

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/json.hpp"

namespace fcfn::model {
namespace {

struct KindToken {
  TransitionKind value;
  const char* token;
};

constexpr KindToken kKindTokens[] = {
    {TransitionKind::Expand, "expand"},
    {TransitionKind::Contract, "contract"},
    {TransitionKind::Release, "release"},
};

struct StatusToken {
  TransitionStatus value;
  const char* token;
};

constexpr StatusToken kStatusTokens[] = {
    {TransitionStatus::Accepted, "accepted"},
    {TransitionStatus::Denied, "denied"},
    {TransitionStatus::Indeterminate, "indeterminate"},
    {TransitionStatus::Stale, "stale"},
    {TransitionStatus::Invalid, "invalid"},
};

}  // namespace

const char* to_string(TransitionKind value) noexcept {
  for (const KindToken& entry : kKindTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_transition_kind";
}

const char* to_string(TransitionStatus value) noexcept {
  for (const StatusToken& entry : kStatusTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_transition_status";
}

Digest TransitionDecision::compute_digest() const {
  CanonicalWriter writer;
  writer.strong(generation);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u8(static_cast<std::uint8_t>(status));
  writer.u16(static_cast<std::uint16_t>(code));
  writer.strong(from_generation);
  writer.strong(to_generation);
  writer.boolean(to_released);
  writer.digest(from_digest);
  writer.digest(to_digest);
  writer.digest(plan_digest);
  writer.u8(static_cast<std::uint8_t>(required_effect_state));
  writer.u8(static_cast<std::uint8_t>(observed_effect_state));
  writer.boolean(effect_verified);
  writer.strong(epoch);
  writer.u64(boot.id.value());
  writer.u32(boot.process_id);
  writer.strong(sequence);
  writer.u32(static_cast<std::uint32_t>(explanation.items().size()));
  for (const ExplanationItem& item : explanation.items()) {
    writer.u16(static_cast<std::uint16_t>(item.code));
    writer.resource_id(item.subject);
    writer.u64(item.value);
    writer.text(item.text);
  }
  return writer.digest();
}

std::string TransitionDecision::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.member("transition_generation", generation.value());
  writer.member("kind", to_string(kind));
  writer.member("status", to_string(status));
  writer.member("code", fcfn::to_string(code));
  writer.member("from_boundary_generation", from_generation.value());
  writer.member("to_boundary_generation", to_generation.value());
  writer.member("to_released", to_released);
  writer.member("from_digest", from_digest.hex());
  writer.member("to_digest", to_digest.hex());
  writer.member("plan_digest", plan_digest.hex());
  writer.member("required_effect_state", to_string(required_effect_state));
  writer.member("observed_effect_state", to_string(observed_effect_state));
  writer.member("effect_verified", effect_verified);
  writer.member("epoch", epoch.value());
  writer.member("boot_id", boot.id.value());
  writer.member("boot_pid", static_cast<std::uint64_t>(boot.process_id));
  writer.member("sequence", sequence.value());
  writer.member("decision_digest", compute_digest().hex());
  writer.key("explanation");
  writer.begin_array();
  for (const ExplanationItem& item : explanation.items()) {
    writer.begin_object();
    writer.member("code", to_string(item.code));
    writer.member("subject", item.subject.value());
    writer.member("value", item.value);
    writer.member("text", item.text);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

}  // namespace fcfn::model
