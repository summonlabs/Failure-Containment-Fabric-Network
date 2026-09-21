// FCFN codec suite: durable record framing integrity.
//
// Product proposition proved here: every field of a durable record frame is
// independently enforced. Magic, format version, record type, declared payload
// length, header integrity, and payload integrity each produce their own
// classified refusal, an out-of-bound declared length is refused before
// allocation, and the only recoverable damage class is a strict prefix at the
// end of a stream.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "allocation_probe.hpp"
#include "fcfn/core/hash.hpp"
#include "fcfn/store/record.hpp"
#include "test_harness.hpp"

namespace {

using namespace fcfn::store;
using fcfn::Crc32c;
using fcfn::kMaxRecordPayloadBytes;
using fcfn::Sequence;

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kLengthOffset = 8;
constexpr std::size_t kSequenceOffset = 12;
constexpr std::size_t kPayloadCrcOffset = 20;
constexpr std::size_t kHeaderCrcOffset = 24;

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

std::vector<std::byte> sample_payload() {
  std::vector<std::byte> payload;
  payload.reserve(37);
  for (std::uint32_t index = 0; index < 37; ++index) {
    payload.push_back(static_cast<std::byte>((index * 7u + 3u) & 0xffu));
  }
  return payload;
}

/// Hand-built frame so every header field can be controlled independently of
/// the encoder.
std::vector<std::byte> make_frame(std::uint32_t magic, std::uint16_t version, std::uint16_t type,
                                  std::uint32_t declared_length, std::uint64_t sequence,
                                  std::uint32_t payload_crc, std::uint32_t header_crc,
                                  std::span<const std::byte> payload, bool fix_header_crc) {
  std::vector<std::byte> bytes;
  bytes.reserve(kRecordHeaderBytes + payload.size());
  put_u32(bytes, magic);
  put_u16(bytes, version);
  put_u16(bytes, type);
  put_u32(bytes, declared_length);
  put_u64(bytes, sequence);
  put_u32(bytes, payload_crc);
  if (fix_header_crc) {
    header_crc = Crc32c::compute(std::span<const std::byte>(bytes.data(), bytes.size()));
  }
  put_u32(bytes, header_crc);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

void expect_frame_status(const char* what, const DecodeOutcome& outcome, FrameStatus expected) {
  if (outcome.status != expected) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what + "': expected " + to_string(expected) +
                           " but got " + to_string(outcome.status) + " (" + outcome.detail + ")");
  }
}

const std::vector<std::uint16_t>& record_type_values() {
  static const std::vector<std::uint16_t> values{1,  2,  3,  4,  5,  6,  7,  8,
                                                 9, 10, 11, 12, 13, 14, 15, 16};
  return values;
}

}  // namespace

FCFN_TEST(frame, every_record_type_round_trips) {
  const std::vector<std::byte> payload = sample_payload();
  for (const std::uint16_t raw : record_type_values()) {
    FCFN_CHECK(is_valid_record_type(raw));
    const auto type = static_cast<RecordType>(raw);
    RecordType parsed = RecordType::StartupMarker;
    FCFN_CHECK(parse_record_type(to_string(type), parsed));
    FCFN_CHECK(parsed == type);
    for (const std::uint64_t sequence : {std::uint64_t{0}, std::uint64_t{1},
                                         std::uint64_t{0xffffffffffffffffull}}) {
      const std::vector<std::byte> bytes = encode_record(type, Sequence{sequence}, payload);
      FCFN_CHECK_EQ(bytes.size(), kRecordHeaderBytes + payload.size());
      const DecodeOutcome outcome = decode_record(bytes);
      expect_frame_status(to_string(type), outcome, FrameStatus::Ok);
      FCFN_CHECK(outcome.frame.type == type);
      FCFN_CHECK_EQ(outcome.frame.sequence.value(), sequence);
      FCFN_CHECK_EQ(outcome.frame.payload.size(), payload.size());
      FCFN_CHECK(std::equal(outcome.frame.payload.begin(), outcome.frame.payload.end(),
                            payload.begin()));
      FCFN_CHECK_EQ(outcome.frame.encoded_size, bytes.size());
      FCFN_CHECK_EQ(outcome.consumed, bytes.size());
    }
  }
  FCFN_CHECK(!is_valid_record_type(0));
  FCFN_CHECK(!is_valid_record_type(17));
  FCFN_CHECK(!is_valid_record_type(999));
}

