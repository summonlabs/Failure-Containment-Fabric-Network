// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/store/record.hpp"

#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/hash.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fcfn::store {
namespace {

struct TypeToken {
  RecordType value;
  const char* token;
};

constexpr TypeToken kTypeTokens[] = {
    {RecordType::TopologyDefinition, "topology_definition"},
    {RecordType::PolicyDefinition, "policy_definition"},
    {RecordType::DetectionRecord, "detection_record"},
    {RecordType::EvidenceReconfirmed, "evidence_reconfirmed"},
    {RecordType::PlanDecision, "plan_decision"},
    {RecordType::BoundaryDecision, "boundary_decision"},
    {RecordType::TransitionDecision, "transition_decision"},
    {RecordType::ApplyAttempt, "apply_attempt"},
    {RecordType::ApplyAcknowledgement, "apply_acknowledgement"},
    {RecordType::EffectVerification, "effect_verification"},
    {RecordType::AuthorizationIssued, "authorization_issued"},
    {RecordType::Fence, "fence"},
    {RecordType::EpochAdvance, "epoch_advance"},
    {RecordType::SnapshotPayload, "snapshot_payload"},
    {RecordType::StartupMarker, "startup_marker"},
    {RecordType::ShutdownMarker, "shutdown_marker"},
};

struct StatusToken {
  FrameStatus value;
  const char* token;
};

constexpr StatusToken kStatusTokens[] = {
    {FrameStatus::Ok, "ok"},
    {FrameStatus::NoMoreData, "no_more_data"},
    {FrameStatus::TornTail, "torn_tail"},
    {FrameStatus::Corrupt, "corrupt"},
    {FrameStatus::UnsupportedVersion, "unsupported_version"},
    {FrameStatus::InvalidType, "invalid_type"},
    {FrameStatus::Oversized, "oversized"},
    {FrameStatus::SequenceRegression, "sequence_regression"},
    {FrameStatus::TrailingGarbage, "trailing_garbage"},
};

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xffu));
  out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                    (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8u * i);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8u * i);
  }
  return value;
}

std::vector<std::byte> encode_header(const RecordHeader& header) {
  std::vector<std::byte> out;
  out.reserve(kRecordHeaderBytes);
  put_u32(out, header.magic);
  put_u16(out, header.version);
  put_u16(out, header.type);
  put_u32(out, header.payload_length);
  put_u64(out, header.sequence);
  put_u32(out, header.payload_crc);
  put_u32(out, header.header_crc);
  return out;
}

}  // namespace

const char* to_string(RecordType value) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_record_type";
}

bool parse_record_type(std::string_view token, RecordType& out) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool is_valid_record_type(std::uint16_t raw) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (static_cast<std::uint16_t>(entry.value) == raw) {
      return true;
    }
  }
  return false;
}

