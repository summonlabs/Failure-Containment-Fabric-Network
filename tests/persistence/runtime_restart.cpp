// FCFN persistence suite: containment runtime restart semantics.
//
// Product proposition proved here: a restart is a new authority incarnation. It
// takes a fresh epoch and a different boot identity, it fences every
// pre-restart authorization, it restores plans, boundaries, and detections as
// historical lineage only, it never restores verified effect, and it never
// restores evidence freshness: a plan computed before the evidence is
// reconfirmed in the new boot is INDETERMINATE, and only a new apply attempt
// verified under the new epoch makes a boundary change legal again.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "store_support.hpp"

namespace {

using namespace fcfn::test::persist;

using fcfn::model::BoundaryGeneration;
using fcfn::model::ContainmentClaim;
using fcfn::model::EvidenceCurrency;
using fcfn::model::EvidenceState;
using fcfn::model::ResourceId;
using fcfn::model::TransitionKind;
using fcfn::model::TransitionStatus;
using fcfn::runtime::ContainmentRuntime;
using fcfn::runtime::PlanRequest;
using fcfn::runtime::RuntimeConfig;
using fcfn::runtime::SubmitApplyRequest;

std::unique_ptr<ContainmentRuntime> open_runtime(const RuntimeConfig& config,
                                                 fcfn::ManualClock& clock) {
  Result<std::unique_ptr<ContainmentRuntime>> opened = ContainmentRuntime::open(config, &clock);
  if (!opened.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__, "cannot open runtime: " + opened.status().to_string());
  }
  return std::move(opened.value());
}

/// Records one authoritative failure detection for the fixture source.
void report_source(ContainmentRuntime& runtime, std::uint64_t generation, std::uint64_t digest_hi,
                   std::uint64_t digest_lo) {
  const Result<fcfn::model::DetectionRecord> recorded =
      runtime.record_detection(source_observation(generation, digest_hi, digest_lo));
  if (!recorded.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "record_detection failed: " + recorded.status().to_string());
  }
}

/// Drives one apply attempt for a retained plan through to verified effect.
void verify_boundary_effect(ContainmentRuntime& runtime, fcfn::runtime::AuthorizationId authorization,
                            fcfn::model::PlanGeneration plan, const fcfn::model::Digest& digest) {
  const Result<fcfn::model::ApplyAttempt> submitted =
      runtime.submit_apply(SubmitApplyRequest{authorization, plan, digest});
  if (!submitted.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "submit_apply failed: " + submitted.status().to_string());
  }
  const fcfn::model::AuthorityVector authority = runtime.authority();

  fcfn::model::ApplyAcknowledgement acknowledgement;
  acknowledgement.id = submitted.value().id;
  acknowledgement.expected_epoch = authority.epoch;
  acknowledgement.expected_boot = authority.boot;
  acknowledgement.applier_epoch = fcfn::model::ApplierEpoch{1};
  acknowledgement.applier_boot =
      fcfn::model::BootIdentity{fcfn::model::BootId{0xfeedfaceull}, 5150};
  acknowledgement.applier_sequence = fcfn::Sequence{1};
  acknowledgement.observed_boundary_digest = submitted.value().boundary_digest;
  acknowledgement.accepted = true;
  const Result<fcfn::model::ApplyAttempt> acknowledged = runtime.acknowledge(acknowledgement);
  if (!acknowledged.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "acknowledge failed: " + acknowledged.status().to_string());
  }

  fcfn::model::EffectVerification verification;
  verification.id = submitted.value().id;
  verification.expected_epoch = authority.epoch;
  verification.expected_boot = authority.boot;
  verification.applier_epoch = fcfn::model::ApplierEpoch{1};
  verification.applier_boot = acknowledgement.applier_boot;
  verification.applier_sequence = fcfn::Sequence{2};
  verification.observed_boundary_digest = submitted.value().boundary_digest;
  verification.observation = fcfn::model::EffectObservation::Applied;
  const Result<fcfn::model::ApplyAttempt> verified = runtime.verify_effect(verification);
  if (!verified.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "verify_effect failed: " + verified.status().to_string());
  }
  if (verified.value().state != fcfn::model::EffectState::VerifiedApplied) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("verified effect state is ") +
                           fcfn::model::to_string(verified.value().state));
  }
}

