// FCFN persistence suite: fresh creation, commit/replay, and snapshot rotation.
//
// Product proposition proved here: the durable log is exactly the sequence of
// framed decisions that were committed, replay reproduces them in order, and a
// snapshot plus the log records written after it reconstructs the identical
// durable state, compared as canonical encodings.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "store_support.hpp"

namespace {

using namespace fcfn::test::persist;

const std::vector<fcfn::store::RecordType>& sample_types() {
  static const std::vector<fcfn::store::RecordType> types{
      fcfn::store::RecordType::TopologyDefinition, fcfn::store::RecordType::PolicyDefinition,
      fcfn::store::RecordType::DetectionRecord,    fcfn::store::RecordType::PlanDecision,
      fcfn::store::RecordType::BoundaryDecision,   fcfn::store::RecordType::TransitionDecision,
      fcfn::store::RecordType::ApplyAttempt,       fcfn::store::RecordType::EffectVerification};
  return types;
}

}  // namespace

FCFN_TEST(store, a_fresh_store_publishes_a_pointer_and_an_empty_segment) {
  const fcfn::test::TempDir dir("store-fresh");
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());

  FCFN_CHECK(store->recovery().fresh);
  FCFN_CHECK(!store->recovery().torn_tail);
  FCFN_CHECK_EQ(store->recovery().torn_tail_bytes, static_cast<std::uint64_t>(0));
  FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(0));
  FCFN_CHECK_EQ(store->wal_segment().value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(store->snapshot_sequence().value(), static_cast<std::uint64_t>(0));
  FCFN_CHECK(std::filesystem::exists(store->current_path()));
  FCFN_CHECK(std::filesystem::exists(store->wal_path()));
  FCFN_CHECK_EQ(size_of(store->wal_path()), static_cast<std::uintmax_t>(0));
  FCFN_CHECK_EQ(store->replayed_frames().size(), static_cast<std::size_t>(0));
  FCFN_CHECK(store->snapshot_path() == snapshot_file(dir.path(), 0));
}

FCFN_TEST(store, reopening_an_untouched_store_is_a_recovery_not_a_fresh_start) {
  const fcfn::test::TempDir dir("store-reopen");
  {
    const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());
    FCFN_CHECK(store->recovery().fresh);
  }
  const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(dir.path());
  FCFN_CHECK(!reopened->recovery().fresh);
  FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(0));
  FCFN_CHECK_EQ(reopened->recovery().replayed_records, static_cast<std::size_t>(0));
  FCFN_CHECK_EQ(reopened->wal_segment().value(), static_cast<std::uint64_t>(1));
}

FCFN_TEST(store, commit_and_replay_round_trip_every_record) {
  const fcfn::test::TempDir dir("store-commit");
  const std::size_t record_count = sample_types().size();
  std::vector<RecordView> expected;
  std::vector<std::byte> expected_log;

  {
    const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());
    for (std::size_t index = 0; index < record_count; ++index) {
      const std::vector<std::byte> payload = make_payload(index + 1);
      require_ok("commit", store->commit(sample_types()[index], payload));
      FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(index + 1));
      RecordView view;
      view.type = static_cast<std::uint16_t>(sample_types()[index]);
      view.sequence = index + 1;
      view.payload = payload;
      expected.push_back(std::move(view));
      const std::vector<std::byte> frame =
          fcfn::store::encode_record(sample_types()[index], Sequence{index + 1}, payload);
      expected_log.insert(expected_log.end(), frame.begin(), frame.end());
    }
    // The durable log is exactly the concatenation of the framed records.
    const std::vector<std::byte> on_disk = read_bytes(store->wal_path());
    FCFN_CHECK(same_bytes(on_disk, expected_log));
  }

  const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(dir.path());
  FCFN_CHECK(!reopened->recovery().fresh);
  FCFN_CHECK_EQ(reopened->recovery().replayed_records, record_count);
  FCFN_CHECK_EQ(reopened->recovery().skipped_records, static_cast<std::size_t>(0));
  FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(record_count));
  FCFN_CHECK_EQ(reopened->replayed_frames().size(), record_count);
  for (std::size_t index = 0; index < record_count; ++index) {
    const RecordView replayed = view_of(reopened->replayed_frames()[index]);
    FCFN_CHECK_EQ(replayed.type, expected[index].type);
    FCFN_CHECK_EQ(replayed.sequence, expected[index].sequence);
    FCFN_CHECK(same_bytes(replayed.payload, expected[index].payload));
  }
}

