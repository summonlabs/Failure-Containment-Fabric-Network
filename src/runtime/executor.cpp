// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Apply protocol. FCFN owns intent; the enforcement plane owns effect. This file
// implements the only path that can move a containment intent to verified
// effect, and it refuses every shortcut: acknowledgements are not application,
// mismatched observations become AMBIGUOUS, duplicate or late completions are
// rejected, and authority from another incarnation is fenced.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <utility>

#include "fcfn/runtime/coordinator.hpp"
#include "runtime_state.hpp"

namespace fcfn::runtime {
namespace {

void add_reason(model::Explanation& explanation, model::ReasonCode code, std::string_view text) {
  (void)explanation.add(code, text);
}

StatusResult check_attempt_authority(const RuntimeState& state, CoordinatorEpoch expected_epoch,
                                     const BootIdentity& expected_boot) {
  if (!(expected_epoch == state.epoch)) {
    return Status{StatusCode::StaleGeneration, "completion carries a stale coordinator epoch"};
  }
  if (!(expected_boot == state.boot)) {
    return Status{StatusCode::Fenced, "completion belongs to another process incarnation"};
  }
  return ok_result();
}

}  // namespace

Result<model::ApplyAttempt> ContainmentRuntime::submit_apply(const SubmitApplyRequest& request) {
  const std::lock_guard<std::mutex> guard(state_->lock);
  const Result<AuthorizationRecord> authorization =
      validate_authorization(*state_, request.authorization, true);
  if (!authorization.ok()) {
    return authorization.status();
  }
  const model::ContainmentPlan* plan = state_->find_plan(request.plan);
  if (plan == nullptr) {
    return Status{StatusCode::NotFound, "authorizing plan is not retained"};
  }
  if (!(plan->digest() == request.plan_digest)) {
    return Status{StatusCode::Conflict, "plan digest does not match the retained plan"};
  }
  if (!state_->boundary.has_value()) {
    return Status{StatusCode::StaleGeneration, "no containment boundary is current"};
  }
  const bool same_decision = state_->boundary_plan == plan->generation &&
                             state_->boundary->generation() == plan->boundary.generation();
  // Re-assertion: a newer plan whose membership is identical may be applied to
  // re-establish the surviving boundary under this incarnation's authority.
  // This is the only path by which effect state becomes current again after a
  // restart, and it never changes the governed scope.
  const bool reassertion = plan->boundary.same_members(state_->boundary.value());
  if (!same_decision && !reassertion) {
    return Status{StatusCode::StaleGeneration, "plan is not the current boundary decision"};
  }
  switch (state_->effect_state) {
    case model::EffectState::Submitted:
    case model::EffectState::Acknowledged:
    case model::EffectState::VerifiedApplied:
    case model::EffectState::VerifiedReleased:
    case model::EffectState::Fenced:
    case model::EffectState::Superseded:
      return Status{StatusCode::AlreadyExists, "an effect attempt is already in flight or complete"};
    case model::EffectState::NotRequested:
    case model::EffectState::Failed:
    case model::EffectState::Ambiguous:
      break;
  }

  state_->attempt_counter = AttemptSequence{state_->attempt_counter.value() + 1};
  model::ApplyAttempt attempt;
  attempt.id.plan = plan->generation;
  attempt.id.sequence = state_->attempt_counter;
  attempt.plan_digest = plan->digest();
  attempt.boundary_digest = plan->boundary.digest();
  attempt.boundary_generation = plan->boundary.generation();
  attempt.epoch = state_->epoch;
  attempt.boot = state_->boot;
  attempt.state = model::EffectState::Submitted;
  attempt.submitted_sequence = Sequence{state_->sequence.value() + 1};
  attempt.submitted_at_millis = state_->clock->now_millis();
  add_reason(attempt.explanation, model::ReasonCode::ApplySubmitted,
             "containment intent handed to the enforcement plane");

  const std::vector<std::byte> encoded = codec::encode_attempt(attempt);
  const VoidResult committed = state_->commit(store::RecordType::ApplyAttempt, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  state_->attempts.push_back(attempt);
  state_->effect_state = model::EffectState::Submitted;
  state_->evict_bounded_tables();
  ++state_->stats.apply_attempts;
  return attempt;
}

Result<model::ApplyAttempt> ContainmentRuntime::acknowledge(const model::ApplyAcknowledgement& ack) {
  const std::lock_guard<std::mutex> guard(state_->lock);
  model::ApplyAttempt* attempt = state_->find_attempt(ack.id);
  if (attempt == nullptr) {
    return Status{StatusCode::NotFound, "apply attempt is not retained"};
  }
  const StatusResult authority = check_attempt_authority(*state_, ack.expected_epoch, ack.expected_boot);
  if (!authority.ok()) {
    return authority.status();
  }
  if (attempt->terminal()) {
    ++state_->stats.duplicate_completions_rejected;
    return Status{StatusCode::AlreadyExists, "attempt already reached a terminal state"};
  }
  if (!(ack.applier_sequence > attempt->applier_sequence)) {
    return Status{StatusCode::SequenceRegression, "applier sequence did not advance"};
  }
  if (!(ack.observed_boundary_digest == attempt->boundary_digest)) {
    attempt->state = model::EffectState::Ambiguous;
    add_reason(attempt->explanation, model::ReasonCode::EffectVerificationDigestMismatch,
               "acknowledgement observed a different boundary than the intent");
    const std::vector<std::byte> encoded = codec::encode_attempt(*attempt);
    const VoidResult committed = state_->commit(store::RecordType::ApplyAcknowledgement, encoded);
    if (!committed.ok()) {
      return committed.status();
    }
    return Status{StatusCode::Conflict, "acknowledgement boundary digest does not match the intent"};
  }

  attempt->applier_epoch = ack.applier_epoch;
  attempt->applier_boot = ack.applier_boot;
  attempt->applier_sequence = ack.applier_sequence;
  attempt->observed_boundary_digest = ack.observed_boundary_digest;
  attempt->acknowledged_at_millis = state_->clock->now_millis();
  if (ack.accepted) {
    attempt->state = model::EffectState::Acknowledged;
    add_reason(attempt->explanation, model::ReasonCode::ApplyAcknowledged,
               "enforcement plane acknowledged the request; effect is not yet verified");
  } else {
    attempt->state = model::EffectState::Failed;
    add_reason(attempt->explanation, model::ReasonCode::EffectNotApplied,
               "enforcement plane refused the request");
  }

  const std::vector<std::byte> encoded = codec::encode_attempt(*attempt);
  const VoidResult committed = state_->commit(store::RecordType::ApplyAcknowledgement, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  const model::ApplyAttempt result = *attempt;
  state_->recompute_effect_state();
  ++state_->stats.acknowledgements;
  return result;
}

Result<model::ApplyAttempt> ContainmentRuntime::verify_effect(const model::EffectVerification& report) {
  const std::lock_guard<std::mutex> guard(state_->lock);
  model::ApplyAttempt* attempt = state_->find_attempt(report.id);
  if (attempt == nullptr) {
    return Status{StatusCode::NotFound, "apply attempt is not retained"};
  }
  const StatusResult authority =
      check_attempt_authority(*state_, report.expected_epoch, report.expected_boot);
  if (!authority.ok()) {
    return authority.status();
  }
  switch (attempt->state) {
    case model::EffectState::VerifiedApplied:
    case model::EffectState::VerifiedReleased:
      ++state_->stats.duplicate_completions_rejected;
      return Status{StatusCode::AlreadyExists, "attempt already has a verified effect"};
    case model::EffectState::Fenced:
    case model::EffectState::Superseded:
      return Status{StatusCode::Fenced, "attempt was invalidated by a later decision"};
    case model::EffectState::NotRequested:
    case model::EffectState::Submitted:
    case model::EffectState::Acknowledged:
    case model::EffectState::Failed:
    case model::EffectState::Ambiguous:
      break;
  }
  if (!(report.applier_sequence > attempt->applier_sequence)) {
    return Status{StatusCode::SequenceRegression, "applier sequence did not advance"};
  }

  attempt->applier_epoch = report.applier_epoch;
  attempt->applier_boot = report.applier_boot;
  attempt->applier_sequence = report.applier_sequence;
  attempt->observed_boundary_digest = report.observed_boundary_digest;
  attempt->verified_at_millis = state_->clock->now_millis();

  const bool digest_matches = report.observed_boundary_digest == attempt->boundary_digest;
  switch (report.observation) {
    case model::EffectObservation::Applied: {
      if (!digest_matches) {
        attempt->state = model::EffectState::Ambiguous;
        add_reason(attempt->explanation, model::ReasonCode::EffectVerificationDigestMismatch,
                   "observed boundary differs from the intent");
        const std::vector<std::byte> encoded = codec::encode_attempt(*attempt);
        const VoidResult committed = state_->commit(store::RecordType::EffectVerification, encoded);
        if (!committed.ok()) {
          return committed.status();
        }
        state_->recompute_effect_state();
        return Status{StatusCode::Conflict, "observed boundary digest does not match the intent"};
      }
      attempt->state = model::EffectState::VerifiedApplied;
      add_reason(attempt->explanation, model::ReasonCode::EffectVerified,
                 "enforcement plane observed the intended boundary applied");
      break;
    }
    case model::EffectObservation::Released: {
      if (!digest_matches) {
        attempt->state = model::EffectState::Ambiguous;
        add_reason(attempt->explanation, model::ReasonCode::EffectVerificationDigestMismatch,
                   "observed release differs from the intent");
        const std::vector<std::byte> encoded = codec::encode_attempt(*attempt);
        const VoidResult committed = state_->commit(store::RecordType::EffectVerification, encoded);
        if (!committed.ok()) {
          return committed.status();
        }
        state_->recompute_effect_state();
        return Status{StatusCode::Conflict, "observed release digest does not match the intent"};
      }
      attempt->state = model::EffectState::VerifiedReleased;
      add_reason(attempt->explanation, model::ReasonCode::EffectVerified,
                 "enforcement plane observed the boundary released");
      break;
    }
    case model::EffectObservation::NotApplied: {
      attempt->state = model::EffectState::Failed;
      add_reason(attempt->explanation, model::ReasonCode::EffectNotApplied,
                 "enforcement plane reports the boundary is not applied");
      break;
    }
    case model::EffectObservation::PartiallyApplied:
    case model::EffectObservation::Unknown: {
      attempt->state = model::EffectState::Ambiguous;
      add_reason(attempt->explanation, model::ReasonCode::EffectAmbiguous,
                 "effect state is not provable from the report");
      break;
    }
  }

  const std::vector<std::byte> encoded = codec::encode_attempt(*attempt);
  const VoidResult committed = state_->commit(store::RecordType::EffectVerification, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  const model::ApplyAttempt result = *attempt;
  if (model::is_verified_effect(result.state)) {
    state_->effect_requires_reverification = false;
    const model::ContainmentPlan* plan = state_->find_plan(result.id.plan);
    if (plan != nullptr && state_->boundary.has_value() &&
        plan->boundary.same_members(state_->boundary.value())) {
      // The verified attempt belongs to a re-asserted decision with identical
      // membership: adopt it so the verified effect is bound to the generation
      // the enforcement plane actually established.
      state_->boundary = plan->boundary;
      state_->boundary_plan = plan->generation;
      state_->boundary_plan_digest = plan->digest();
      state_->boundary_counter = plan->boundary.generation();
    }
  }
  state_->recompute_effect_state();
  ++state_->stats.verifications;
  return result;
}

Result<model::ApplyAttempt> ContainmentRuntime::attempt(PlanGeneration plan,
                                                        AttemptSequence sequence) const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  const model::ApplyAttemptId id{plan, sequence};
  const model::ApplyAttempt* found = state_->find_attempt(id);
  if (found == nullptr) {
    return Status{StatusCode::NotFound, "apply attempt is not retained"};
  }
  return *found;
}

std::vector<model::ApplyAttempt> ContainmentRuntime::attempts() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return std::vector<model::ApplyAttempt>(state_->attempts.begin(), state_->attempts.end());
}

}  // namespace fcfn::runtime