/// Expands from the released state to the plan's boundary on the first boot.
void expand_from_released(ContainmentRuntime& runtime, fcfn::runtime::AuthorizationId authorization,
                          fcfn::model::PlanGeneration plan, const fcfn::model::Digest& digest) {
  fcfn::model::TransitionRequest transition;
  transition.kind = TransitionKind::Expand;
  transition.plan = plan;
  transition.plan_digest = digest;
  transition.expected_current_boundary = BoundaryGeneration{0};
  transition.expected_released = true;
  transition.authorization = authorization;
  const Result<fcfn::model::TransitionDecision> applied = runtime.transition(transition);
  if (!applied.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "transition failed: " + applied.status().to_string());
  }
  if (applied.value().status != TransitionStatus::Accepted) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("transition was not accepted: ") +
                           fcfn::model::to_string(applied.value().status));
  }
}

}  // namespace

FCFN_TEST(restart, a_fresh_runtime_advances_the_epoch_and_reports_its_identity) {
  const fcfn::test::TempDir dir("restart-fresh");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());
  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);

  FCFN_CHECK(runtime->startup().fresh);
  FCFN_CHECK(!runtime->startup().recovered);
  FCFN_CHECK_EQ(runtime->startup().epoch.value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK(!runtime->startup().boot.id.is_zero());
  FCFN_CHECK_EQ(runtime->startup().topology.value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(runtime->startup().policy.value(), static_cast<std::uint64_t>(1));
  const Result<fcfn::model::TopologyGeneration> generation = runtime->topology_generation();
  FCFN_CHECK_OK(generation);
  FCFN_CHECK_EQ(generation.value().value(), static_cast<std::uint64_t>(1));
}

FCFN_TEST(restart, a_new_incarnation_fences_effect_authority_and_evidence_freshness) {
  const fcfn::test::TempDir dir("restart-incarnation");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());

  fcfn::CoordinatorEpoch first_epoch{};
  fcfn::model::BootIdentity first_boot{};
  fcfn::runtime::AuthorizationId stale_authorization{};
  fcfn::model::AuthorityVector stale_authority{};
  fcfn::model::PlanGeneration first_plan{};
  fcfn::model::Digest first_plan_digest{};
  std::uint64_t boundary_generation = 0;

  {
    const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
    FCFN_CHECK(runtime->startup().fresh);
    report_source(*runtime, 1, 0x1111, 0x2222);

    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    stale_authorization = authorization.value().id;
    stale_authority = authorization.value().authority;
    first_epoch = authorization.value().authority.epoch;
    first_boot = authorization.value().authority.boot;

    const Result<fcfn::model::ContainmentPlan> planned =
        runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    FCFN_CHECK(planned.value().claim == ContainmentClaim::ProvenContainment);
    FCFN_CHECK(planned.value().currency == EvidenceCurrency::Current);
    FCFN_CHECK(planned.value().boundary.size() > 0);
    FCFN_CHECK(planned.value().boundary.contains(ResourceId::unchecked("l0c0")));
    first_plan = planned.value().generation;
    first_plan_digest = planned.value().digest();
    boundary_generation = planned.value().boundary.generation().value();

    expand_from_released(*runtime, authorization.value().id, first_plan, first_plan_digest);
    verify_boundary_effect(*runtime, authorization.value().id, first_plan, first_plan_digest);
    FCFN_CHECK(runtime->current_effect_state() == fcfn::model::EffectState::VerifiedApplied);
    FCFN_CHECK(fcfn::model::is_verified_effect(runtime->current_effect_state()));
    FCFN_CHECK_OK(runtime->checkpoint());
  }

  // A brand-new incarnation over the same durable directory.
  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
  FCFN_CHECK(runtime->startup().recovered);
  FCFN_CHECK(!runtime->startup().fresh);
  FCFN_CHECK_EQ(runtime->startup().epoch.value(), first_epoch.value() + 1);
  FCFN_CHECK(runtime->authority().boot != first_boot);
  FCFN_CHECK(runtime->authority().boot.id != first_boot.id);
  FCFN_CHECK(!runtime->startup().torn_tail_recovered);

  // 1. Every pre-restart authorization is gone; a stale identifier is refused.
  const Result<fcfn::model::ContainmentPlan> with_stale =
      runtime->plan(PlanRequest{stale_authorization});
  FCFN_CHECK(!with_stale.ok());
  require_status("stale authorization id", with_stale.status().code(), StatusCode::Unauthorized);
  // The pre-restart authority vector itself is fenced, not merely stale.
  const fcfn::model::AuthorityCheck fenced =
      fcfn::model::validate_authority(stale_authority, runtime->authority());
  FCFN_CHECK(!fenced.ok());
  FCFN_CHECK(fenced.status_code() == StatusCode::Fenced);
  FCFN_CHECK(fenced.state == fcfn::model::AuthorityState::Fenced);

  // 2. Plans, boundaries, and detections are restored as historical lineage.
  const Result<fcfn::model::ContainmentPlan> restored = runtime->plan_by_generation(first_plan);
  FCFN_CHECK_OK(restored);
  FCFN_CHECK(restored.value().digest() == first_plan_digest);
  FCFN_CHECK_EQ(restored.value().boundary.generation().value(), boundary_generation);
  const Result<fcfn::model::ContainmentBoundary> boundary = runtime->current_boundary();
  FCFN_CHECK_OK(boundary);
  FCFN_CHECK_EQ(boundary.value().generation().value(), boundary_generation);
  const Result<fcfn::model::EvidenceVector> evidence = runtime->evidence();
  FCFN_CHECK_OK(evidence);
  const fcfn::model::EvidenceEntry* source = evidence.value().find(ResourceId::unchecked("l0c0"));
  FCFN_CHECK(source != nullptr);
  FCFN_CHECK(source->state == EvidenceState::Present);
  FCFN_CHECK_EQ(source->generation.value(), static_cast<std::uint64_t>(1));

  // 3. Effect state is not restored as verified: history records a verified
  // attempt, currentness still requires a verified effect under this boot.
  const std::vector<fcfn::model::ApplyAttempt> history = runtime->attempts();
  FCFN_CHECK_EQ(history.size(), static_cast<std::size_t>(1));
  FCFN_CHECK(history.front().state == fcfn::model::EffectState::VerifiedApplied);
  FCFN_CHECK(runtime->current_effect_state() == fcfn::model::EffectState::Ambiguous);
  FCFN_CHECK(!fcfn::model::is_verified_effect(runtime->current_effect_state()));

  // The new incarnation issues its own authority; everything below uses it.
  const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
  FCFN_CHECK_OK(authorization);

  // 4. A transition that requires verified effect is refused.
  {
    fcfn::model::TransitionRequest transition;
    transition.kind = TransitionKind::Expand;
    transition.plan = first_plan;
    transition.plan_digest = first_plan_digest;
    transition.expected_current_boundary = BoundaryGeneration{boundary_generation};
    transition.expected_released = false;
    transition.authorization = authorization.value().id;
    const Result<fcfn::model::TransitionDecision> refused = runtime->transition(transition);
    FCFN_CHECK_OK(refused);
    FCFN_CHECK(refused.value().status == TransitionStatus::Indeterminate);
    FCFN_CHECK(refused.value().code == StatusCode::Ambiguous);
    FCFN_CHECK(fcfn::is_failure(refused.value().code));
  }

  // 5. Evidence freshness is not restored: a plan computed now is INDETERMINATE
  //    because present evidence was never confirmed in this boot.
  const Result<fcfn::model::ContainmentPlan> stale_plan =
      runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK_OK(stale_plan);
  FCFN_CHECK(stale_plan.value().claim == ContainmentClaim::Indeterminate);
  FCFN_CHECK(stale_plan.value().currency == EvidenceCurrency::Stale);
  // Stale currency is explained by its own reason code, not by the adjacency
  // completeness code that describes an entirely different condition.
  FCFN_CHECK(stale_plan.value().explanation.contains(
      fcfn::model::ReasonCode::EvidenceNotConfirmedSinceRestart));
  FCFN_CHECK(!stale_plan.value().explanation.contains(
      fcfn::model::ReasonCode::IncompleteAdjacencyOnFrontier));

  // 6. Re-reporting the same evidence (same generation, same digest) confirms it
  //    in this boot, and the plan becomes PROVEN_CONTAINMENT again.
  const std::uint64_t unchanged_before = runtime->stats().detections_unchanged;
  report_source(*runtime, 1, 0x1111, 0x2222);
  FCFN_CHECK_EQ(runtime->stats().detections_unchanged, unchanged_before + 1);
  const Result<fcfn::model::ContainmentPlan> reconfirmed =
      runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK_OK(reconfirmed);
  FCFN_CHECK(reconfirmed.value().claim == ContainmentClaim::ProvenContainment);
  FCFN_CHECK(reconfirmed.value().currency == EvidenceCurrency::Current);

  // 7. Only a NEW apply attempt verified under the new epoch re-establishes the
  //    effect; the restored plan is still the current boundary decision.
  verify_boundary_effect(*runtime, authorization.value().id, first_plan, first_plan_digest);
  FCFN_CHECK(runtime->current_effect_state() == fcfn::model::EffectState::VerifiedApplied);
  FCFN_CHECK_EQ(runtime->attempts().size(), static_cast<std::size_t>(2));
  FCFN_CHECK(runtime->attempts().back().epoch == runtime->authority().epoch);

  // 8. With verified effect under the new epoch a boundary change is legal again.
  fcfn::model::TransitionRequest transition;
  transition.kind = TransitionKind::Expand;
  transition.plan = reconfirmed.value().generation;
  transition.plan_digest = reconfirmed.value().digest();
  transition.expected_current_boundary = BoundaryGeneration{boundary_generation};
  transition.expected_released = false;
  transition.authorization = authorization.value().id;
  const Result<fcfn::model::TransitionDecision> accepted = runtime->transition(transition);
  FCFN_CHECK_OK(accepted);
  FCFN_CHECK(accepted.value().status == TransitionStatus::Accepted);
  FCFN_CHECK_EQ(accepted.value().to_generation.value(),
                reconfirmed.value().boundary.generation().value());

  // The startup report tells the operator how many leases must be re-issued: the
  // snapshot recorded one active authorization, and this incarnation dropped it.
  FCFN_CHECK_EQ(runtime->startup().fenced_authorizations, static_cast<std::size_t>(1));
  FCFN_CHECK(runtime->startup().explanation.contains(fcfn::model::ReasonCode::PreRestartLeasesFenced));
}

