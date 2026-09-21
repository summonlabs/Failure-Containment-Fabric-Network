// FCFN - deterministic explanation vocabulary.
//
// Explanations are contract data, not prose: each item carries a stable reason
// code, the exact subject it applies to, and an optional numeric value. The
// rendered text is derived from those fields so that two runs over identical
// inputs produce identical documents.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_EXPLANATION_HPP
#define FCFN_MODEL_EXPLANATION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::model {

/// Stable reason codes. The numeric values are part of the durable contract.
enum class ReasonCode : std::uint16_t {
  None = 0,

  // Containment structure
  FailureSourceContained = 1,
  CutVertexOnProvenPath = 2,
  CutVertexOnUnknownPath = 3,
  ProtectedObligationIncluded = 4,
  SharedRiskDomainFence = 5,
  PolicyMandatedInclusion = 6,

  // Evidence quality
  UnknownEdgesPresent = 10,
  IncompleteAdjacencyOnFrontier = 11,
  EvidenceStale = 12,
  EvidenceConflicting = 13,
  EvidenceGenerationRegressed = 14,
  AdjacencyCompletenessUnknown = 15,

  // Feasibility and optimality
  OptimalityProven = 20,
  FeasibilityProven = 21,
  SearchLimitReached = 22,
  InfeasibleCertificateIneligiblePath = 23,
  InfeasibleByExhaustiveSearch = 24,
  NoFeasibleSolutionFound = 25,
  BoundedSolutionNotMinimal = 26,
  HeuristicConstructionUsed = 27,
  LocalMinimalityRestored = 28,

  // Input domain
  TopologyEmpty = 30,
  NoFailureSources = 31,
  NoProtectedObligations = 32,
  SourceNotContainable = 33,
  ProtectedObligationNotContainable = 34,
  WeightBudgetExceeded = 35,
  MemberBudgetExceeded = 36,
  InstanceReduced = 37,
  FailureSourceNotInTopology = 38,
  EvidenceNotConfirmedSinceRestart = 39,

  // Authority and lifecycle
  AuthorityValidated = 40,
  AuthorityStaleEpoch = 41,
  AuthorityStaleBoot = 42,
  AuthorityStalePolicy = 43,
  AuthorityStaleTopology = 44,
  AuthorityStaleEvidence = 45,
  AuthorityFenced = 46,
  AuthorizationRequired = 47,

  // Apply protocol
  ApplySubmitted = 50,
  ApplyAcknowledged = 51,
  EffectVerified = 52,
  EffectAmbiguous = 53,
  EffectNotApplied = 54,
  EffectVerificationDigestMismatch = 55,
  DuplicateCompletionRejected = 56,
  AttemptFencedByRestart = 57,
  AttemptSequenceRegressed = 58,
  ApplierEpochStale = 59,

  // Transitions
  TransitionAccepted = 70,
  TransitionDeniedUnverifiedEffect = 71,
  TransitionDeniedActiveFailures = 72,
  TransitionStaleGeneration = 73,
  TransitionSupersededByNewerPlan = 74,
  TransitionReleaseRequiresEmptyBoundary = 75,
  TransitionContractNotASubset = 76,
  TransitionExpandNotASuperset = 77,
  TransitionSuperseded = 78,

  // Persistence and restart
  StoreRecovered = 90,
  TornTailRecovered = 91,
  StoreRefusedCorruption = 92,
  StoreVersionUnsupported = 93,
  RestartEpochAdvanced = 94,
  PreRestartAttemptsFenced = 95,
  PreRestartLeasesFenced = 96,
  SnapshotCommitted = 97,
  EvidenceFreshnessNotRestored = 98,
};

/// Highest defined reason code; canonical decoders reject anything above it.
inline constexpr std::uint16_t kMaxReasonCodeValue = 98;

[[nodiscard]] const char* to_string(ReasonCode code) noexcept;
[[nodiscard]] bool parse_reason_code(std::string_view token, ReasonCode& out) noexcept;

/// One bounded explanation item.
struct ExplanationItem {
  ReasonCode code{ReasonCode::None};
  ResourceId subject{};
  bool has_subject{false};
  std::uint64_t value{0};
  bool has_value{false};
  std::string text{};

  [[nodiscard]] std::string render() const;
};

/// Bounded explanation document.
class Explanation {
 public:
  /// Items are best-effort within the configured bound; when the bound is
  /// reached truncation() becomes true and the item is dropped. The bool result
  /// reports which happened.
  bool add(ReasonCode code, std::string_view text = {});
  bool add_resource(ReasonCode code, const ResourceId& subject, std::string_view text = {});
  bool add_value(ReasonCode code, std::uint64_t value, std::string_view text = {});
  /// Append a fully formed item (used by canonical decoding). Applies the same
  /// bound as add().
  bool add_item(const ExplanationItem& item);
  void mark_truncated() noexcept { truncated_ = true; }

  [[nodiscard]] const std::vector<ExplanationItem>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] bool contains(ReasonCode code) const noexcept;
  [[nodiscard]] std::string render() const;

 private:
  std::vector<ExplanationItem> items_{};
  bool truncated_{false};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_EXPLANATION_HPP
