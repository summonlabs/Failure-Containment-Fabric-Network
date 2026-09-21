// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/core/ids.hpp"

#include <cstdio>

#include "fcfn/core/limits.hpp"

namespace fcfn {
namespace {

[[nodiscard]] bool is_allowed_char(char c) noexcept {
  const auto uc = static_cast<unsigned char>(c);
  const bool alpha = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z');
  const bool digit = uc >= '0' && uc <= '9';
  return alpha || digit || c == '.' || c == '_' || c == ':' || c == '-';
}

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

}  // namespace

bool is_valid_resource_id(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxResourceIdLength) {
    return false;
  }
  if (text.front() == '.' || text.front() == '-' || text.back() == '.' || text.back() == '-') {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (!is_allowed_char(text[i])) {
      return false;
    }
    if (text[i] == '.' && i + 1 < text.size() && text[i + 1] == '.') {
      return false;
    }
  }
  return true;
}

Result<ResourceId> ResourceId::parse(std::string_view text) {
  if (!is_valid_resource_id(text)) {
    return Status{StatusCode::InvalidArgument, "resource id is not in canonical form"};
  }
  return ResourceId{std::string{text}};
}

std::string Digest::hex() const {
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx", static_cast<unsigned long long>(hi),
                static_cast<unsigned long long>(lo));
  return std::string{buffer, 32};
}

bool Digest::parse(std::string_view text, Digest& out) noexcept {
  if (text.size() != 32) {
    return false;
  }
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    const int nibble = hex_value(text[i]);
    if (nibble < 0) {
      return false;
    }
    hi = static_cast<std::uint64_t>((hi << 4) | static_cast<std::uint64_t>(nibble));
  }
  for (std::size_t i = 16; i < 32; ++i) {
    const int nibble = hex_value(text[i]);
    if (nibble < 0) {
      return false;
    }
    lo = static_cast<std::uint64_t>((lo << 4) | static_cast<std::uint64_t>(nibble));
  }
  out = Digest{hi, lo};
  return true;
}

std::string BootIdentity::render() const {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "boot-%016llx/pid-%lu", static_cast<unsigned long long>(id.value()),
                static_cast<unsigned long>(process_id));
  return std::string{buffer};
}

}  // namespace fcfn
