// FCFN - bounded framed wire protocol.
//
// Frame layout (44-byte header, then the payload):
//   0  magic            u32  "FCFN"
//   4  version          u16  wire format version
//   6  type             u16  message type
//   8  payload_length   u32  declared payload bytes
//  12  session_id       u64  established session identity
//  20  sequence         u64  monotone per session
//  28  epoch            u64  coordinator epoch the client believes is current
//  36  payload_crc      u32  CRC-32C over the payload
//  40  header_crc       u32  CRC-32C over bytes [0, 40)
//
// Rules enforced by the codec:
//   * a declared payload length above the bound is refused BEFORE allocation
//   * decoding is total and sticky: the first failure poisons the decoder
//   * enum and domain values are validated
//   * every request is bound to the established session identity
//
// Transport security is intentionally out of scope: the session token is an
// opaque bearer label, the channel is not encrypted, and FCFN makes no
// cryptographic authentication claim. See the README trust boundary section.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_NET_FRAME_HPP
#define FCFN_NET_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::net {

inline constexpr std::uint32_t kWireMagic = 0x4e434646u;  // "FCFN" little-endian
inline constexpr std::uint16_t kWireVersion = 1;
inline constexpr std::size_t kWireHeaderBytes = 44;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Request = 3,
  Response = 4,
  Goodbye = 5,
};

[[nodiscard]] const char* to_string(MessageType value) noexcept;
[[nodiscard]] bool parse_message_type(std::string_view token, MessageType& out) noexcept;
[[nodiscard]] bool is_valid_message_type(std::uint16_t raw) noexcept;

/// Operations a session may request. Numeric values are contract.
enum class Operation : std::uint16_t {
  Describe = 1,
  ApplyTopology = 2,
  ApplyPolicy = 3,
  RecordDetection = 4,
  Authorize = 5,
  Plan = 6,
  Transition = 7,
  SubmitApply = 8,
  Acknowledge = 9,
  VerifyEffect = 10,
  CurrentBoundary = 11,
  PlanByGeneration = 12,
  Attempts = 13,
  Evidence = 14,
  Checkpoint = 15,
  Shutdown = 16,
};

[[nodiscard]] const char* to_string(Operation value) noexcept;
[[nodiscard]] bool parse_operation(std::string_view token, Operation& out) noexcept;
[[nodiscard]] bool is_valid_operation(std::uint16_t raw) noexcept;
/// True when the operation mutates durable state (used by crash-point injection).
[[nodiscard]] bool is_mutating_operation(Operation value) noexcept;

struct Frame {
  MessageType type{MessageType::Request};
  SessionId session{};
  Sequence sequence{};
  CoordinatorEpoch epoch{};
  std::vector<std::byte> payload{};
};

enum class FrameStatus : std::uint8_t {
  Ok = 0,
  /// Fewer bytes than a header: keep reading.
  NeedMore = 1,
  /// A complete header and payload were decoded.
  Complete = 2,
  Corrupt = 3,
  UnsupportedVersion = 4,
  InvalidType = 5,
  Oversized = 6,
  TrailingGarbage = 7,
  SequenceRegression = 8,
  SessionMismatch = 9,
};

[[nodiscard]] const char* to_string(FrameStatus value) noexcept;

struct DecodeOutcome {
  FrameStatus status{FrameStatus::NeedMore};
  Frame frame{};
  std::string detail{};
};

/// Encode one frame.
[[nodiscard]] std::vector<std::byte> encode_frame(const Frame& frame);

/// Decode one frame from the front of the buffer. Never allocates more than the
/// bound allows, and reports how many bytes were consumed.
[[nodiscard]] DecodeOutcome decode_frame(std::span<const std::byte> bytes);

/// Sticky stream decoder: once a failure is observed every later call returns it.
class FrameStream {
 public:
  /// Feed bytes into the stream buffer. Returns false when the buffer would
  /// exceed the configured bound (the stream is then poisoned).
  bool feed(std::span<const std::byte> bytes);

  /// Extract the next complete frame. NeedMore means "feed more bytes".
  DecodeOutcome next();

  [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::byte> buffer_{};
  std::size_t offset_{0};
  bool poisoned_{false};
  Status status_{};
};

/// Control payloads.
struct HelloPayload {
  std::string token{};
  std::string label{};
  CoordinatorEpoch requested_epoch{};
};

struct HelloAckPayload {
  SessionId session{};
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  bool accepted{false};
  std::string reason{};
  std::string version{};
};

struct RequestPayload {
  Operation operation{Operation::Describe};
  std::vector<std::byte> arguments{};
};

struct ResponsePayload {
  Operation operation{Operation::Describe};
  StatusCode code{StatusCode::Internal};
  std::string message{};
  std::string json{};
  std::vector<std::byte> result{};
};

[[nodiscard]] std::vector<std::byte> encode_hello(const HelloPayload& hello);
[[nodiscard]] Result<HelloPayload> decode_hello(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_hello_ack(const HelloAckPayload& ack);
[[nodiscard]] Result<HelloAckPayload> decode_hello_ack(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_request(const RequestPayload& request);
[[nodiscard]] Result<RequestPayload> decode_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_response(const ResponsePayload& response);
[[nodiscard]] Result<ResponsePayload> decode_response(std::span<const std::byte> bytes);

}  // namespace fcfn::net

#endif  // FCFN_NET_FRAME_HPP
