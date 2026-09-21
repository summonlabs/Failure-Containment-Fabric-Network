// FCFN adversarial suite: authority binding.
//
// Product proposition proved here: currentness is never inferred from matching
// identifiers. A vector that matches on epoch, policy, topology, and evidence but
// belongs to a different process incarnation is FENCED, a vector whose evidence
// revision has moved on is STALE_GENERATION, and those are distinct refusals -
// never a silent success and never a fallback to "looks the same, so accept".
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <memory>
#include <string>

#include "adversarial_support.hpp"

namespace {

using namespace fcfn::test::adversarial;

using fcfn::model::AuthorityCheck;
using fcfn::model::AuthorityState;
using fcfn::model::AuthorityVector;
using fcfn::model::BootId;
using fcfn::model::BootIdentity;
using fcfn::model::EvidenceRevision;
using fcfn::model::EvidenceState;
using fcfn::model::PolicyGeneration;
using fcfn::model::TopologyGeneration;
using fcfn::runtime::PlanRequest;

void prove_refusal(const char* what, const AuthorityCheck& check, AuthorityState state,
                   StatusCode code) {
  if (check.state != state) {
    fail_here(what, std::string("expected authority state ") + fcfn::model::to_string(state) +
                        " but got " + fcfn::model::to_string(check.state));
  }
  if (check.status_code() != code) {
    fail_here(what, std::string("expected ") + to_string(code) + " but got " +
                        to_string(check.status_code()));
  }
}

}  // namespace

FCFN_TEST(adversarial, currentness_is_never_inferred_from_matching_identifiers) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
  source_detection(*runtime, 1);
  const AuthorityVector current = runtime->authority();
  FCFN_CHECK(!current.epoch.is_zero());
  FCFN_CHECK(!current.boot.id.is_zero());
  FCFN_CHECK(!current.evidence_revision.is_zero());

  // Control: the current vector validates against itself.
  FCFN_CHECK(fcfn::model::validate_authority(current, current).ok());

  // Same epoch, same policy, same topology, same evidence revision, same evidence
  // digest, same issued sequence - only the process incarnation differs.
  AuthorityVector foreign = current;
  foreign.boot = BootIdentity{BootId{current.boot.id.value() ^ 0xffffffffffffffffull},
                              current.boot.process_id + 1u};
  FCFN_CHECK(foreign.epoch == current.epoch);
  FCFN_CHECK(foreign.policy == current.policy);
  FCFN_CHECK(foreign.topology == current.topology);
  FCFN_CHECK(foreign.evidence_revision == current.evidence_revision);
  FCFN_CHECK(foreign.evidence_digest == current.evidence_digest);
  FCFN_CHECK(foreign.issued_sequence == current.issued_sequence);
  prove_refusal("matching identifiers with a different boot", 
                fcfn::model::validate_authority(foreign, current), AuthorityState::Fenced,
                StatusCode::Fenced);

  // A stale evidence revision is a different refusal: stale, not fenced.
  AuthorityVector stale = current;
  stale.evidence_revision = EvidenceRevision{current.evidence_revision.value() - 1};
  FCFN_CHECK(stale.boot == current.boot);
  prove_refusal("stale evidence revision", fcfn::model::validate_authority(stale, current),
                AuthorityState::StaleEvidence, StatusCode::StaleGeneration);

  // The same revision with a different digest is equally stale.
  AuthorityVector rewritten = current;
  rewritten.evidence_digest = fcfn::model::Digest{current.evidence_digest.hi ^ 1ull,
                                                  current.evidence_digest.lo};
  prove_refusal("rewritten evidence digest",
                fcfn::model::validate_authority(rewritten, current), AuthorityState::StaleEvidence,
                StatusCode::StaleGeneration);

  // Every other binding is independently enforced too.
  AuthorityVector other_epoch = current;
  other_epoch.epoch = fcfn::CoordinatorEpoch{current.epoch.value() + 1};
  prove_refusal("different epoch", fcfn::model::validate_authority(other_epoch, current),
                AuthorityState::StaleEpoch, StatusCode::StaleGeneration);
  AuthorityVector other_policy = current;
  other_policy.policy = PolicyGeneration{current.policy.value() + 1};
  prove_refusal("different policy generation",
                fcfn::model::validate_authority(other_policy, current), AuthorityState::StalePolicy,
                StatusCode::StaleGeneration);
  AuthorityVector other_topology = current;
  other_topology.topology = TopologyGeneration{current.topology.value() + 1};
  prove_refusal("different topology generation",
                fcfn::model::validate_authority(other_topology, current),
                AuthorityState::StaleTopology, StatusCode::StaleGeneration);

  // Fenced and stale are distinct classifications, and both are refusals: the
  // runtime never folds "another incarnation" into "merely older".
  const AuthorityCheck fenced = fcfn::model::validate_authority(foreign, current);
  const AuthorityCheck stale_check = fcfn::model::validate_authority(stale, current);
  FCFN_CHECK(fcfn::is_failure(fenced.status_code()));
  FCFN_CHECK(fcfn::is_failure(stale_check.status_code()));
  FCFN_CHECK(static_cast<int>(fenced.status_code()) != static_cast<int>(stale_check.status_code()));
}

FCFN_TEST(adversarial, a_stale_evidence_revision_invalidates_live_authority) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
  source_detection(*runtime, 1);
  const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
  FCFN_CHECK_OK(authorization);
  const std::uint64_t revision = authorization.value().authority.evidence_revision.value();

  const Result<ContainmentPlan> before = runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK_OK(before);

  // Authoritative evidence advances: the authorization's bound revision is now
  // behind the runtime's current authority.
  const Result<fcfn::model::DetectionRecord> advanced =
      runtime->record_detection(source_observation(2, 0x1234, 0x5678));
  FCFN_CHECK_OK(advanced);
  FCFN_CHECK_EQ(runtime->authority().evidence_revision.value(), revision + 1);

  expect_result("authorization bound to a superseded evidence revision",
                runtime->plan(PlanRequest{authorization.value().id}),
                StatusCode::StaleGeneration);
  // The refusal fenced the authorization: it stays refused afterwards.
  expect_result("fenced authorization reused",
                runtime->plan(PlanRequest{authorization.value().id}),
                StatusCode::Fenced);

  // A fresh authorization over the current revision is accepted.
  const Result<fcfn::runtime::AuthorizationRecord> fresh = runtime->authorize();
  FCFN_CHECK_OK(fresh);
  FCFN_CHECK_OK(runtime->plan(PlanRequest{fresh.value().id}));
}

FCFN_TEST(adversarial, a_pre_restart_authority_vector_is_fenced_and_its_id_is_unauthorized) {
  const fcfn::test::TempDir dir("adv-authority-restart");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = durable_config(dir.path());
  AuthorityVector previous{};
  fcfn::runtime::AuthorizationId previous_id{};
  {
    const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    previous = authorization.value().authority;
    previous_id = authorization.value().id;
    FCFN_CHECK_OK(runtime->plan(PlanRequest{previous_id}));
  }
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
  // The identifier is refused outright; the vector itself is fenced, which is a
  // different and stronger statement than "unknown".
  expect_result("pre-restart authorization id", runtime->plan(PlanRequest{previous_id}),
                StatusCode::Unauthorized);
  const AuthorityCheck check = fcfn::model::validate_authority(previous, runtime->authority());
  prove_refusal("pre-restart authority vector", check, AuthorityState::Fenced, StatusCode::Fenced);
  FCFN_CHECK(runtime->authority().epoch.value() > previous.epoch.value());
}
