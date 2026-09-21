// FCFN - durable record framing.
//
// Every durable unit is a framed record: magic, format version, record type,
// declared payload length, monotonic sequence, payload integrity, and header
// integrity. Declared lengths are refused before allocation. Decoding is total
// and never silently accepts a damaged unit: a torn tail is reported as such,
// while an integrity failure inside a complete unit is corruption.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_STORE_RECORD_HPP
#define FCFN_STORE_RECORD_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::store {

/// Durable record kinds. Numeric values are part of the format contract.
enum class RecordType : std::uint16_t {
  TopologyDefinition = 1,
  PolicyDefinition = 2,
  DetectionRecord = 3,
  EvidenceReconfirmed = 4,
  PlanDecision = 5,
  BoundaryDecision = 6,
  TransitionDecision = 7,
  ApplyAttempt = 8,
  ApplyAcknowledgement = 9,
  EffectVerification = 10,
  AuthorizationIssued = 11,
  Fence = 12,
  EpochAdvance = 13,
  SnapshotPayload = 14,
  StartupMarker = 15,
  ShutdownMarker = 16,
};

[[nodiscard]] const char* to_string(RecordType value) noexcept;
[[nodiscard]] bool parse_record_type(std::string_view token, RecordType& out) noexcept;
[[nodiscard]] bool is_valid_record_type(std::uint16_t raw) noexcept;

/// "FCFN" in little-endian byte order.
inline constexpr std::uint32_t kRecordMagic = 0x4e464346u;
inline constexpr std::uint16_t kRecordFormatVersion = 1;
inline constexpr std::size_t kRecordHeaderBytes = 28;
inline constexpr char kSnapshotFileMagic[8] = {'F', 'C', 'F', 'N', 'S', 'N', 'A', 'P'};
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

struct RecordHeader {
  std::uint32_t magic{0};
  std::uint16_t version{0};
  std::uint16_t type{0};
  std::uint32_t payload_length{0};
  std::uint64_t sequence{0};
  std::uint32_t payload_crc{0};
  std::uint32_t header_crc{0};
};

/// Outcome of decoding one framed unit.
enum class FrameStatus : std::uint8_t {
  /// A complete, integrity-checked record.
  Ok = 0,
  /// No bytes remain: the stream ended cleanly on a record boundary.
  NoMoreData = 1,
  /// A strict prefix of a record is present at the end of the stream. This is
  /// the only recoverable damage class.
  TornTail = 2,
  /// A complete-looking unit failed integrity or violated framing rules.
  Corrupt = 3,
  /// Unsupported format version.
  UnsupportedVersion = 4,
  /// Record type outside the defined domain.
  InvalidType = 5,
  /// Declared payload length exceeds the configured bound.
  Oversized = 6,
  /// Sequence did not advance as required.
  SequenceRegression = 7,
  /// Bytes remained after the end of the expected content.
  TrailingGarbage = 8,
};

[[nodiscard]] const char* to_string(FrameStatus value) noexcept;

struct Frame {
  RecordType type{RecordType::StartupMarker};
  Sequence sequence{};
  std::vector<std::byte> payload{};
  std::size_t encoded_size{0};
};

struct DecodeOutcome {
  FrameStatus status{FrameStatus::NoMoreData};
  Frame frame{};
  std::string detail{};
  /// Bytes consumed from the front of the input.
  std::size_t consumed{0};
};

/// Encode one record. The caller owns sequencing and payload bounds.
[[nodiscard]] std::vector<std::byte> encode_record(RecordType type, Sequence sequence,
                                                   std::span<const std::byte> payload);

/// Decode one record from the front of the buffer. Total: every input either
/// yields a record or a classified failure.
[[nodiscard]] DecodeOutcome decode_record(std::span<const std::byte> bytes);

/// True when the status means "this stream ends here with recoverable damage".
[[nodiscard]] constexpr bool is_recoverable_tail(FrameStatus status) noexcept {
  return status == FrameStatus::TornTail || status == FrameStatus::NoMoreData;
}

// ---------------------------------------------------------------------------
// File helpers (durable ordering lives here, not in callers)
// ---------------------------------------------------------------------------

/// Read a whole file. Bounded: refuses files larger than max_bytes.
[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                                       std::uint64_t max_bytes);

/// Write bytes to a temporary sibling file, flush it durably, then atomically
/// replace the target. A crash leaves either the old or the new content.
[[nodiscard]] VoidResult write_file_atomic(const std::filesystem::path& target,
                                           std::span<const std::byte> bytes, bool durable);

/// Append bytes and flush durably.
[[nodiscard]] VoidResult append_file_durable(const std::filesystem::path& path,
                                             std::span<const std::byte> bytes, bool durable);

/// Flush a file's contents to stable storage.
[[nodiscard]] VoidResult sync_file(const std::filesystem::path& path);

/// Remove a file if it exists; missing files are not an error.
[[nodiscard]] VoidResult remove_file_if_present(const std::filesystem::path& path);

}  // namespace fcfn::store

#endif  // FCFN_STORE_RECORD_HPP
