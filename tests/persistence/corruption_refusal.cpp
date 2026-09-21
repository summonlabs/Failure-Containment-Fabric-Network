// FCFN persistence suite: corruption is never silently truncated.
//
// Product proposition proved here: only a strict prefix at the end of a log is
// recoverable. Damage inside a complete record, trailing garbage, a damaged or
// absent CURRENT pointer, and an unsupported snapshot format are all refused
// with an explicit status, and a refusal never rewrites the damaged artifact.
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

constexpr std::size_t kPayloadBytes = 16;
constexpr std::size_t kRecordBytes = fcfn::store::kRecordHeaderBytes + kPayloadBytes;

std::string to_text(std::span<const std::byte> bytes) {
  std::string out(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i] = static_cast<char>(bytes[i]);
  }
  return out;
}

std::vector<std::byte> from_text(const std::string& text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

/// Store with three committed records; returns the active log path.
std::filesystem::path prepare_store(const std::filesystem::path& root, std::size_t records) {
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(root);
  for (std::size_t index = 1; index <= records; ++index) {
    require_ok("commit", store->commit(fcfn::store::RecordType::PlanDecision,
                                       make_payload(index, kPayloadBytes)));
  }
  return wal_file(root, 1);
}

/// Store with a snapshot covering two committed records; returns the snapshot
/// file path.
std::filesystem::path prepare_snapshot_store(const std::filesystem::path& root) {
  const std::unique_ptr<fcfn::store::DurableStore> store = open_store(root);
  std::vector<RecordView> records;
  for (std::size_t index = 1; index <= 2; ++index) {
    const std::vector<std::byte> payload = make_payload(index, kPayloadBytes);
    require_ok("commit", store->commit(fcfn::store::RecordType::PlanDecision, payload));
    RecordView view;
    view.type = static_cast<std::uint16_t>(fcfn::store::RecordType::PlanDecision);
    view.sequence = index;
    view.payload = payload;
    records.push_back(std::move(view));
  }
  require_ok("write snapshot", store->write_snapshot(state_document(records)));
  return snapshot_file(root, 1);
}

/// Reopening must refuse with exactly this status and must not rewrite the file.
void expect_refusal(const char* what, const std::filesystem::path& root,
                    const std::filesystem::path& damaged_file, StatusCode expected) {
  const std::vector<std::byte> before = read_bytes(damaged_file);
  const Result<std::unique_ptr<fcfn::store::DurableStore>> refused = try_open_store(root);
  if (refused.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what + "': the damaged store was accepted");
  }
  if (refused.status().code() != expected) {
    ::fcfn::test::fail(__FILE__, __LINE__, std::string("case '") + what + "': expected " +
                                               to_string(expected) + " but got " +
                                               refused.status().to_string());
  }
  if (!same_bytes(read_bytes(damaged_file), before)) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what +
                           "': the refusal rewrote the damaged artifact instead of leaving it");
  }
}

void truncate_to(const std::filesystem::path& path, std::size_t size) {
  std::error_code error;
  std::filesystem::resize_file(path, size, error);
  if (error) {
    ::fcfn::test::fail(__FILE__, __LINE__, "cannot truncate " + path.string());
  }
}

}  // namespace