FCFN_TEST(frame, magic_version_type_and_length_are_each_independently_enforced) {
  const std::vector<std::byte> payload = sample_payload();
  const std::uint32_t payload_crc = Crc32c::compute(payload);

  // Magic is checked first: a wrong magic is corruption, never a torn tail.
  {
    const std::vector<std::byte> bytes = make_frame(0xdeadbeefu, kRecordFormatVersion,
                                                    static_cast<std::uint16_t>(RecordType::Fence),
                                                    static_cast<std::uint32_t>(payload.size()), 1,
                                                    payload_crc, 0, payload, true);
    expect_frame_status("magic mismatch", decode_record(bytes), FrameStatus::Corrupt);
  }
  // An unsupported version is reported as such with and without a consistent
  // header CRC, so the classification cannot depend on integrity ordering.
  for (const bool fix : {false, true}) {
    const std::vector<std::byte> bytes =
        make_frame(kRecordMagic, kRecordFormatVersion + 1,
                   static_cast<std::uint16_t>(RecordType::Fence),
                   static_cast<std::uint32_t>(payload.size()), 1, payload_crc, 0, payload, fix);
    expect_frame_status("unsupported version", decode_record(bytes), FrameStatus::UnsupportedVersion);
  }
  // An undefined record type is reported as such, never coerced to a known one.
  for (const std::uint16_t type : {std::uint16_t{0}, std::uint16_t{17}, std::uint16_t{999}}) {
    for (const bool fix : {false, true}) {
      const std::vector<std::byte> bytes =
          make_frame(kRecordMagic, kRecordFormatVersion, type,
                     static_cast<std::uint32_t>(payload.size()), 1, payload_crc, 0, payload, fix);
      expect_frame_status("invalid type", decode_record(bytes), FrameStatus::InvalidType);
    }
  }
  // A declared length above the bound is refused before anything is read or
  // allocated, again independent of the header CRC.
  for (const std::uint32_t declared : {kMaxRecordPayloadBytes + 1u, 0xffffffffu}) {
    for (const bool fix : {false, true}) {
      const std::vector<std::byte> bytes =
          make_frame(kRecordMagic, kRecordFormatVersion,
                     static_cast<std::uint16_t>(RecordType::Fence), declared, 1, payload_crc, 0,
                     payload, fix);
      expect_frame_status("declared length above bound", decode_record(bytes), FrameStatus::Oversized);
    }
  }
  // A declared length at the bound with an incomplete buffer is a torn tail: it
  // is never materialised, and it is never misreported as corruption.
  {
    const std::vector<std::byte> bytes =
        make_frame(kRecordMagic, kRecordFormatVersion,
                   static_cast<std::uint16_t>(RecordType::Fence), kMaxRecordPayloadBytes, 1,
                   payload_crc, 0, payload, true);
    expect_frame_status("declared length at bound, short buffer", decode_record(bytes),
                        FrameStatus::TornTail);
  }
}

FCFN_TEST(frame, header_and_payload_integrity_are_each_independently_enforced) {
  const std::vector<std::byte> payload = sample_payload();
  const std::vector<std::byte> valid =
      encode_record(RecordType::PlanDecision, Sequence{9}, payload);

  const auto flip = [&valid](std::size_t offset) {
    std::vector<std::byte> bytes = valid;
    bytes[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes[offset]) ^ 0x40u);
    return bytes;
  };

  // Any header field covered by the header CRC.
  expect_frame_status("sequence field corrupted", decode_record(flip(kSequenceOffset)),
                      FrameStatus::Corrupt);
  expect_frame_status("payload crc field corrupted", decode_record(flip(kPayloadCrcOffset)),
                      FrameStatus::Corrupt);
  expect_frame_status("header crc field corrupted", decode_record(flip(kHeaderCrcOffset)),
                      FrameStatus::Corrupt);
  expect_frame_status("payload byte corrupted", decode_record(flip(kRecordHeaderBytes + 3)),
                      FrameStatus::Corrupt);

  // Recomputing the header CRC does not launder a wrong payload CRC: the payload
  // integrity check stands on its own.
  {
    std::vector<std::byte> bytes = valid;
    const std::uint32_t wrong = Crc32c::compute(payload) ^ 0x00000001u;
    for (unsigned index = 0; index < 4; ++index) {
      bytes[kPayloadCrcOffset + index] = static_cast<std::byte>((wrong >> (8u * index)) & 0xffu);
    }
    const std::uint32_t fixed =
        Crc32c::compute(std::span<const std::byte>(bytes.data(), kHeaderCrcOffset));
    for (unsigned index = 0; index < 4; ++index) {
      bytes[kHeaderCrcOffset + index] = static_cast<std::byte>((fixed >> (8u * index)) & 0xffu);
    }
    expect_frame_status("payload crc wrong with a consistent header crc", decode_record(bytes),
                        FrameStatus::Corrupt);
  }
}

