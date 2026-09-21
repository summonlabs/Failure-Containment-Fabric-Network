// FCFN adversarial suite: hostile input to every public runtime entry point.
//
// Product proposition proved here: no malformed, contradictory, or misbound
// request is ever silently accepted. Every entry point answers with an explicit
// status, contradictory evidence at one generation is a conflict rather than an
// update, an unverified effect never becomes verified, and a refused request
// leaves the runtime exactly as it was.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adversarial_support.hpp"

namespace {

using namespace fcfn::test::adversarial;

using fcfn::model::ApplierEpoch;
using fcfn::model::ApplyAcknowledgement;
using fcfn::model::ApplyAttempt;
using fcfn::model::BootId;
using fcfn::model::BootIdentity;
using fcfn::model::BoundaryGeneration;
using fcfn::model::ContainmentClaim;
using fcfn::model::Digest;
using fcfn::model::EffectObservation;
using fcfn::model::EffectState;
using fcfn::model::EffectVerification;
using fcfn::model::EvidenceCurrency;
using fcfn::model::EvidenceState;
using fcfn::model::PlanGeneration;
using fcfn::model::ReasonCode;
using fcfn::model::TransitionGeneration;
using fcfn::model::TransitionKind;
using fcfn::model::TransitionRequest;
using fcfn::model::TransitionStatus;
using fcfn::runtime::AuthorizationId;
using fcfn::runtime::PlanRequest;
using fcfn::runtime::SubmitApplyRequest;

/// A live runtime with a planned, then expanded, containment boundary.
struct Arrangement {
  explicit Arrangement(fcfn::Clock* clock) : runtime(open_or_fail(non_durable_config(), clock)) {}

  std::unique_ptr<ContainmentRuntime> runtime;
  AuthorizationId authorization{};
  PlanGeneration plan{};
  Digest plan_digest{};
  std::uint64_t boundary_generation{0};
};

void plan_boundary(Arrangement& arrangement) {
  source_detection(*arrangement.runtime, 1);
  const Result<fcfn::runtime::AuthorizationRecord> authorization =
      arrangement.runtime->authorize();
  FCFN_CHECK_OK(authorization);
  arrangement.authorization = authorization.value().id;
  const Result<ContainmentPlan> planned =
      arrangement.runtime->plan(PlanRequest{arrangement.authorization});
  FCFN_CHECK_OK(planned);
  FCFN_CHECK(planned.value().claim == ContainmentClaim::ProvenContainment);
  arrangement.plan = planned.value().generation;
  arrangement.plan_digest = planned.value().digest();
  arrangement.boundary_generation = planned.value().boundary.generation().value();
}

void expand_boundary(Arrangement& arrangement) {
  TransitionRequest request;
  request.kind = TransitionKind::Expand;
  request.plan = arrangement.plan;
  request.plan_digest = arrangement.plan_digest;
  request.expected_current_boundary = BoundaryGeneration{0};
  request.expected_released = true;
  request.authorization = arrangement.authorization;
  const Result<fcfn::model::TransitionDecision> applied = arrangement.runtime->transition(request);
  FCFN_CHECK_OK(applied);
  FCFN_CHECK(applied.value().status == TransitionStatus::Accepted);
}

ApplyAttempt submit(Arrangement& arrangement) {
  const Result<ApplyAttempt> attempt = arrangement.runtime->submit_apply(
      SubmitApplyRequest{arrangement.authorization, arrangement.plan, arrangement.plan_digest});
  FCFN_CHECK_OK(attempt);
  return attempt.value();
}

void verify(Arrangement& arrangement, const ApplyAttempt& attempt, EffectObservation observation,
            const Digest& observed, std::uint64_t applier_sequence) {
  const fcfn::model::AuthorityVector authority = arrangement.runtime->authority();
  EffectVerification report;
  report.id = attempt.id;
  report.expected_epoch = authority.epoch;
  report.expected_boot = authority.boot;
  report.applier_epoch = ApplierEpoch{1};
  report.applier_boot = BootIdentity{BootId{0xabcdu}, 700};
  report.applier_sequence = fcfn::Sequence{applier_sequence};
  report.observed_boundary_digest = observed;
  report.observation = observation;
  const Result<ApplyAttempt> verified = arrangement.runtime->verify_effect(report);
  FCFN_CHECK_OK(verified);
}

}  // namespace