FCFN_TEST(store, snapshot_plus_log_replay_reconstructs_identical_state) {
  const fcfn::test::TempDir dir("store-snapshot");
  std::vector<RecordView> before_snapshot;
  std::vector<RecordView> after_snapshot;

  {
    const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path());
    for (std::size_t index = 0; index < 4; ++index) {
      const std::vector<std::byte> payload = make_payload(index + 1);
      require_ok("commit before snapshot", store->commit(sample_types()[index], payload));
      RecordView view;
      view.type = static_cast<std::uint16_t>(sample_types()[index]);
      view.sequence = index + 1;
      view.payload = payload;
      before_snapshot.push_back(std::move(view));
    }
    const std::vector<std::byte> snapshot_payload = state_document(before_snapshot);
    require_ok("write snapshot", store->write_snapshot(snapshot_payload));
    FCFN_CHECK_EQ(store->snapshot_sequence().value(), static_cast<std::uint64_t>(1));
    FCFN_CHECK_EQ(store->wal_segment().value(), static_cast<std::uint64_t>(2));
    FCFN_CHECK(store->wal_path() == wal_file(dir.path(), 2));
    FCFN_CHECK(std::filesystem::exists(snapshot_file(dir.path(), 1)));
    // Rotation retires the superseded segment rather than appending to it.
    FCFN_CHECK(!std::filesystem::exists(wal_file(dir.path(), 1)));

    for (std::size_t index = 0; index < 3; ++index) {
      const std::vector<std::byte> payload = make_payload(index + 5);
      require_ok("commit after snapshot", store->commit(sample_types()[index], payload));
      RecordView view;
      view.type = static_cast<std::uint16_t>(sample_types()[index]);
      view.sequence = index + 5;
      view.payload = payload;
      after_snapshot.push_back(std::move(view));
    }
    FCFN_CHECK_EQ(store->last_sequence().value(), static_cast<std::uint64_t>(7));
  }

  const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(dir.path());
  const std::vector<std::byte> expected_snapshot = state_document(before_snapshot);
  FCFN_CHECK(same_bytes(reopened->recovery().snapshot_payload, expected_snapshot));
  FCFN_CHECK_EQ(reopened->recovery().snapshot_sequence.value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(reopened->recovery().wal_segment.value(), static_cast<std::uint64_t>(2));
  FCFN_CHECK_EQ(reopened->snapshot_sequence().value(), static_cast<std::uint64_t>(1));
  FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(7));
  FCFN_CHECK_EQ(reopened->recovery().replayed_records, after_snapshot.size());
  FCFN_CHECK_EQ(reopened->replayed_frames().size(), after_snapshot.size());
  for (std::size_t index = 0; index < after_snapshot.size(); ++index) {
    const RecordView replayed = view_of(reopened->replayed_frames()[index]);
    FCFN_CHECK_EQ(replayed.type, after_snapshot[index].type);
    FCFN_CHECK_EQ(replayed.sequence, after_snapshot[index].sequence);
    FCFN_CHECK(same_bytes(replayed.payload, after_snapshot[index].payload));
  }

  // Identical state: the recovered snapshot plus the replayed records encode to
  // exactly the same canonical document as the original full sequence.
  std::vector<RecordView> reconstructed = before_snapshot;
  for (const fcfn::store::Frame& frame : reopened->replayed_frames()) {
    reconstructed.push_back(view_of(frame));
  }
  std::vector<RecordView> original = before_snapshot;
  original.insert(original.end(), after_snapshot.begin(), after_snapshot.end());
  FCFN_CHECK_EQ(reconstructed.size(), static_cast<std::size_t>(7));
  FCFN_CHECK(same_bytes(state_document(reconstructed), state_document(original)));
}

FCFN_TEST(store, snapshot_rotation_retains_only_the_configured_number_of_snapshots) {
  const fcfn::test::TempDir dir("store-rotation");
  {
    const std::unique_ptr<fcfn::store::DurableStore> store = open_store(dir.path(), 1);
    for (std::uint64_t round = 1; round <= 3; ++round) {
      const std::vector<std::byte> payload = make_payload(round);
      require_ok("commit", store->commit(fcfn::store::RecordType::PlanDecision, payload));
      std::vector<RecordView> records;
      for (std::uint64_t index = 1; index <= round; ++index) {
        RecordView view;
        view.type = static_cast<std::uint16_t>(fcfn::store::RecordType::PlanDecision);
        view.sequence = index;
        view.payload = make_payload(index);
        records.push_back(std::move(view));
      }
      require_ok("write snapshot", store->write_snapshot(state_document(records)));
    }
    FCFN_CHECK_EQ(store->snapshot_sequence().value(), static_cast<std::uint64_t>(3));
    FCFN_CHECK_EQ(store->wal_segment().value(), static_cast<std::uint64_t>(4));
  }
  FCFN_CHECK(std::filesystem::exists(snapshot_file(dir.path(), 3)));
  FCFN_CHECK(!std::filesystem::exists(snapshot_file(dir.path(), 2)));
  FCFN_CHECK(!std::filesystem::exists(snapshot_file(dir.path(), 1)));
  FCFN_CHECK(std::filesystem::exists(wal_file(dir.path(), 4)));
  FCFN_CHECK(!std::filesystem::exists(wal_file(dir.path(), 3)));
  FCFN_CHECK(!std::filesystem::exists(wal_file(dir.path(), 2)));
  FCFN_CHECK(!std::filesystem::exists(wal_file(dir.path(), 1)));

  // The rotated store still recovers to the newest snapshot.
  const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(dir.path(), 1);
  FCFN_CHECK_EQ(reopened->snapshot_sequence().value(), static_cast<std::uint64_t>(3));
  FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(3));
  FCFN_CHECK_EQ(reopened->recovery().replayed_records, static_cast<std::size_t>(0));
  std::vector<RecordView> expected;
  for (std::uint64_t index = 1; index <= 3; ++index) {
    RecordView view;
    view.type = static_cast<std::uint16_t>(fcfn::store::RecordType::PlanDecision);
    view.sequence = index;
    view.payload = make_payload(index);
    expected.push_back(std::move(view));
  }
  FCFN_CHECK(same_bytes(reopened->recovery().snapshot_payload, state_document(expected)));
}