FCFN_TEST(restart, without_a_checkpoint_the_boundary_survives_but_effect_is_never_verified) {
  const fcfn::test::TempDir dir("restart-log-only");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());

  fcfn::model::PlanGeneration first_plan{};
  fcfn::model::Digest first_plan_digest{};
  std::uint64_t boundary_generation = 0;
  {
    const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
    report_source(*runtime, 1, 0x33, 0x44);
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    const Result<fcfn::model::ContainmentPlan> planned =
        runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    first_plan = planned.value().generation;
    first_plan_digest = planned.value().digest();
    boundary_generation = planned.value().boundary.generation().value();
    expand_from_released(*runtime, authorization.value().id, first_plan, first_plan_digest);
    verify_boundary_effect(*runtime, authorization.value().id, first_plan, first_plan_digest);
    FCFN_CHECK(runtime->current_effect_state() == fcfn::model::EffectState::VerifiedApplied);
  }

  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
  FCFN_CHECK(runtime->startup().recovered);
  FCFN_CHECK(runtime->startup().replayed_records > 0);

  // The boundary is rebuilt from the log even without a snapshot.
  const Result<fcfn::model::ContainmentBoundary> boundary = runtime->current_boundary();
  FCFN_CHECK_OK(boundary);
  FCFN_CHECK_EQ(boundary.value().generation().value(), boundary_generation);

  // The durable apply-protocol history is replayed exactly: the attempt that was
  // verified before the restart is restored as VERIFIED_APPLIED history, nothing
  // was in flight, and nothing is fenced.
  FCFN_CHECK_EQ(runtime->attempts().size(), static_cast<std::size_t>(1));
  FCFN_CHECK(runtime->attempts().front().state == fcfn::model::EffectState::VerifiedApplied);
  FCFN_CHECK_EQ(runtime->startup().fenced_attempts, static_cast<std::size_t>(0));
  // History is not currentness: this incarnation still refuses to treat the
  // effect as verified until the enforcement plane re-verifies it, and it
  // reports the same AMBIGUOUS state a snapshot recovery reports.
  FCFN_CHECK(!fcfn::model::is_verified_effect(runtime->current_effect_state()));
  FCFN_CHECK(runtime->current_effect_state() == fcfn::model::EffectState::Ambiguous);

  const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
  FCFN_CHECK_OK(authorization);
  fcfn::model::TransitionRequest transition;
  transition.kind = TransitionKind::Expand;
  transition.plan = first_plan;
  transition.plan_digest = first_plan_digest;
  transition.expected_current_boundary = BoundaryGeneration{boundary_generation};
  transition.expected_released = false;
  transition.authorization = authorization.value().id;
  const Result<fcfn::model::TransitionDecision> refused = runtime->transition(transition);
  FCFN_CHECK_OK(refused);
  FCFN_CHECK(refused.value().status == TransitionStatus::Indeterminate);
  FCFN_CHECK(refused.value().code == StatusCode::Ambiguous);
}

