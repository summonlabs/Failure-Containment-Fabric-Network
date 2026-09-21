// FCFN persistence suite: torn-tail repair.
//
// Product proposition proved here: exactly one damage class is recoverable - a
// strict prefix of a record at the end of the log. Every truncation length from
// one byte to a whole record is repaired to the last complete record boundary,
// the exact byte count is reported, every complete record is replayed, and the
// repaired file is byte-identical to the prefix of complete records. A clean
// truncation on a record boundary is not reported as damage at all.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "store_support.hpp"

namespace {

using namespace fcfn::test::persist;

constexpr std::size_t kRecords = 4;
constexpr std::size_t kPayloadBytes = 16;
constexpr std::size_t kRecordBytes = fcfn::store::kRecordHeaderBytes + kPayloadBytes;

fcfn::store::RecordType record_type(std::size_t index) {
  return index % 2 == 0 ? fcfn::store::RecordType::PlanDecision
                        : fcfn::store::RecordType::BoundaryDecision;
}

/// Create a log of kRecords records and return the exact durable bytes.
std::vector<std::byte> write_reference_log(const std::filesystem::path& root) {
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(root);
  for (std::size_t index = 1; index <= kRecords; ++index) {
    require_ok("commit", store->commit(record_type(index), make_payload(index, kPayloadBytes)));
  }
  return read_bytes(wal_file(root, 1));
}

void truncate_to(const std::filesystem::path& path, std::size_t size) {
  std::error_code error;
  std::filesystem::resize_file(path, size, error);
  if (error) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "cannot truncate " + path.string() + " to " + std::to_string(size));
  }
}

void prove_repaired_prefix(const std::filesystem::path& wal,
                           const std::vector<std::byte>& reference,
                           std::size_t expected_records, const char* label) {
  const std::vector<std::byte> repaired = read_bytes(wal);
  const std::size_t expected_bytes = expected_records * kRecordBytes;
  if (repaired.size() != expected_bytes) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(label) + ": repaired log is " + std::to_string(repaired.size()) +
                           " bytes, expected " + std::to_string(expected_bytes));
  }
  const std::span<const std::byte> prefix(reference.data(), expected_bytes);
  if (!same_bytes(repaired, prefix)) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(label) + ": repaired log is not the prefix of complete records");
  }
}

}  // namespace

FCFN_TEST(torn_tail, every_truncation_length_is_repaired_to_the_last_complete_record) {
  const fcfn::test::TempDir dir("torn-every");
  const std::filesystem::path reference_root = dir.child("reference");
  const std::vector<std::byte> reference = write_reference_log(reference_root);
  FCFN_CHECK_EQ(reference.size(), kRecords * kRecordBytes);

  for (std::size_t cut = 1; cut <= kRecordBytes; ++cut) {
    const std::string label = "cut=" + std::to_string(cut);
    const std::filesystem::path root = dir.child("cut-" + std::to_string(cut));
    {
      const std::unique_ptr<fcfn::store::DurableStore> created = open_store(root);
      for (std::size_t index = 1; index <= kRecords; ++index) {
        require_ok("commit", created->commit(record_type(index), make_payload(index, kPayloadBytes)));
      }
    }
    const std::filesystem::path wal = wal_file(root, 1);
    FCFN_CHECK_EQ(size_of(wal), reference.size());
    truncate_to(wal, reference.size() - cut);
    // The log now holds kRecords-1 complete records plus cut bytes of the last.
    FCFN_CHECK_EQ(size_of(wal), reference.size() - cut);

    const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(root);
    // torn_tail_bytes is the number of bytes the store discarded from the end of
    // the file: the size of the incomplete record that was present there.
    const std::uint64_t expected_torn_bytes = static_cast<std::uint64_t>(
        (reference.size() - cut) - (kRecords - 1) * kRecordBytes);
    const bool lost_bytes_inside_a_record = expected_torn_bytes != 0;
    if (reopened->recovery().torn_tail != lost_bytes_inside_a_record) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         label + ": torn_tail=" + (reopened->recovery().torn_tail ? "true" : "false") +
                             " expected " +
                             (lost_bytes_inside_a_record ? "true" : "false"));
    }
    FCFN_CHECK_EQ(reopened->recovery().torn_tail_bytes, expected_torn_bytes);
    FCFN_CHECK_EQ(reopened->recovery().replayed_records, kRecords - 1);
    FCFN_CHECK_EQ(reopened->last_sequence().value(), static_cast<std::uint64_t>(kRecords - 1));
    FCFN_CHECK_EQ(reopened->replayed_frames().size(), kRecords - 1);
    for (std::size_t index = 0; index + 1 < kRecords; ++index) {
      const RecordView replayed = view_of(reopened->replayed_frames()[index]);
      FCFN_CHECK_EQ(replayed.type, static_cast<std::uint16_t>(record_type(index + 1)));
      FCFN_CHECK_EQ(replayed.sequence, static_cast<std::uint64_t>(index + 1));
      FCFN_CHECK(same_bytes(replayed.payload, make_payload(index + 1, kPayloadBytes)));
    }
    prove_repaired_prefix(wal, reference, kRecords - 1, label.c_str());
  }
}