const char* to_string(FrameStatus value) noexcept {
  for (const StatusToken& entry : kStatusTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_frame_status";
}

std::vector<std::byte> encode_record(RecordType type, Sequence sequence,
                                     std::span<const std::byte> payload) {
  RecordHeader header;
  header.magic = kRecordMagic;
  header.version = kRecordFormatVersion;
  header.type = static_cast<std::uint16_t>(type);
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  header.sequence = sequence.value();
  header.payload_crc = Crc32c::compute(payload);

  std::vector<std::byte> header_bytes;
  header_bytes.reserve(kRecordHeaderBytes);
  put_u32(header_bytes, header.magic);
  put_u16(header_bytes, header.version);
  put_u16(header_bytes, header.type);
  put_u32(header_bytes, header.payload_length);
  put_u64(header_bytes, header.sequence);
  put_u32(header_bytes, header.payload_crc);
  header.header_crc = Crc32c::compute(header_bytes);

  std::vector<std::byte> out = encode_header(header);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

DecodeOutcome decode_record(std::span<const std::byte> bytes) {
  DecodeOutcome outcome;
  if (bytes.empty()) {
    outcome.status = FrameStatus::NoMoreData;
    return outcome;
  }
  if (bytes.size() < kRecordHeaderBytes) {
    outcome.status = FrameStatus::TornTail;
    outcome.detail = "fewer bytes than a record header";
    return outcome;
  }

  RecordHeader header;
  header.magic = get_u32(bytes, 0);
  header.version = get_u16(bytes, 4);
  header.type = get_u16(bytes, 6);
  header.payload_length = get_u32(bytes, 8);
  header.sequence = get_u64(bytes, 12);
  header.payload_crc = get_u32(bytes, 20);
  header.header_crc = get_u32(bytes, 24);

  if (header.magic != kRecordMagic) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "record magic mismatch";
    return outcome;
  }
  if (header.version != kRecordFormatVersion) {
    outcome.status = FrameStatus::UnsupportedVersion;
    outcome.detail = "record format version is not supported";
    return outcome;
  }
  if (!is_valid_record_type(header.type)) {
    outcome.status = FrameStatus::InvalidType;
    outcome.detail = "record type outside the defined domain";
    return outcome;
  }
  if (header.payload_length > kMaxRecordPayloadBytes) {
    outcome.status = FrameStatus::Oversized;
    outcome.detail = "declared payload length exceeds the record bound";
    return outcome;
  }
  const std::uint32_t header_crc = Crc32c::compute(bytes.first(24));
  if (header_crc != header.header_crc) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "record header integrity check failed";
    return outcome;
  }

  const std::size_t total = kRecordHeaderBytes + static_cast<std::size_t>(header.payload_length);
  if (bytes.size() < total) {
    outcome.status = FrameStatus::TornTail;
    outcome.detail = "declared payload is incomplete";
    return outcome;
  }

  const std::span<const std::byte> payload = bytes.subspan(kRecordHeaderBytes, header.payload_length);
  if (Crc32c::compute(payload) != header.payload_crc) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "record payload integrity check failed";
    return outcome;
  }

  outcome.status = FrameStatus::Ok;
  outcome.frame.type = static_cast<RecordType>(header.type);
  outcome.frame.sequence = Sequence{header.sequence};
  outcome.frame.payload.assign(payload.begin(), payload.end());
  outcome.frame.encoded_size = total;
  outcome.consumed = total;
  return outcome;
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Status{StatusCode::IoError, "cannot stat durable file"};
  }
  if (size > max_bytes) {
    return Status{StatusCode::LimitExceeded, "durable file exceeds the configured bound"};
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Status{StatusCode::IoError, "cannot open durable file for reading"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  const std::size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
  const bool short_read = read != bytes.size();
  std::fclose(file);
  if (short_read) {
    return Status{StatusCode::IoError, "short read from durable file"};
  }
  return bytes;
}

VoidResult write_file_atomic(const std::filesystem::path& target, std::span<const std::byte> bytes,
                             bool durable) {
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  {
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
      return Status{StatusCode::IoError, "cannot open temporary file for writing"};
    }
    if (!bytes.empty()) {
      const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
      if (written != bytes.size()) {
        std::fclose(file);
        (void)remove_file_if_present(temporary);
        return Status{StatusCode::IoError, "short write to temporary file"};
      }
    }
    if (std::fflush(file) != 0) {
      std::fclose(file);
      (void)remove_file_if_present(temporary);
      return Status{StatusCode::IoError, "flush failed"};
    }
    if (durable) {
#ifdef _WIN32
      if (_commit(_fileno(file)) != 0) {
        std::fclose(file);
        (void)remove_file_if_present(temporary);
        return Status{StatusCode::IoError, "durable flush failed"};
      }
#else
      if (::fsync(::fileno(file)) != 0) {
        std::fclose(file);
        (void)remove_file_if_present(temporary);
        return Status{StatusCode::IoError, "durable flush failed"};
      }
#endif
    }
    std::fclose(file);
  }

#ifdef _WIN32
  if (::MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    (void)remove_file_if_present(temporary);
    return Status{StatusCode::IoError, "atomic replace failed"};
  }
#else
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    (void)remove_file_if_present(temporary);
    return Status{StatusCode::IoError, "atomic replace failed"};
  }
#endif
  return ok_result();
}

VoidResult append_file_durable(const std::filesystem::path& path, std::span<const std::byte> bytes,
                               bool durable) {
  std::FILE* file = std::fopen(path.string().c_str(), "ab");
  if (file == nullptr) {
    return Status{StatusCode::IoError, "cannot open file for append"};
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      return Status{StatusCode::IoError, "short append"};
    }
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return Status{StatusCode::IoError, "append flush failed"};
  }
  if (durable) {
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
      std::fclose(file);
      return Status{StatusCode::IoError, "durable append flush failed"};
    }
#else
    if (::fsync(::fileno(file)) != 0) {
      std::fclose(file);
      return Status{StatusCode::IoError, "durable append flush failed"};
    }
#endif
  }
  std::fclose(file);
  return ok_result();
}

VoidResult sync_file(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Status{StatusCode::IoError, "cannot open file for sync"};
  }
#ifdef _WIN32
  const int result = _commit(_fileno(file));
#else
  const int result = ::fsync(::fileno(file));
#endif
  std::fclose(file);
  if (result != 0) {
    return Status{StatusCode::IoError, "sync failed"};
  }
  return ok_result();
}

VoidResult remove_file_if_present(const std::filesystem::path& path) {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error && !removed) {
    return Status{StatusCode::IoError, "cannot remove file"};
  }
  return ok_result();
}

}  // namespace fcfn::store
