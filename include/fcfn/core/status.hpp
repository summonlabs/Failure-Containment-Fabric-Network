// FCFN - explicit outcome classification.
//
// Every externally visible operation returns a StatusCode. The vocabulary is
// deliberately explicit: unknown, stale, conflicting, invalid, unsupported,
// fenced and ambiguous conditions are first-class results and are never folded
// into success or into ordinary absence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_STATUS_HPP
#define FCFN_CORE_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace fcfn {

/// Classification of every FCFN operation outcome.
enum class StatusCode : std::uint8_t {
  Ok = 0,
  /// Input is malformed, out of domain, or violates a stated precondition.
  InvalidArgument,
  /// The named entity does not exist in the addressed scope.
  NotFound,
  /// The named entity already exists and may not be created again.
  AlreadyExists,
  /// Evidence or state is explicitly unknown. Never treated as affirmative.
  Unknown,
  /// A generation/epoch/boot binding no longer matches current authority.
  StaleGeneration,
  /// Contradictory evidence, authority vectors, or effect reports.
  Conflict,
  /// Recognised request that this build does not implement.
  Unsupported,
  /// A configured structural bound was exceeded before materialisation.
  LimitExceeded,
  /// A bounded resource budget was exhausted (deterministic refusal).
  Exhausted,
  /// Policy or authority explicitly refused the operation.
  Denied,
  /// No usable authority was presented, or it belongs to another session.
  Unauthorized,
  /// Superseded by a later process incarnation or coordinator epoch.
  Fenced,
  /// Integrity check failed; the artifact may be corrupt or tampered with.
  Corrupt,
  /// Durable/wire format version is not supported by this build.
  VersionUnsupported,
  /// A monotonic sequence, attempt, or epoch moved backwards.
  SequenceRegression,
  /// Bytes exist after the end of a well-formed unit.
  TrailingGarbage,
  /// Wire or file framing rule violated.
  ProtocolViolation,
  /// Underlying storage or socket operation failed.
  IoError,
  /// Shutdown or cancellation released the operation before completion.
  Interrupted,
  /// Operation may or may not have taken effect; effect state is not provable.
  Ambiguous,
  /// Invariant violation inside this implementation.
  Internal,
};

/// Stable lowercase token for a status code (used in canonical output and tests).
[[nodiscard]] const char* to_string(StatusCode code) noexcept;

/// Parse the token produced by to_string. Returns false when unrecognised.
[[nodiscard]] bool parse_status_code(std::string_view token, StatusCode& out) noexcept;

/// True for codes that represent a refusal/indeterminacy rather than success.
[[nodiscard]] constexpr bool is_failure(StatusCode code) noexcept {
  return code != StatusCode::Ok;
}

/// True when the code expresses indeterminacy that must be surfaced, not hidden.
[[nodiscard]] constexpr bool is_indeterminate(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Unknown:
    case StatusCode::StaleGeneration:
    case StatusCode::Conflict:
    case StatusCode::Ambiguous:
    case StatusCode::Fenced:
    case StatusCode::Interrupted:
    case StatusCode::Exhausted:
      return true;
    default:
      return false;
  }
}

/// Maximum number of bytes retained in a status message (bounded, never grows).
inline constexpr std::size_t kMaxStatusMessageLength = 192;

/// A status code plus a bounded human-readable detail string.
class Status {
 public:
  Status() noexcept = default;
  explicit Status(StatusCode code, std::string_view message = {}) noexcept;

  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

inline const Status kOkStatus{};

}  // namespace fcfn

#endif  // FCFN_CORE_STATUS_HPP