FCFN_TEST(torn_tail, truncation_across_several_records_keeps_every_complete_record) {
  const fcfn::test::TempDir dir("torn-deep");
  const std::filesystem::path reference_root = dir.child("reference");
  const std::vector<std::byte> reference = write_reference_log(reference_root);

  struct Case {
    std::size_t cut;
    std::size_t expected_records;
  };
  // Each case removes the last record plus the stated number of earlier bytes.
  const std::vector<Case> cases{
      {kRecordBytes + 1, kRecords - 2},
      {kRecordBytes + kPayloadBytes, kRecords - 2},
      {kRecordBytes + kRecordBytes, kRecords - 2},
      {2 * kRecordBytes + 1, kRecords - 3},
  };

  for (const Case& item : cases) {
    const std::string label = "cut=" + std::to_string(item.cut);
    const std::filesystem::path root = dir.child("deep-" + std::to_string(item.cut));
    {
      const std::unique_ptr<fcfn::store::DurableStore> created = open_store(root);
      for (std::size_t index = 1; index <= kRecords; ++index) {
        require_ok("commit", created->commit(record_type(index), make_payload(index, kPayloadBytes)));
      }
    }
    const std::filesystem::path wal = wal_file(root, 1);
    truncate_to(wal, reference.size() - item.cut);

    const std::uint64_t expected_torn_bytes = static_cast<std::uint64_t>(
        (reference.size() - item.cut) - item.expected_records * kRecordBytes);
    const std::unique_ptr<fcfn::store::DurableStore> reopened = open_store(root);
    FCFN_CHECK_EQ(reopened->recovery().torn_tail, expected_torn_bytes != 0);
    FCFN_CHECK_EQ(reopened->recovery().torn_tail_bytes, expected_torn_bytes);
    FCFN_CHECK_EQ(reopened->recovery().replayed_records, item.expected_records);
    FCFN_CHECK_EQ(reopened->last_sequence().value(),
                  static_cast<std::uint64_t>(item.expected_records));
    prove_repaired_prefix(wal, reference, item.expected_records, label.c_str());
  }
}

FCFN_TEST(torn_tail, recovery_is_refused_when_torn_tail_repair_is_disabled) {
  const fcfn::test::TempDir dir("torn-disabled");
  const std::filesystem::path root = dir.child("store");
  {
    const std::unique_ptr<fcfn::store::DurableStore> created = open_store(root);
    for (std::size_t index = 1; index <= 3; ++index) {
      require_ok("commit", created->commit(record_type(index), make_payload(index, kPayloadBytes)));
    }
  }
  const std::filesystem::path wal = wal_file(root, 1);
  truncate_to(wal, 3 * kRecordBytes - 5);
  const std::vector<std::byte> damaged = read_bytes(wal);

  const Result<std::unique_ptr<fcfn::store::DurableStore>> refused =
      try_open_store(root, fcfn::kMaxRetainedSnapshots, false);
  FCFN_CHECK(!refused.ok());
  require_status("torn tail with recovery disabled", refused.status().code(), StatusCode::Corrupt);
  // A refusal never repairs: the damaged file is left exactly as it was found.
  FCFN_CHECK(same_bytes(read_bytes(wal), damaged));
}
