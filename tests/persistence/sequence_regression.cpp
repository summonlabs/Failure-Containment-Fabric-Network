// FCFN persistence suite: log sequence discipline.
//
// Product proposition proved here: a durable log is a monotone sequence
// authority. A record whose sequence does not advance past its predecessor is
// refused with SequenceRegression rather than replayed or silently rewritten,
// and a record already represented by the snapshot is skipped rather than
// applied twice.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "store_support.hpp"

namespace {

using namespace fcfn::test::persist;

constexpr std::size_t kPayloadBytes = 16;

/// Replace the active log with hand-crafted frames.
void craft_log(const std::filesystem::path& root, const std::vector<std::uint64_t>& sequences) {
  const std::unique_ptr<fcfn::store::DurableStore> created = open_store(root);
  const std::filesystem::path wal = wal_file(root, 1);
  std::vector<std::byte> bytes;
  for (const std::uint64_t sequence : sequences) {
    const std::vector<std::byte> payload = make_payload(sequence, kPayloadBytes);
    const std::vector<std::byte> frame = fcfn::store::encode_record(
        fcfn::store::RecordType::DetectionRecord, Sequence{sequence}, payload);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }
  FCFN_CHECK(created != nullptr);
  write_bytes(wal, bytes);
}

/// Records whose sequence order is not strictly ascending must be refused.
void expect_sequence_regression(const fcfn::test::TempDir& dir, const char* label,
                                std::vector<std::uint64_t> sequences) {
  const std::filesystem::path root = dir.child(label);
  craft_log(root, sequences);
  const std::filesystem::path wal = wal_file(root, 1);
  const std::vector<std::byte> before = read_bytes(wal);
  const Result<std::unique_ptr<fcfn::store::DurableStore>> refused = try_open_store(root);
  if (refused.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + label + "': the log was accepted");
  }
  if (refused.status().code() != StatusCode::SequenceRegression) {
    ::fcfn::test::fail(__FILE__, __LINE__, std::string("case '") + label + "': expected " +
                                               to_string(StatusCode::SequenceRegression) +
                                               " but got " + refused.status().to_string());
  }
  if (!same_bytes(read_bytes(wal), before)) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + label + "': the refused log was rewritten");
  }
}

}  // namespace

FCFN_TEST(sequence, a_log_whose_sequences_do_not_advance_is_refused) {
  const fcfn::test::TempDir dir("sequence-regression");
  expect_sequence_regression(dir, "backwards", {5, 4});
  expect_sequence_regression(dir, "repeated", {5, 5});
  expect_sequence_regression(dir, "restart-from-one", {2, 1});
  expect_sequence_regression(dir, "late-regression", {1, 2, 9, 8});
  expect_sequence_regression(dir, "zero-then-zero", {0, 0});
}

FCFN_TEST(sequence, a_strictly_advancing_log_replays_every_record) {
  const fcfn::test::TempDir dir("sequence-advancing");
  const std::filesystem::path root = dir.child("store");
  craft_log(root, {1, 2, 3, 7});
  const std::unique_ptr<fcfn::store::DurableStore> opened = open_store(root);
  FCFN_CHECK_EQ(opened->recovery().replayed_records, static_cast<std::size_t>(4));
  FCFN_CHECK_EQ(opened->last_sequence().value(), static_cast<std::uint64_t>(7));
  for (std::size_t index = 0; index < opened->replayed_frames().size(); ++index) {
    const RecordView view = view_of(opened->replayed_frames()[index]);
    FCFN_CHECK_EQ(view.type,
                  static_cast<std::uint16_t>(fcfn::store::RecordType::DetectionRecord));
    FCFN_CHECK(same_bytes(view.payload, make_payload(view.sequence, kPayloadBytes)));
  }
}

FCFN_TEST(sequence, a_record_already_covered_by_the_snapshot_is_skipped_not_reapplied) {
  const fcfn::test::TempDir dir("sequence-skip");
  const std::filesystem::path root = dir.child("store");
  std::vector<RecordView> covered;
  {
    const std::unique_ptr<fcfn::store::DurableStore> store = open_store(root);
    for (std::size_t index = 1; index <= 3; ++index) {
      const std::vector<std::byte> payload = make_payload(index, kPayloadBytes);
      require_ok("commit", store->commit(fcfn::store::RecordType::PlanDecision, payload));
      RecordView view;
      view.type = static_cast<std::uint16_t>(fcfn::store::RecordType::PlanDecision);
      view.sequence = index;
      view.payload = payload;
      covered.push_back(std::move(view));
    }
    require_ok("write snapshot", store->write_snapshot(state_document(covered)));
  }

  // The new segment already contains a duplicate of the last snapshot sequence
  // followed by a genuinely new record.
  const std::filesystem::path wal = wal_file(root, 2);
  append_frame(wal, fcfn::store::RecordType::PlanDecision, 3, make_payload(3, kPayloadBytes));
  append_frame(wal, fcfn::store::RecordType::PlanDecision, 4, make_payload(4, kPayloadBytes));

  const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(root);
  FCFN_CHECK_EQ(reopened->recovery().skipped_records, static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(reopened->recovery().replayed_records, static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(reopened->replayed_frames().size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(reopened->replayed_frames().front().sequence.value(), static_cast<std::uint64_t>(4));
  FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(4));
}
