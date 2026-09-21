// FCFN - deterministic JSON rendering for tools and diagnostics.
//
// Rendering is canonical: object keys are emitted in the order the caller
// writes them, arrays in element order, integers exactly, and no floating point
// is ever produced. This keeps CLI output and golden tests byte-stable.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_JSON_HPP
#define FCFN_CORE_JSON_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "fcfn/core/ids.hpp"

namespace fcfn {

/// Append a JSON-escaped string (including surrounding quotes).
void json_escape_append(std::string& out, std::string_view text);

[[nodiscard]] std::string json_escape(std::string_view text);

/// Minimal streaming writer that guarantees valid, deterministic JSON.
class JsonWriter {
 public:
  explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  void key(std::string_view name);
  void value(std::string_view text);
  void value(const char* text) { value(std::string_view(text)); }
  void value(const std::string& text) { value(std::string_view(text)); }
  void value(std::uint64_t number);
  void value(std::uint32_t number) { value(static_cast<std::uint64_t>(number)); }
  void value(std::uint16_t number) { value(static_cast<std::uint64_t>(number)); }
  void value(std::uint8_t number) { value(static_cast<std::uint64_t>(number)); }
  void value(std::int64_t number);
  void value(int number) { value(static_cast<std::int64_t>(number)); }
  void value(bool flag);
  void value(const Digest& digest) { value(digest.hex()); }
  void null_value();

  void member(std::string_view name, std::string_view text) {
    key(name);
    value(text);
  }
  /// Explicit overload: without it, a const char* argument would convert to bool.
  void member(std::string_view name, const char* text) {
    key(name);
    value(std::string_view(text));
  }
  void member(std::string_view name, std::uint64_t number) {
    key(name);
    value(number);
  }
  void member(std::string_view name, bool flag) {
    key(name);
    value(flag);
  }

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  void before_value();
  void newline_indent();

  std::string out_;
  bool pretty_{false};
  int depth_{0};
  bool need_comma_{false};
  bool after_key_{false};
};

}  // namespace fcfn

#endif  // FCFN_CORE_JSON_HPP