FCFN_TEST(adversarial, opening_a_runtime_with_hostile_configuration_is_refused) {
  fcfn::ManualClock clock(1000);
  expect_result("null clock", ContainmentRuntime::open(non_durable_config(), nullptr),
                StatusCode::InvalidArgument);

  RuntimeConfig no_root = non_durable_config();
  no_root.persist = true;
  no_root.store_root.clear();
  expect_result("durable runtime without a store root", ContainmentRuntime::open(no_root, &clock),
                StatusCode::InvalidArgument);

  RuntimeConfig bad_plans = non_durable_config();
  bad_plans.max_retained_plans = 0;
  expect_result("zero plan retention bound", ContainmentRuntime::open(bad_plans, &clock),
                StatusCode::InvalidArgument);
  bad_plans.max_retained_plans = fcfn::kMaxRetainedPlans + 1;
  expect_result("plan retention bound above the bound",
                ContainmentRuntime::open(bad_plans, &clock), StatusCode::InvalidArgument);

  RuntimeConfig bad_attempts = non_durable_config();
  bad_attempts.max_retained_attempts = 0;
  expect_result("zero attempt retention bound", ContainmentRuntime::open(bad_attempts, &clock),
                StatusCode::InvalidArgument);

  RuntimeConfig bad_authorizations = non_durable_config();
  bad_authorizations.max_active_authorizations = 0;
  expect_result("zero authorization bound", ContainmentRuntime::open(bad_authorizations, &clock),
                StatusCode::InvalidArgument);
  bad_authorizations.max_active_authorizations = fcfn::kMaxSessions + 1;
  expect_result("authorization bound above the bound",
                ContainmentRuntime::open(bad_authorizations, &clock), StatusCode::InvalidArgument);
}

