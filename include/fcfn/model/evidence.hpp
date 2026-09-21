// FCFN - failure evidence, detections, and evidence generations.
//
// Detection is not containment authority. A DetectionRecord states what was
// observed and under which generation; it never authorizes anything by itself.
// Evidence for one resource advances through strictly increasing generations,
// and contradictory reports at the same generation are a conflict, not a
// silent update.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_EVIDENCE_HPP
#define FCFN_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::model {

/// Observed failure state of a resource.
enum class EvidenceState : std::uint8_t {
  /// Authoritative evidence that the resource is failed/at risk right now.
  Present = 0,
  /// Authoritative evidence that the resource is not failed at this generation.
  Absent = 1,
  /// No usable evidence: explicitly unknown, never treated as healthy.
  Unknown = 2,
};

[[nodiscard]] const char* to_string(EvidenceState value) noexcept;
[[nodiscard]] bool parse_evidence_state(std::string_view token, EvidenceState& out) noexcept;

/// Raw observation submitted to the runtime.
struct FailureObservation {
  ResourceId resource{};
  EvidenceState state{EvidenceState::Unknown};
  EvidenceGeneration generation{};
  /// Digest of the authoritative detector payload that produced this observation.
  Digest source_digest{};
  /// Informational only. Never used as authority and never restored on restart.
  std::uint64_t observed_at_millis{0};
};

/// Durable record of an accepted observation.
struct DetectionRecord {
  ResourceId resource{};
  EvidenceState state{EvidenceState::Unknown};
  EvidenceGeneration generation{};
  Digest source_digest{};
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  Sequence sequence{};
  Digest record_digest{};

  [[nodiscard]] Digest compute_digest() const;
};

/// One entry of the evidence vector.
struct EvidenceEntry {
  ResourceId resource{};
  EvidenceState state{EvidenceState::Unknown};
  EvidenceGeneration generation{};
  Digest source_digest{};
};

/// Ordered, canonical evidence vector. Two vectors with the same content have
/// the same rendering and digest regardless of insertion order.
class EvidenceVector {
 public:
  EvidenceVector() = default;

  /// Build from entries. Rejects duplicates and malformed ids; sorts by resource.
  [[nodiscard]] static Result<EvidenceVector> build(std::vector<EvidenceEntry> entries);

  /// Merge an observation into the vector.
  ///   - unknown resource in vector: inserts, returns the new vector
  ///   - higher generation: replaces
  ///   - equal generation and equal digest: no-op
  ///   - equal generation and different digest: Conflict
  ///   - lower generation: SequenceRegression
  [[nodiscard]] Result<EvidenceVector> merged(const FailureObservation& observation) const;

  [[nodiscard]] const std::vector<EvidenceEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const EvidenceEntry* find(const ResourceId& resource) const noexcept;

  /// Resources whose current state is Present.
  [[nodiscard]] std::vector<ResourceId> present_resources() const;

  [[nodiscard]] Digest digest() const noexcept { return digest_; }
  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<EvidenceVector> decode(std::span<const std::byte> bytes);

 private:
  void recompute();

  std::vector<EvidenceEntry> entries_{};
  Digest digest_{};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_EVIDENCE_HPP
