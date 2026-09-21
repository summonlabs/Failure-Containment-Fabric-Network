// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/core/json.hpp"

#include <cstdio>

namespace fcfn {
namespace {

void append_unsigned(std::string& out, std::uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  out.append(buffer);
}

void append_signed(std::string& out, std::int64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  out.append(buffer);
}

}  // namespace

void json_escape_append(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      default:
        if (uc < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(uc));
          out.append(buffer);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  json_escape_append(out, text);
  return out;
}

void JsonWriter::newline_indent() {
  if (!pretty_) {
    return;
  }
  out_.push_back('\n');
  for (int i = 0; i < depth_; ++i) {
    out_.append("  ");
  }
}

void JsonWriter::before_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (need_comma_) {
    out_.push_back(',');
  }
  newline_indent();
  need_comma_ = true;
}

void JsonWriter::begin_object() {
  before_value();
  out_.push_back('{');
  ++depth_;
  need_comma_ = false;
}

void JsonWriter::end_object() {
  --depth_;
  const bool had_content = need_comma_;
  need_comma_ = true;
  if (had_content) {
    newline_indent();
  }
  out_.push_back('}');
}

void JsonWriter::begin_array() {
  before_value();
  out_.push_back('[');
  ++depth_;
  need_comma_ = false;
}

void JsonWriter::end_array() {
  --depth_;
  const bool had_content = need_comma_;
  need_comma_ = true;
  if (had_content) {
    newline_indent();
  }
  out_.push_back(']');
}

void JsonWriter::key(std::string_view name) {
  if (need_comma_) {
    out_.push_back(',');
  }
  newline_indent();
  need_comma_ = true;
  json_escape_append(out_, name);
  out_.push_back(':');
  if (pretty_) {
    out_.push_back(' ');
  }
  after_key_ = true;
}

void JsonWriter::value(std::string_view text) {
  before_value();
  json_escape_append(out_, text);
}

void JsonWriter::value(std::uint64_t number) {
  before_value();
  append_unsigned(out_, number);
}

void JsonWriter::value(std::int64_t number) {
  before_value();
  append_signed(out_, number);
}

void JsonWriter::value(bool flag) {
  before_value();
  out_.append(flag ? "true" : "false");
}

void JsonWriter::null_value() {
  before_value();
  out_.append("null");
}

}  // namespace fcfn
