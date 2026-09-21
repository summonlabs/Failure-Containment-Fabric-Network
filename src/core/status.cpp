// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/core/status.hpp"

#include <algorithm>

namespace fcfn {
namespace {

struct CodeToken {
  StatusCode code;
  const char* token;
};

constexpr CodeToken kTokens[] = {
    {StatusCode::Ok, "ok"},
    {StatusCode::InvalidArgument, "invalid_argument"},
    {StatusCode::NotFound, "not_found"},
    {StatusCode::AlreadyExists, "already_exists"},
    {StatusCode::Unknown, "unknown"},
    {StatusCode::StaleGeneration, "stale_generation"},
    {StatusCode::Conflict, "conflict"},
    {StatusCode::Unsupported, "unsupported"},
    {StatusCode::LimitExceeded, "limit_exceeded"},
    {StatusCode::Exhausted, "exhausted"},
    {StatusCode::Denied, "denied"},
    {StatusCode::Unauthorized, "unauthorized"},
    {StatusCode::Fenced, "fenced"},
    {StatusCode::Corrupt, "corrupt"},
    {StatusCode::VersionUnsupported, "version_unsupported"},
    {StatusCode::SequenceRegression, "sequence_regression"},
    {StatusCode::TrailingGarbage, "trailing_garbage"},
    {StatusCode::ProtocolViolation, "protocol_violation"},
    {StatusCode::IoError, "io_error"},
    {StatusCode::Interrupted, "interrupted"},
    {StatusCode::Ambiguous, "ambiguous"},
    {StatusCode::Internal, "internal"},
};

}  // namespace

const char* to_string(StatusCode code) noexcept {
  for (const CodeToken& entry : kTokens) {
    if (entry.code == code) {
      return entry.token;
    }
  }
  return "unknown_status_code";
}

bool parse_status_code(std::string_view token, StatusCode& out) noexcept {
  for (const CodeToken& entry : kTokens) {
    if (token == entry.token) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

Status::Status(StatusCode code, std::string_view message) noexcept : code_(code) {
  const std::size_t limit = std::min(message.size(), kMaxStatusMessageLength);
  message_.assign(message.substr(0, limit));
}

std::string Status::to_string() const {
  std::string out = fcfn::to_string(code_);
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace fcfn
