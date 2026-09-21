// FCFN persistence suite support: durable-file helpers and fixture builders.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_PERSIST_SUPPORT_HPP
#define FCFN_TEST_PERSIST_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/hash.hpp"
#include "fcfn/core/clock.hpp"
#include "fcfn/model/topology.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/store/record.hpp"
#include "fcfn/store/store.hpp"
#include "fixtures.hpp"
#include "temp_dir.hpp"
#include "test_harness.hpp"

namespace fcfn::test::persist {

using fcfn::CanonicalWriter;
using fcfn::Crc32c;
using fcfn::kMaxRecordPayloadBytes;
using fcfn::Result;
using fcfn::Sequence;
using fcfn::StatusCode;
using fcfn::VoidResult;

inline void require_ok(const char* what, const VoidResult& result) {
  if (!result.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(what) + " failed: " + result.status().to_string());
  }
}

inline void require_status(const char* what, StatusCode actual, StatusCode expected) {
  if (actual != expected) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(what) + ": expected " + to_string(expected) + " but got " +
                           to_string(actual));
  }
}

inline bool same_bytes(std::span<const std::byte> left, std::span<const std::byte> right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i] != right[i]) {
      return false;
    }
  }
  return true;
}

/// Deterministic, fixed-size record payload derived from the caller's index.
inline std::vector<std::byte> make_payload(std::uint64_t index, std::size_t size = 16) {
  CanonicalWriter writer;
  writer.u64(index);
  writer.u64(index * 0x9e3779b97f4a7c15ull + 0x1234ull);
  std::vector<std::byte> out(size, std::byte{0});
  const std::vector<std::byte>& data = writer.data();
  const std::size_t count = data.size() < size ? data.size() : size;
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = data[i];
  }
  return out;
}

inline std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  const Result<std::vector<std::byte>> result = fcfn::store::read_file(path, 1ull << 30);
  if (!result.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "cannot read " + path.string() + ": " + result.status().to_string());
  }
  return std::vector<std::byte>(result.value().begin(), result.value().end());
}

inline void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  require_ok("write durable file", fcfn::store::write_file_atomic(path, bytes, true));
}

inline void append_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  require_ok("append durable file", fcfn::store::append_file_durable(path, bytes, true));
}

inline std::uintmax_t size_of(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    ::fcfn::test::fail(__FILE__, __LINE__, "cannot stat " + path.string());
  }
  return size;
}

inline std::string hex16(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

inline std::filesystem::path wal_file(const std::filesystem::path& root, std::uint64_t segment) {
  return root / ("wal-" + hex16(segment) + ".fcfn");
}

inline std::filesystem::path snapshot_file(const std::filesystem::path& root, std::uint64_t sequence) {
  return root / ("snapshot-" + hex16(sequence) + ".fcfn");
}

/// The single write-ahead log segment present in a store root.
inline std::filesystem::path active_wal(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> found;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("wal-", 0) == 0 && name.size() > 4 &&
        name.compare(name.size() - 5, 5, ".fcfn") == 0) {
      found.push_back(entry.path());
    }
  }
  if (error || found.size() != 1) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       "expected exactly one log segment in " + root.string() + ", found " +
                           std::to_string(found.size()));
  }
  return found.front();
}

inline fcfn::store::DurableStore::Options store_options(
    const std::filesystem::path& root, std::size_t retained_snapshots,
    bool allow_torn_tail_recovery) {
  fcfn::store::DurableStore::Options options;
  options.root = root;
  options.retained_snapshots = retained_snapshots;
  options.durable_commit = true;
  options.allow_torn_tail_recovery = allow_torn_tail_recovery;
  return options;
}

/// Open without failing the test, so refusal classification can be asserted.
inline Result<std::unique_ptr<fcfn::store::DurableStore>> try_open_store(
    const std::filesystem::path& root, std::size_t retained_snapshots = fcfn::kMaxRetainedSnapshots,
    bool allow_torn_tail_recovery = true) {
  return fcfn::store::DurableStore::open(
      store_options(root, retained_snapshots, allow_torn_tail_recovery));
}