FCFN_TEST(restart, a_runtime_repairs_a_torn_tail_and_reports_the_exact_byte_count) {
  const fcfn::test::TempDir dir("restart-torn");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());
  std::vector<std::byte> complete;

  {
    const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
    report_source(*runtime, 1, 0x55, 0x66);
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    const Result<fcfn::model::ContainmentPlan> planned =
        runtime->plan(PlanRequest{authorization.value().id});
    FCFN_CHECK_OK(planned);
    complete = read_bytes(active_wal(dir.path()));
    FCFN_CHECK(!complete.empty());
  }

  // Append a strict prefix of a well-formed record: the only recoverable damage.
  const std::vector<std::byte> frame = fcfn::store::encode_record(
      fcfn::store::RecordType::PlanDecision, fcfn::Sequence{4096}, make_payload(99));
  constexpr std::size_t kTornBytes = 10;
  append_bytes(active_wal(dir.path()), std::span<const std::byte>(frame.data(), kTornBytes));

  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
  FCFN_CHECK(runtime->startup().torn_tail_recovered);
  FCFN_CHECK_EQ(runtime->startup().torn_tail_bytes, static_cast<std::uint64_t>(kTornBytes));
  FCFN_CHECK(runtime->startup().recovered);

  // The repaired log keeps every complete record and discards exactly the torn
  // bytes; the records this incarnation appends follow them.
  const std::vector<std::byte> repaired = read_bytes(active_wal(dir.path()));
  FCFN_CHECK(repaired.size() > complete.size());
  FCFN_CHECK(same_bytes(std::span<const std::byte>(repaired.data(), complete.size()), complete));

  // The recovered evidence is intact, but not fresh in the new boot.
  const Result<fcfn::model::EvidenceVector> evidence = runtime->evidence();
  FCFN_CHECK_OK(evidence);
  const fcfn::model::EvidenceEntry* source = evidence.value().find(ResourceId::unchecked("l0c0"));
  FCFN_CHECK(source != nullptr);
  FCFN_CHECK(source->state == EvidenceState::Present);
}