FCFN_TEST(corruption, damage_inside_a_complete_record_is_never_repaired) {
  const fcfn::test::TempDir dir("corrupt-record");

  {
    const std::filesystem::path root = dir.child("payload");
    const std::filesystem::path wal = prepare_store(root, 3);
    std::vector<std::byte> damaged = read_bytes(wal);
    FCFN_CHECK_EQ(damaged.size(), 3 * kRecordBytes);
    // Second record, inside its payload.
    const std::size_t offset = kRecordBytes + fcfn::store::kRecordHeaderBytes + 5;
    damaged[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(damaged[offset]) ^ 0x20u);
    write_bytes(wal, damaged);
    expect_refusal("payload byte flipped in a complete record", root, wal, StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("header");
    const std::filesystem::path wal = prepare_store(root, 3);
    std::vector<std::byte> damaged = read_bytes(wal);
    // First record, inside its sequence field: the payload is untouched, so only
    // the header integrity check can catch this.
    damaged[12] = static_cast<std::byte>(static_cast<std::uint8_t>(damaged[12]) ^ 0x01u);
    write_bytes(wal, damaged);
    expect_refusal("header byte flipped in a complete record", root, wal, StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("trailing");
    const std::filesystem::path wal = prepare_store(root, 3);
    std::vector<std::byte> damaged = read_bytes(wal);
    damaged.insert(damaged.end(), 40, std::byte{0xab});
    write_bytes(wal, damaged);
    expect_refusal("trailing garbage after a valid log", root, wal, StatusCode::Corrupt);
  }
}

FCFN_TEST(corruption, a_damaged_current_pointer_is_refused) {
  const fcfn::test::TempDir dir("corrupt-current");

  {
    const std::filesystem::path root = dir.child("zero-length");
    const std::filesystem::path wal = prepare_store(root, 2);
    const std::filesystem::path current = root / "CURRENT";
    write_bytes(current, {});
    expect_refusal("zero-length CURRENT", root, current, StatusCode::Corrupt);
    FCFN_CHECK(std::filesystem::exists(wal));
  }
  {
    const std::filesystem::path root = dir.child("bad-crc");
    prepare_store(root, 2);
    const std::filesystem::path current = root / "CURRENT";
    std::vector<std::byte> damaged = read_bytes(current);
    damaged[damaged.size() - 2] =
        static_cast<std::byte>(static_cast<std::uint8_t>(damaged[damaged.size() - 2]) ^ 0x01u);
    write_bytes(current, damaged);
    expect_refusal("CURRENT with a corrupted integrity field", root, current,
                   StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("stale-crc");
    prepare_store(root, 2);
    const std::filesystem::path current = root / "CURRENT";
    std::string text = to_text(read_bytes(current));
    const std::size_t position = text.find("wal 1");
    FCFN_CHECK(position != std::string::npos);
    text.replace(position, 5, "wal 2");
    write_bytes(current, from_text(text));
    expect_refusal("CURRENT edited without updating its integrity field", root, current,
                   StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("garbage");
    prepare_store(root, 2);
    const std::filesystem::path current = root / "CURRENT";
    write_bytes(current, from_text("not a pointer at all\n"));
    expect_refusal("CURRENT that is not a pointer document", root, current, StatusCode::Corrupt);
  }
}

FCFN_TEST(corruption, a_current_pointer_at_a_missing_snapshot_is_not_ignored) {
  const fcfn::test::TempDir dir("corrupt-missing-snapshot");
  const std::filesystem::path root = dir.child("store");
  prepare_store(root, 2);
  const std::filesystem::path current = root / "CURRENT";
  // A well-formed pointer naming a snapshot that was never written.
  write_bytes(current, current_document(5, 1));
  expect_refusal("CURRENT pointing at a missing snapshot", root, current, StatusCode::NotFound);
}

FCFN_TEST(corruption, store_files_without_a_current_pointer_are_refused) {
  const fcfn::test::TempDir dir("corrupt-no-current");
  const std::filesystem::path root = dir.child("store");
  const std::filesystem::path wal = prepare_store(root, 2);
  const std::filesystem::path current = root / "CURRENT";
  std::error_code error;
  FCFN_CHECK(std::filesystem::remove(current, error));
  FCFN_CHECK(!error);
  const std::vector<std::byte> before = read_bytes(wal);
  const Result<std::unique_ptr<fcfn::store::DurableStore>> refused = try_open_store(root);
  FCFN_CHECK(!refused.ok());
  require_status("store files without CURRENT", refused.status().code(), StatusCode::Corrupt);
  FCFN_CHECK(same_bytes(read_bytes(wal), before));
}

FCFN_TEST(corruption, snapshot_damage_is_refused_rather_than_ignored) {
  const fcfn::test::TempDir dir("corrupt-snapshot");

  {
    const std::filesystem::path root = dir.child("version");
    const std::filesystem::path snapshot = prepare_snapshot_store(root);
    std::vector<std::byte> damaged = read_bytes(snapshot);
    FCFN_CHECK(damaged.size() > 18);
    damaged[8] = std::byte{2};  // unsupported snapshot format version
    damaged[9] = std::byte{0};
    write_bytes(snapshot, damaged);
    expect_refusal("unsupported snapshot format version", root, snapshot,
                   StatusCode::VersionUnsupported);
  }
  {
    const std::filesystem::path root = dir.child("payload-crc");
    const std::filesystem::path snapshot = prepare_snapshot_store(root);
    std::vector<std::byte> damaged = read_bytes(snapshot);
    const std::size_t offset = damaged.size() - 3;
    damaged[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(damaged[offset]) ^ 0x08u);
    write_bytes(snapshot, damaged);
    expect_refusal("snapshot payload integrity failure", root, snapshot, StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("short");
    const std::filesystem::path snapshot = prepare_snapshot_store(root);
    const std::size_t original = static_cast<std::size_t>(size_of(snapshot));
    truncate_to(snapshot, original - 1);
    // A short snapshot is corruption: what is missing cannot be recovered, so it
    // is never repaired and never partially applied.
    expect_refusal("truncated snapshot file", root, snapshot, StatusCode::Corrupt);
  }
  {
    const std::filesystem::path root = dir.child("long");
    const std::filesystem::path snapshot = prepare_snapshot_store(root);
    std::vector<std::byte> damaged = read_bytes(snapshot);
    damaged.push_back(std::byte{0x00});
    write_bytes(snapshot, damaged);
    expect_refusal("snapshot file with trailing bytes", root, snapshot,
                   StatusCode::TrailingGarbage);
  }
  {
    const std::filesystem::path root = dir.child("tiny");
    const std::filesystem::path snapshot = prepare_snapshot_store(root);
    write_bytes(snapshot, from_text("FCFN"));
    expect_refusal("snapshot file shorter than its header", root, snapshot, StatusCode::Corrupt);
  }
}
