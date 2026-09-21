// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/net/frame.hpp"

#include <algorithm>
#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/hash.hpp"

namespace fcfn::net {
namespace {

struct TypeToken {
  MessageType value;
  const char* token;
};

constexpr TypeToken kTypeTokens[] = {
    {MessageType::Hello, "hello"},
    {MessageType::HelloAck, "hello_ack"},
    {MessageType::Request, "request"},
    {MessageType::Response, "response"},
    {MessageType::Goodbye, "goodbye"},
};

struct OperationToken {
  Operation value;
  const char* token;
  bool mutating;
};

constexpr OperationToken kOperationTokens[] = {
    {Operation::Describe, "describe", false},
    {Operation::ApplyTopology, "apply_topology", true},
    {Operation::ApplyPolicy, "apply_policy", true},
    {Operation::RecordDetection, "record_detection", true},
    {Operation::Authorize, "authorize", true},
    {Operation::Plan, "plan", true},
    {Operation::Transition, "transition", true},
    {Operation::SubmitApply, "submit_apply", true},
    {Operation::Acknowledge, "acknowledge", true},
    {Operation::VerifyEffect, "verify_effect", true},
    {Operation::CurrentBoundary, "current_boundary", false},
    {Operation::PlanByGeneration, "plan_by_generation", false},
    {Operation::Attempts, "attempts", false},
    {Operation::Evidence, "evidence", false},
    {Operation::Checkpoint, "checkpoint", true},
    {Operation::Shutdown, "shutdown", true},
};

struct StatusToken {
  FrameStatus value;
  const char* token;
};

constexpr StatusToken kStatusTokens[] = {
    {FrameStatus::Ok, "ok"},
    {FrameStatus::NeedMore, "need_more"},
    {FrameStatus::Complete, "complete"},
    {FrameStatus::Corrupt, "corrupt"},
    {FrameStatus::UnsupportedVersion, "unsupported_version"},
    {FrameStatus::InvalidType, "invalid_type"},
    {FrameStatus::Oversized, "oversized"},
    {FrameStatus::TrailingGarbage, "trailing_garbage"},
    {FrameStatus::SequenceRegression, "sequence_regression"},
    {FrameStatus::SessionMismatch, "session_mismatch"},
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

/// A frame may carry at most one maximum-size payload.
constexpr std::size_t kMaxBufferedBytes = static_cast<std::size_t>(kMaxFramePayloadBytes) + kWireHeaderBytes;

}  // namespace

const char* to_string(MessageType value) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_message_type";
}

bool parse_message_type(std::string_view token, MessageType& out) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool is_valid_message_type(std::uint16_t raw) noexcept {
  for (const TypeToken& entry : kTypeTokens) {
    if (static_cast<std::uint16_t>(entry.value) == raw) {
      return true;
    }
  }
  return false;
}

const char* to_string(Operation value) noexcept {
  for (const OperationToken& entry : kOperationTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_operation";
}

bool parse_operation(std::string_view token, Operation& out) noexcept {
  for (const OperationToken& entry : kOperationTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool is_valid_operation(std::uint16_t raw) noexcept {
  for (const OperationToken& entry : kOperationTokens) {
    if (static_cast<std::uint16_t>(entry.value) == raw) {
      return true;
    }
  }
  return false;
}

bool is_mutating_operation(Operation value) noexcept {
  for (const OperationToken& entry : kOperationTokens) {
    if (entry.value == value) {
      return entry.mutating;
    }
  }
  return true;
}

const char* to_string(FrameStatus value) noexcept {
  for (const StatusToken& entry : kStatusTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_frame_status";
}

std::vector<std::byte> encode_frame(const Frame& frame) {
  std::vector<std::byte> out;
  out.reserve(kWireHeaderBytes + frame.payload.size());
  put_u32(out, kWireMagic);
  put_u16(out, kWireVersion);
  put_u16(out, static_cast<std::uint16_t>(frame.type));
  put_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
  put_u64(out, frame.session.value());
  put_u64(out, frame.sequence.value());
  put_u64(out, frame.epoch.value());
  put_u32(out, Crc32c::compute(frame.payload));
  put_u32(out, Crc32c::compute(std::span<const std::byte>(out.data(), out.size())));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

DecodeOutcome decode_frame(std::span<const std::byte> bytes) {
  DecodeOutcome outcome;
  if (bytes.size() < kWireHeaderBytes) {
    outcome.status = FrameStatus::NeedMore;
    return outcome;
  }
  if (get_u32(bytes, 0) != kWireMagic) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "wire magic mismatch";
    return outcome;
  }
  if (get_u16(bytes, 4) != kWireVersion) {
    outcome.status = FrameStatus::UnsupportedVersion;
    outcome.detail = "wire format version is not supported";
    return outcome;
  }
  const std::uint16_t type = get_u16(bytes, 6);
  if (!is_valid_message_type(type)) {
    outcome.status = FrameStatus::InvalidType;
    outcome.detail = "message type outside the defined domain";
    return outcome;
  }
  const std::uint32_t payload_length = get_u32(bytes, 8);
  if (payload_length > kMaxFramePayloadBytes) {
    // Refused before any allocation is attempted.
    outcome.status = FrameStatus::Oversized;
    outcome.detail = "declared payload length exceeds the wire bound";
    return outcome;
  }
  const std::uint32_t header_crc = get_u32(bytes, 40);
  if (Crc32c::compute(bytes.first(40)) != header_crc) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "frame header integrity check failed";
    return outcome;
  }
  const std::size_t total = kWireHeaderBytes + static_cast<std::size_t>(payload_length);
  if (bytes.size() < total) {
    outcome.status = FrameStatus::NeedMore;
    return outcome;
  }
  const std::span<const std::byte> payload = bytes.subspan(kWireHeaderBytes, payload_length);
  if (Crc32c::compute(payload) != get_u32(bytes, 36)) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = "frame payload integrity check failed";
    return outcome;
  }
  outcome.status = FrameStatus::Complete;
  outcome.frame.type = static_cast<MessageType>(type);
  outcome.frame.session = SessionId{get_u64(bytes, 12)};
  outcome.frame.sequence = Sequence{get_u64(bytes, 20)};
  outcome.frame.epoch = CoordinatorEpoch{get_u64(bytes, 28)};
  outcome.frame.payload.assign(payload.begin(), payload.end());
  return outcome;
}

bool FrameStream::feed(std::span<const std::byte> bytes) {
  if (poisoned_) {
    return false;
  }
  if (offset_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  if (buffer_.size() + bytes.size() > kMaxBufferedBytes) {
    poisoned_ = true;
    status_ = Status{StatusCode::LimitExceeded, "frame stream buffer bound exceeded"};
    return false;
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  return true;
}

DecodeOutcome FrameStream::next() {
  DecodeOutcome outcome;
  if (poisoned_) {
    outcome.status = FrameStatus::Corrupt;
    outcome.detail = status_.message();
    return outcome;
  }
  const std::span<const std::byte> remaining(buffer_.data() + offset_, buffer_.size() - offset_);
  outcome = decode_frame(remaining);
  if (outcome.status == FrameStatus::Complete) {
    offset_ += kWireHeaderBytes + outcome.frame.payload.size();
  } else if (outcome.status != FrameStatus::NeedMore) {
    poisoned_ = true;
    status_ = Status{StatusCode::ProtocolViolation, outcome.detail};
  }
  return outcome;
}

std::vector<std::byte> encode_hello(const HelloPayload& hello) {
  CanonicalWriter writer;
  writer.u32(1);
  writer.text(hello.token);
  writer.text(hello.label);
  writer.strong(hello.requested_epoch);
  return writer.data();
}

Result<HelloPayload> decode_hello(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint32_t version = reader.u32();
  HelloPayload hello;
  hello.token = reader.text(kMaxSessionTokenLength);
  hello.label = reader.text(kMaxSessionLabelLength);
  hello.requested_epoch = reader.strong<CoordinatorEpochTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (version != 1) {
    return Status{StatusCode::VersionUnsupported, "hello version is not supported"};
  }
  if (hello.token.empty()) {
    return Status{StatusCode::Unauthorized, "hello carries no session token"};
  }
  return hello;
}

std::vector<std::byte> encode_hello_ack(const HelloAckPayload& ack) {
  CanonicalWriter writer;
  writer.strong(ack.session);
  writer.strong(ack.epoch);
  writer.u64(ack.boot.id.value());
  writer.u32(ack.boot.process_id);
  writer.boolean(ack.accepted);
  writer.text(ack.reason);
  writer.text(ack.version);
  return writer.data();
}

Result<HelloAckPayload> decode_hello_ack(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  HelloAckPayload ack;
  ack.session = reader.strong<SessionIdTag>();
  ack.epoch = reader.strong<CoordinatorEpochTag>();
  ack.boot.id = BootId{reader.u64()};
  ack.boot.process_id = reader.u32();
  ack.accepted = reader.boolean();
  ack.reason = reader.text(kMaxStatusMessageLength);
  ack.version = reader.text(32);
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return ack;
}

std::vector<std::byte> encode_request(const RequestPayload& request) {
  CanonicalWriter writer;
  writer.u16(static_cast<std::uint16_t>(request.operation));
  writer.blob(request.arguments);
  return writer.data();
}

Result<RequestPayload> decode_request(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint16_t operation = reader.u16();
  const std::span<const std::byte> arguments = reader.blob(kMaxFramePayloadBytes);
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!is_valid_operation(operation)) {
    return Status{StatusCode::InvalidArgument, "operation outside the defined domain"};
  }
  RequestPayload request;
  request.operation = static_cast<Operation>(operation);
  request.arguments.assign(arguments.begin(), arguments.end());
  return request;
}

std::vector<std::byte> encode_response(const ResponsePayload& response) {
  CanonicalWriter writer;
  writer.u16(static_cast<std::uint16_t>(response.operation));
  writer.u16(static_cast<std::uint16_t>(response.code));
  writer.text(response.message);
  writer.text(response.json);
  writer.blob(response.result);
  return writer.data();
}

Result<ResponsePayload> decode_response(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint16_t operation = reader.u16();
  const std::uint16_t code = reader.u16();
  ResponsePayload response;
  response.message = reader.text(kMaxStatusMessageLength);
  response.json = reader.text(kMaxDiagnosticTextLength * 16);
  const std::span<const std::byte> result = reader.blob(kMaxFramePayloadBytes);
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!is_valid_operation(operation) || code > static_cast<std::uint16_t>(StatusCode::Internal)) {
    return Status{StatusCode::InvalidArgument, "response carries an out-of-domain value"};
  }
  response.operation = static_cast<Operation>(operation);
  response.code = static_cast<StatusCode>(code);
  response.result.assign(result.begin(), result.end());
  return response;
}

}  // namespace fcfn::net
