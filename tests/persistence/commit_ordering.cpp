// FCFN persistence suite: durable ordering of the mutation path.
//
// Product proposition proved here: the append -> flush -> publish ordering is
// real. A commit that fails leaves both the durable sequence and the in-memory
// state exactly as they were, no half-applied decision is ever published, and a
// bounded table that refuses an entry leaves everything already published
// usable.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "store_support.hpp"

namespace {

using namespace fcfn::test::persist;

using fcfn::model::PlanGeneration;
using fcfn::model::ResourceId;
using fcfn::runtime::ContainmentRuntime;
using fcfn::runtime::PlanRequest;
using fcfn::runtime::RuntimeConfig;

/// Replace the active log with a directory so every append must fail.
void make_log_unwritable(const std::filesystem::path& wal) {
  std::error_code error;
  std::filesystem::remove(wal, error);
  FCFN_CHECK(!error);
  std::filesystem::create_directory(wal, error);
  FCFN_CHECK(!error);
}

void restore_log_path(const std::filesystem::path& wal) {
  std::error_code error;
  std::filesystem::remove_all(wal, error);
  FCFN_CHECK(!error);
}

std::unique_ptr<ContainmentRuntime> open_runtime(const RuntimeConfig& config,
                                                 fcfn::ManualClock& clock) {
  Result<std::unique_ptr<ContainmentRuntime>> opened = ContainmentRuntime::open(config, &clock);
  if (!opened.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__, "cannot open runtime: " + opened.status().to_string());
  }
  return std::move(opened.value());
}

RuntimeConfig runtime_config(const std::filesystem::path& root) {
  RuntimeConfig config = durable_config(root);
  return config;
}

}  // namespace

FCFN_TEST(ordering, a_failed_commit_leaves_the_durable_sequence_unchanged) {
  const fcfn::test::TempDir dir("ordering-store");
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());
  require_ok("first commit",
             store->commit(fcfn::store::RecordType::Fence, make_payload(1)));
  FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(1));

  const std::filesystem::path wal = store->wal_path();
  make_log_unwritable(wal);
  const VoidResult failed = store->commit(fcfn::store::RecordType::Fence, make_payload(2));
  FCFN_CHECK(!failed.ok());
  require_status("commit against an unwritable log", failed.status().code(), StatusCode::IoError);
  // The published sequence must not move for a commit that never reached disk.
  FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(1));

  restore_log_path(wal);
  require_ok("commit after repair",
             store->commit(fcfn::store::RecordType::Fence, make_payload(3)));
  // The failed attempt burned no sequence: the next record is exactly the next one.
  FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(2));
}

FCFN_TEST(ordering, a_record_above_the_payload_bound_is_refused_without_publishing) {
  const fcfn::test::TempDir dir("ordering-bound");
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());
  std::vector<std::byte> oversized(kMaxRecordPayloadBytes + 1, std::byte{0x01});
  const VoidResult refused =
      store->commit(fcfn::store::RecordType::Fence, oversized);
  FCFN_CHECK(!refused.ok());
  require_status("oversized record", refused.status().code(), StatusCode::LimitExceeded);
  FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(0));
  // Nothing was written: a rejected record never reaches the log.
  FCFN_CHECK_EQ(size_of(store->wal_path()), static_cast<std::uintmax_t>(0));
}