FCFN_TEST(frame, every_truncated_prefix_is_a_torn_tail_and_never_corruption) {
  const std::vector<std::byte> payload = sample_payload();
  const std::vector<std::byte> valid =
      encode_record(RecordType::ApplyAttempt, Sequence{4}, payload);
  const std::size_t total = valid.size();

  expect_frame_status("empty input", decode_record(std::span<const std::byte>{}),
                      FrameStatus::NoMoreData);
  expect_frame_status("complete frame", decode_record(valid), FrameStatus::Ok);

  for (std::size_t length = 1; length < total; ++length) {
    const DecodeOutcome outcome =
        decode_record(std::span<const std::byte>(valid.data(), length));
    if (outcome.status != FrameStatus::TornTail) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         "prefix of " + std::to_string(length) + " of " + std::to_string(total) +
                             " bytes reported " + to_string(outcome.status) + ": " + outcome.detail);
    }
    // A refused frame carries no partially built content.
    if (outcome.consumed != 0 || !outcome.frame.payload.empty() ||
        outcome.frame.encoded_size != 0) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         "torn tail of " + std::to_string(length) +
                             " bytes materialised partial frame content");
    }
  }

  // The recoverable class is exactly this one, plus the clean end of stream.
  FCFN_CHECK(is_recoverable_tail(FrameStatus::TornTail));
  FCFN_CHECK(is_recoverable_tail(FrameStatus::NoMoreData));
  FCFN_CHECK(!is_recoverable_tail(FrameStatus::Corrupt));
  FCFN_CHECK(!is_recoverable_tail(FrameStatus::Oversized));
  FCFN_CHECK(!is_recoverable_tail(FrameStatus::SequenceRegression));
  FCFN_CHECK(!is_recoverable_tail(FrameStatus::TrailingGarbage));
}

FCFN_TEST(frame, an_out_of_bound_declared_length_is_refused_before_allocating) {
  const std::vector<std::byte> payload = sample_payload();
  const std::uint32_t payload_crc = Crc32c::compute(payload);

  // Control: the probe really does observe allocations.
  {
    const fcfn::test::AllocationWindow window;
    std::vector<std::byte> control(1u << 20, std::byte{0x5a});
    FCFN_CHECK(window.bytes() >= (1u << 20));
    FCFN_CHECK(control.size() == (1u << 20));
  }

  {
    const std::vector<std::byte> bytes =
        make_frame(kRecordMagic, kRecordFormatVersion,
                   static_cast<std::uint16_t>(RecordType::Fence), kMaxRecordPayloadBytes, 1,
                   payload_crc, 0, payload, true);
    const fcfn::test::AllocationWindow window;
    const DecodeOutcome outcome = decode_record(bytes);
    FCFN_CHECK(outcome.status == FrameStatus::TornTail);
    FCFN_CHECK(window.bytes() < 4096);
  }
  {
    const std::vector<std::byte> bytes =
        make_frame(kRecordMagic, kRecordFormatVersion,
                   static_cast<std::uint16_t>(RecordType::Fence), 0xffffffffu, 1, payload_crc, 0,
                   payload, true);
    const fcfn::test::AllocationWindow window;
    const DecodeOutcome outcome = decode_record(bytes);
    FCFN_CHECK(outcome.status == FrameStatus::Oversized);
    FCFN_CHECK(window.bytes() < 4096);
  }
}
