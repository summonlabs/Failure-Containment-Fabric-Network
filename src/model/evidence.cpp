// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/evidence.hpp"

#include <algorithm>
#include <utility>

#include "fcfn/core/canonical.hpp"

namespace fcfn::model {
namespace {

struct StateToken {
  EvidenceState value;
  const char* token;
};

constexpr StateToken kStateTokens[] = {
    {EvidenceState::Present, "present"},
    {EvidenceState::Absent, "absent"},
    {EvidenceState::Unknown, "unknown"},
};

}  // namespace

const char* to_string(EvidenceState value) noexcept {
  for (const StateToken& entry : kStateTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_evidence_state";
}

bool parse_evidence_state(std::string_view token, EvidenceState& out) noexcept {
  for (const StateToken& entry : kStateTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

Digest DetectionRecord::compute_digest() const {
  CanonicalWriter writer;
  writer.resource_id(resource);
  writer.u8(static_cast<std::uint8_t>(state));
  writer.strong(generation);
  writer.digest(source_digest);
  writer.strong(epoch);
  writer.u64(boot.id.value());
  writer.u32(boot.process_id);
  writer.strong(sequence);
  return writer.digest();
}

Result<EvidenceVector> EvidenceVector::build(std::vector<EvidenceEntry> entries) {
  if (entries.size() > kMaxTopologyNodes) {
    return Status{StatusCode::LimitExceeded, "evidence vector exceeds bound"};
  }
  std::sort(entries.begin(), entries.end(), [](const EvidenceEntry& a, const EvidenceEntry& b) {
    return a.resource < b.resource;
  });
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].resource.empty()) {
      return Status{StatusCode::InvalidArgument, "evidence entry has empty resource id"};
    }
    if (entries[i].generation.is_zero()) {
      return Status{StatusCode::InvalidArgument, "evidence generation must be non-zero"};
    }
    if (i > 0 && entries[i - 1].resource == entries[i].resource) {
      return Status{StatusCode::AlreadyExists, "duplicate resource in evidence vector"};
    }
  }
  EvidenceVector vector;
  vector.entries_ = std::move(entries);
  vector.recompute();
  return vector;
}

void EvidenceVector::recompute() {
  CanonicalWriter writer;
  writer.u32(static_cast<std::uint32_t>(entries_.size()));
  for (const EvidenceEntry& entry : entries_) {
    writer.resource_id(entry.resource);
    writer.u8(static_cast<std::uint8_t>(entry.state));
    writer.strong(entry.generation);
    writer.digest(entry.source_digest);
  }
  digest_ = writer.digest();
}

const EvidenceEntry* EvidenceVector::find(const ResourceId& resource) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), resource,
                                   [](const EvidenceEntry& entry, const ResourceId& key) {
                                     return entry.resource < key;
                                   });
  if (it == entries_.end() || !(it->resource == resource)) {
    return nullptr;
  }
  return &*it;
}

std::vector<ResourceId> EvidenceVector::present_resources() const {
  std::vector<ResourceId> out;
  for (const EvidenceEntry& entry : entries_) {
    if (entry.state == EvidenceState::Present) {
      out.push_back(entry.resource);
    }
  }
  return out;
}

Result<EvidenceVector> EvidenceVector::merged(const FailureObservation& observation) const {
  if (observation.resource.empty()) {
    return Status{StatusCode::InvalidArgument, "observation resource id must not be empty"};
  }
  if (observation.generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "observation generation must be non-zero"};
  }

  std::vector<EvidenceEntry> entries = entries_;
  const auto it = std::lower_bound(entries.begin(), entries.end(), observation.resource,
                                   [](const EvidenceEntry& entry, const ResourceId& key) {
                                     return entry.resource < key;
                                   });

  EvidenceEntry next;
  next.resource = observation.resource;
  next.state = observation.state;
  next.generation = observation.generation;
  next.source_digest = observation.source_digest;

  if (it == entries.end() || !(it->resource == observation.resource)) {
    entries.insert(it, std::move(next));
    return EvidenceVector::build(std::move(entries));
  }

  if (observation.generation < it->generation) {
    return Status{StatusCode::SequenceRegression, "observation generation regressed"};
  }
  if (observation.generation == it->generation) {
    if (it->source_digest != observation.source_digest || it->state != observation.state) {
      return Status{StatusCode::Conflict, "contradictory observation at the same generation"};
    }
    // Identical re-report: no evidence change, but the caller learns it is current.
    return *this;
  }
  *it = std::move(next);
  return EvidenceVector::build(std::move(entries));
}

std::vector<std::byte> EvidenceVector::encode() const {
  CanonicalWriter writer;
  writer.u32(static_cast<std::uint32_t>(entries_.size()));
  for (const EvidenceEntry& entry : entries_) {
    writer.resource_id(entry.resource);
    writer.u8(static_cast<std::uint8_t>(entry.state));
    writer.strong(entry.generation);
    writer.digest(entry.source_digest);
  }
  return writer.data();
}

Result<EvidenceVector> EvidenceVector::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > kMaxTopologyNodes) {
    return Status{StatusCode::LimitExceeded, "encoded evidence count exceeds bound"};
  }
  std::vector<EvidenceEntry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    EvidenceEntry entry;
    auto resource = reader.resource_id();
    if (!reader.ok()) {
      return reader.status();
    }
    if (!resource.ok()) {
      return resource.status();
    }
    entry.resource = std::move(resource.value());
    const std::uint8_t state = reader.u8();
    entry.generation = reader.strong<EvidenceGenerationTag>();
    entry.source_digest = reader.digest();
    if (!reader.ok()) {
      return reader.status();
    }
    if (state > static_cast<std::uint8_t>(EvidenceState::Unknown)) {
      return Status{StatusCode::InvalidArgument, "invalid evidence state value"};
    }
    entry.state = static_cast<EvidenceState>(state);
    entries.push_back(std::move(entry));
  }
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return EvidenceVector::build(std::move(entries));
}

}  // namespace fcfn::model