FCFN_TEST(ordering, a_plan_whose_durable_commit_fails_is_not_published) {
  const fcfn::test::TempDir dir("ordering-plan");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = runtime_config(dir.path());
  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);

  FCFN_CHECK_OK(runtime->record_detection(source_observation(1, 0x77, 0x88)));
  const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
  FCFN_CHECK_OK(authorization);
  const Result<fcfn::model::ContainmentPlan> first =
      runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK_OK(first);
  FCFN_CHECK_EQ(first.value().generation.value(), static_cast<std::uint64_t>(1));
  const std::uint64_t plans_computed = runtime->stats().plans_computed;

  const std::filesystem::path wal = active_wal(dir.path());
  make_log_unwritable(wal);
  const Result<fcfn::model::ContainmentPlan> failed =
      runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK(!failed.ok());
  require_status("plan against an unwritable log", failed.status().code(), StatusCode::IoError);
  // No state divergence: the plan counter, the retained table, and the stats all
  // still describe the state before the failed commit.
  FCFN_CHECK_EQ(runtime->stats().plans_computed, plans_computed);
  const Result<fcfn::model::ContainmentPlan> missing =
      runtime->plan_by_generation(PlanGeneration{2});
  FCFN_CHECK(!missing.ok());
  require_status("plan 2 after a failed commit", missing.status().code(), StatusCode::NotFound);

  restore_log_path(wal);
  const Result<fcfn::model::ContainmentPlan> second =
      runtime->plan(PlanRequest{authorization.value().id});
  FCFN_CHECK_OK(second);
  // The generation the failed attempt would have used is reused, not skipped.
  FCFN_CHECK_EQ(second.value().generation.value(), static_cast<std::uint64_t>(2));
  FCFN_CHECK(second.value().claim == first.value().claim);
}

FCFN_TEST(ordering, a_detection_whose_durable_commit_fails_leaves_evidence_unchanged) {
  const fcfn::test::TempDir dir("ordering-detection");
  fcfn::ManualClock clock(1000);
  const RuntimeConfig config = runtime_config(dir.path());
  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);

  FCFN_CHECK_OK(runtime->record_detection(source_observation(1, 0x11, 0x22)));
  const fcfn::model::AuthorityVector before = runtime->authority();
  const fcfn::model::Digest evidence_before = runtime->evidence().value().digest();
  const std::uint64_t accepted_before = runtime->stats().detections_accepted;

  const std::filesystem::path wal = active_wal(dir.path());
  make_log_unwritable(wal);
  const Result<fcfn::model::DetectionRecord> failed =
      runtime->record_detection(source_observation(2, 0x33, 0x44));
  FCFN_CHECK(!failed.ok());
  require_status("detection against an unwritable log", failed.status().code(), StatusCode::IoError);

  // Evidence, revision, and statistics are all unchanged by the failed commit.
  FCFN_CHECK(runtime->evidence().value().digest() == evidence_before);
  FCFN_CHECK(runtime->authority().evidence_revision == before.evidence_revision);
  FCFN_CHECK_EQ(runtime->stats().detections_accepted, accepted_before);

  restore_log_path(wal);
  FCFN_CHECK_OK(runtime->record_detection(source_observation(2, 0x33, 0x44)));
  // The revision advanced by exactly one, so the failed attempt was not applied
  // twice and did not consume a revision.
  FCFN_CHECK_EQ(runtime->authority().evidence_revision.value(),
                before.evidence_revision.value() + 1);
  FCFN_CHECK_EQ(runtime->stats().detections_accepted, accepted_before + 1);
}

FCFN_TEST(ordering, exhausting_the_authorization_table_leaves_earlier_authority_usable) {
  const fcfn::test::TempDir dir("ordering-authorizations");
  fcfn::ManualClock clock(1000);
  RuntimeConfig config = runtime_config(dir.path());
  config.max_active_authorizations = 1;
  const std::unique_ptr<ContainmentRuntime> runtime = open_runtime(config, clock);
  FCFN_CHECK_OK(runtime->record_detection(source_observation(1, 0x99, 0xaa)));

  const Result<fcfn::runtime::AuthorizationRecord> first = runtime->authorize();
  FCFN_CHECK_OK(first);
  const Result<fcfn::runtime::AuthorizationRecord> second = runtime->authorize();
  FCFN_CHECK(!second.ok());
  require_status("authorization table exhaustion", second.status().code(), StatusCode::Exhausted);

  // The refusal did not disturb the authorization that was already issued.
  const Result<fcfn::model::ContainmentPlan> planned =
      runtime->plan(PlanRequest{first.value().id});
  FCFN_CHECK_OK(planned);
  FCFN_CHECK(planned.value().claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK(planned.value().boundary.contains(ResourceId::unchecked("l0c0")));
}