inline std::unique_ptr<fcfn::store::DurableStore> open_store(
    const std::filesystem::path& root, std::size_t retained_snapshots = fcfn::kMaxRetainedSnapshots,
    bool allow_torn_tail_recovery = true) {
  Result<std::unique_ptr<fcfn::store::DurableStore>> opened =
      try_open_store(root, retained_snapshots, allow_torn_tail_recovery);
  if (!opened.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__, "cannot open durable store at " + root.string() + ": " +
                                               opened.status().to_string());
  }
  return std::move(opened.value());
}

/// Rebuild the exact CURRENT pointer document the store writes.
inline std::vector<std::byte> current_document(std::uint64_t snapshot, std::uint64_t wal) {
  std::string text = "fcfn-store 1";
  text += "\nsnapshot ";
  text += std::to_string(snapshot);
  text += "\nwal ";
  text += std::to_string(wal);
  text += "\n";
  Crc32c crc;
  crc.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", crc.value());
  text += "crc ";
  text += buffer;
  text += "\n";
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

/// One replayed durable record, as the suite compares states.
struct RecordView {
  std::uint16_t type{0};
  std::uint64_t sequence{0};
  std::vector<std::byte> payload{};
};

inline RecordView view_of(const fcfn::store::Frame& frame) {
  RecordView view;
  view.type = static_cast<std::uint16_t>(frame.type);
  view.sequence = frame.sequence.value();
  view.payload = frame.payload;
  return view;
}

/// Canonical encoding of a recovered state: the suite's own state document.
inline std::vector<std::byte> state_document(const std::vector<RecordView>& records) {
  CanonicalWriter writer;
  writer.u32(static_cast<std::uint32_t>(records.size()));
  for (const RecordView& record : records) {
    writer.u16(record.type);
    writer.u64(record.sequence);
    writer.blob(record.payload);
  }
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

inline void append_frame(const std::filesystem::path& path, fcfn::store::RecordType type,
                         std::uint64_t sequence, std::span<const std::byte> payload) {
  const std::vector<std::byte> frame = fcfn::store::encode_record(type, Sequence{sequence}, payload);
  append_bytes(path, frame);
}

// ---------------------------------------------------------------------------
// Containment runtime fixtures
// ---------------------------------------------------------------------------

/// Layered fixture whose failure source is containable, so a containment plan
/// over it is PROVEN rather than degraded by an uncontainable source.
inline fcfn::model::TopologySpec contained_source_topology(std::uint64_t generation) {
  fcfn::model::TopologySpec spec = layered_topology(4, 2, 3, generation);
  spec.nodes[0].containable = true;  // "l0c0", the failure source
  return spec;
}

inline fcfn::runtime::RuntimeConfig durable_config(const std::filesystem::path& root) {
  fcfn::runtime::RuntimeConfig config;
  config.store_root = root;
  config.persist = true;
  config.durable_commit = true;
  config.policy = fcfn::model::default_policy();
  config.topology = contained_source_topology(1);
  config.require_evidence_confirmation_in_current_boot = true;
  return config;
}

inline fcfn::model::FailureObservation source_observation(std::uint64_t generation,
                                                          std::uint64_t digest_hi,
                                                          std::uint64_t digest_lo) {
  fcfn::model::FailureObservation observation;
  observation.resource = fcfn::model::ResourceId::unchecked("l0c0");
  observation.state = fcfn::model::EvidenceState::Present;
  observation.generation = fcfn::model::EvidenceGeneration{generation};
  observation.source_digest = fcfn::model::Digest{digest_hi, digest_lo};
  return observation;
}

}  // namespace fcfn::test::persist

#endif  // FCFN_TEST_PERSIST_SUPPORT_HPP