FCFN_TEST(adversarial, contradictory_evidence_at_one_generation_is_a_conflict_not_an_update) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);

  FCFN_CHECK_OK(runtime->record_detection(source_observation(1, 0x11, 0x22)));
  expect_result("same generation, different digest",
                runtime->record_detection(source_observation(1, 0x99, 0x88)),
                StatusCode::Conflict);
  expect_result("same generation, same digest, different state",
                runtime->record_detection(observation("l0c0", EvidenceState::Absent, 1, 0x11, 0x22)),
                StatusCode::Conflict);

  // A repeated identical report is not a conflict and does not change evidence.
  const Result<fcfn::model::DetectionRecord> repeated =
      runtime->record_detection(source_observation(1, 0x11, 0x22));
  FCFN_CHECK_OK(repeated);
  FCFN_CHECK_EQ(repeated.value().generation.value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(runtime->stats().detections_unchanged, static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(runtime->evidence().value().entries().size(), static_cast<std::size_t>(1));

  // The rejected contradiction poisons the claim: the runtime may not compute a
  // containment plan as if the evidence were coherent.
  {
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    const Result<ContainmentPlan> planned = runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    FCFN_CHECK(planned.value().claim == ContainmentClaim::Indeterminate);
    FCFN_CHECK(planned.value().currency == EvidenceCurrency::Conflicting);
    FCFN_CHECK(planned.value().explanation.contains(ReasonCode::EvidenceConflicting));
  }

  // Only a strictly newer generation clears the conflict.
  FCFN_CHECK_OK(runtime->record_detection(source_observation(2, 0x33, 0x44)));
  expect_result("regressed generation", runtime->record_detection(source_observation(1, 0x11, 0x22)),
                StatusCode::SequenceRegression);
  {
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    const Result<ContainmentPlan> planned = runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    FCFN_CHECK(planned.value().claim == ContainmentClaim::ProvenContainment);
    FCFN_CHECK(planned.value().currency == EvidenceCurrency::Current);
  }
}

FCFN_TEST(adversarial, malformed_requests_to_planning_and_lookup_are_refused) {
  fcfn::ManualClock clock(1000);
  Arrangement arrangement(&clock);
  plan_boundary(arrangement);

  expect_result("zero authorization id", arrangement.runtime->plan(PlanRequest{}),
                StatusCode::Unauthorized);
  expect_result("unknown authorization id",
                arrangement.runtime->plan(PlanRequest{AuthorizationId{9999}}),
                StatusCode::Unauthorized);
  expect_result("unknown plan generation",
                arrangement.runtime->plan_by_generation(PlanGeneration{0}), StatusCode::NotFound);
  expect_result("unknown transition generation",
                arrangement.runtime->transition_by_generation(TransitionGeneration{0}),
                StatusCode::NotFound);
  expect_result("attempt that was never submitted",
                arrangement.runtime->attempt(PlanGeneration{4}, fcfn::AttemptSequence{4}),
                StatusCode::NotFound);
  expect_void("checkpoint without durability", arrangement.runtime->checkpoint(),
              StatusCode::Unsupported);

  // The boundary is not current yet: nothing may be submitted against it.
  expect_result("release without a current boundary",
                arrangement.runtime->current_boundary(), StatusCode::NotFound);

  // A transition for a plan the runtime never computed is refused with an
  // explicit decision, not by silently doing nothing.
  TransitionRequest unknown;
  unknown.kind = TransitionKind::Expand;
  unknown.plan = PlanGeneration{77};
  unknown.plan_digest = Digest{7, 7};
  unknown.expected_current_boundary = BoundaryGeneration{0};
  unknown.expected_released = true;
  unknown.authorization = arrangement.authorization;
  const Result<fcfn::model::TransitionDecision> refused = arrangement.runtime->transition(unknown);
  FCFN_CHECK_OK(refused);
  FCFN_CHECK(refused.value().status == TransitionStatus::Stale);
  expect_code("transition for an unknown plan", refused.value().code, StatusCode::NotFound);
  FCFN_CHECK(fcfn::is_failure(refused.value().code));

  // A retained plan presented with the wrong digest is refused as a conflict.
  TransitionRequest mismatched = unknown;
  mismatched.plan = arrangement.plan;
  mismatched.plan_digest = Digest{1, 2};
  const Result<fcfn::model::TransitionDecision> conflict =
      arrangement.runtime->transition(mismatched);
  FCFN_CHECK_OK(conflict);
  FCFN_CHECK(conflict.value().status == TransitionStatus::Invalid);
  expect_code("transition with a mismatched plan digest", conflict.value().code,
              StatusCode::Conflict);

  // A stale expected boundary generation is refused as stale, never applied.
  TransitionRequest stale = unknown;
  stale.plan = arrangement.plan;
  stale.plan_digest = arrangement.plan_digest;
  stale.expected_released = false;
  stale.expected_current_boundary = BoundaryGeneration{4242};
  const Result<fcfn::model::TransitionDecision> stale_decision =
      arrangement.runtime->transition(stale);
  FCFN_CHECK_OK(stale_decision);
  FCFN_CHECK(stale_decision.value().status == TransitionStatus::Stale);
  expect_code("stale expected boundary", stale_decision.value().code,
              StatusCode::StaleGeneration);
  // No transition was published.
  FCFN_CHECK_EQ(arrangement.runtime->stats().transitions_accepted, static_cast<std::uint64_t>(0));
}

FCFN_TEST(adversarial, malformed_requests_to_the_apply_protocol_are_refused) {
  fcfn::ManualClock clock(1000);
  Arrangement arrangement(&clock);
  plan_boundary(arrangement);

  expect_result("submit without an authorization", arrangement.runtime->submit_apply(
                                                       SubmitApplyRequest{}),
                StatusCode::Unauthorized);
  expect_result("submit for an unknown plan",
                arrangement.runtime->submit_apply(SubmitApplyRequest{
                    arrangement.authorization, PlanGeneration{99}, arrangement.plan_digest}),
                StatusCode::NotFound);
  expect_result("submit with a mismatched plan digest",
                arrangement.runtime->submit_apply(SubmitApplyRequest{
                    arrangement.authorization, arrangement.plan, Digest{1, 2}}),
                StatusCode::Conflict);
  expect_result("submit before the boundary is current",
                arrangement.runtime->submit_apply(SubmitApplyRequest{
                    arrangement.authorization, arrangement.plan, arrangement.plan_digest}),
                StatusCode::StaleGeneration);
  FCFN_CHECK_EQ(arrangement.runtime->attempts().size(), static_cast<std::size_t>(0));

  expand_boundary(arrangement);
  const ApplyAttempt attempt = submit(arrangement);
  FCFN_CHECK(attempt.state == EffectState::Submitted);
  expect_result("duplicate submission while in flight",
                arrangement.runtime->submit_apply(SubmitApplyRequest{
                    arrangement.authorization, arrangement.plan, arrangement.plan_digest}),
                StatusCode::AlreadyExists);

  const fcfn::model::AuthorityVector authority = arrangement.runtime->authority();
  ApplyAcknowledgement acknowledgement;
  acknowledgement.id = attempt.id;
  acknowledgement.expected_epoch = authority.epoch;
  acknowledgement.expected_boot = authority.boot;
  acknowledgement.applier_epoch = ApplierEpoch{1};
  acknowledgement.applier_boot = BootIdentity{BootId{0xabcdu}, 700};
  acknowledgement.observed_boundary_digest = attempt.boundary_digest;
  acknowledgement.accepted = true;

  {
    ApplyAcknowledgement unknown = acknowledgement;
    unknown.id = fcfn::model::ApplyAttemptId{arrangement.plan, fcfn::AttemptSequence{4242}};
    expect_result("acknowledgement for an unknown attempt",
                  arrangement.runtime->acknowledge(unknown), StatusCode::NotFound);
  }
  {
    ApplyAcknowledgement wrong_epoch = acknowledgement;
    wrong_epoch.expected_epoch = fcfn::CoordinatorEpoch{authority.epoch.value() + 1};
    expect_result("acknowledgement with a stale epoch",
                  arrangement.runtime->acknowledge(wrong_epoch), StatusCode::StaleGeneration);
  }
  {
    ApplyAcknowledgement wrong_boot = acknowledgement;
    wrong_boot.expected_boot = BootIdentity{BootId{authority.boot.id.value() ^ 1ull},
                                            authority.boot.process_id};
    expect_result("acknowledgement from another incarnation",
                  arrangement.runtime->acknowledge(wrong_boot), StatusCode::Fenced);
  }
  {
    ApplyAcknowledgement not_advancing = acknowledgement;
    not_advancing.applier_sequence = fcfn::Sequence{0};
    expect_result("acknowledgement with a non-advancing applier sequence",
                  arrangement.runtime->acknowledge(not_advancing), StatusCode::SequenceRegression);
  }
  {
    ApplyAcknowledgement advancing = acknowledgement;
    advancing.applier_sequence = fcfn::Sequence{1};
    const Result<ApplyAttempt> acknowledged = arrangement.runtime->acknowledge(advancing);
    FCFN_CHECK_OK(acknowledged);
    FCFN_CHECK(acknowledged.value().state == EffectState::Acknowledged);
    // Acknowledgement is not application.
    FCFN_CHECK(arrangement.runtime->current_effect_state() == EffectState::Acknowledged);
    FCFN_CHECK(!fcfn::model::is_verified_effect(arrangement.runtime->current_effect_state()));
  }
  {
    // A repeated acknowledgement advances the applier sequence but is still not
    // application: the effect state may not move to verified.
    ApplyAcknowledgement repeated = acknowledgement;
    repeated.applier_sequence = fcfn::Sequence{2};
    const Result<ApplyAttempt> again = arrangement.runtime->acknowledge(repeated);
    FCFN_CHECK_OK(again);
    FCFN_CHECK(again.value().state == EffectState::Acknowledged);
    FCFN_CHECK(!fcfn::model::is_verified_effect(again.value().state));
  }

  // Varying only the observation field must not be able to change the digest.
  {
    EffectVerification report;
    report.id = attempt.id;
    report.expected_epoch = authority.epoch;
    report.expected_boot = authority.boot;
    report.applier_epoch = ApplierEpoch{1};
    report.applier_boot = acknowledgement.applier_boot;
    report.applier_sequence = fcfn::Sequence{1};
    report.observed_boundary_digest = attempt.boundary_digest;
    report.observation = EffectObservation::Applied;
    expect_result("verification with a non-advancing applier sequence",
                  arrangement.runtime->verify_effect(report), StatusCode::SequenceRegression);
  }
  {
    EffectVerification report;
    report.id = attempt.id;
    report.expected_epoch = authority.epoch;
    report.expected_boot = authority.boot;
    report.applier_epoch = ApplierEpoch{1};
    report.applier_boot = acknowledgement.applier_boot;
    report.applier_sequence = fcfn::Sequence{3};
    report.observed_boundary_digest = Digest{attempt.boundary_digest.hi ^ 1ull,
                                             attempt.boundary_digest.lo};
    report.observation = EffectObservation::Applied;
    expect_result("verification that observed a different boundary",
                  arrangement.runtime->verify_effect(report), StatusCode::Conflict);
    // A mismatched observation is ambiguity, never success.
    FCFN_CHECK(arrangement.runtime->current_effect_state() == EffectState::Ambiguous);
    FCFN_CHECK(!fcfn::model::is_verified_effect(arrangement.runtime->current_effect_state()));
  }
  // Ambiguity is resolved by evidence, not by assumption.
  verify(arrangement, attempt, EffectObservation::Applied, attempt.boundary_digest, 4);
  FCFN_CHECK(arrangement.runtime->current_effect_state() == EffectState::VerifiedApplied);
  FCFN_CHECK(fcfn::model::is_verified_effect(arrangement.runtime->current_effect_state()));
  {
    EffectVerification duplicate;
    duplicate.id = attempt.id;
    duplicate.expected_epoch = authority.epoch;
    duplicate.expected_boot = authority.boot;
    duplicate.applier_epoch = ApplierEpoch{1};
    duplicate.applier_boot = acknowledgement.applier_boot;
    duplicate.applier_sequence = fcfn::Sequence{5};
    duplicate.observed_boundary_digest = attempt.boundary_digest;
    duplicate.observation = EffectObservation::Applied;
    expect_result("duplicate completion", arrangement.runtime->verify_effect(duplicate),
                  StatusCode::AlreadyExists);
  }
  // The attempt is terminal now, so even a stale acknowledgement is refused as a
  // duplicate completion rather than silently rewriting the effect history.
  expect_result("acknowledgement after a verified effect",
                arrangement.runtime->acknowledge(acknowledgement), StatusCode::AlreadyExists);
  FCFN_CHECK(arrangement.runtime->stats().duplicate_completions_rejected > 0);
  const Result<ApplyAttempt> retained =
      arrangement.runtime->attempt(arrangement.plan, attempt.id.sequence);
  FCFN_CHECK_OK(retained);
  FCFN_CHECK(retained.value().id == attempt.id);
}

FCFN_TEST(adversarial, a_release_request_with_active_failures_is_denied) {
  fcfn::ManualClock clock(1000);
  Arrangement arrangement(&clock);
  plan_boundary(arrangement);
  expand_boundary(arrangement);
  const ApplyAttempt attempt = submit(arrangement);
  verify(arrangement, attempt, EffectObservation::Applied, attempt.boundary_digest, 1);

  TransitionRequest release;
  release.kind = TransitionKind::Release;
  release.plan = arrangement.plan;
  release.plan_digest = arrangement.plan_digest;
  release.expected_current_boundary = BoundaryGeneration{arrangement.boundary_generation};
  release.expected_released = false;
  release.authorization = arrangement.authorization;
  const Result<fcfn::model::TransitionDecision> denied = arrangement.runtime->transition(release);
  FCFN_CHECK_OK(denied);
  FCFN_CHECK(denied.value().status == TransitionStatus::Denied);
  expect_code("release with active failures", denied.value().code, StatusCode::Denied);
  // The boundary is still in force after the refused release.
  const Result<fcfn::model::ContainmentBoundary> boundary = arrangement.runtime->current_boundary();
  FCFN_CHECK_OK(boundary);
  FCFN_CHECK_EQ(boundary.value().generation().value(), arrangement.boundary_generation);
}
