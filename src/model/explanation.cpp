// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/explanation.hpp"

#include <algorithm>

namespace fcfn::model {
namespace {

struct ReasonToken {
  ReasonCode code;
  const char* token;
};

constexpr ReasonToken kReasonTokens[] = {
    {ReasonCode::None, "none"},
    {ReasonCode::FailureSourceContained, "failure_source_contained"},
    {ReasonCode::CutVertexOnProvenPath, "cut_vertex_on_proven_path"},
    {ReasonCode::CutVertexOnUnknownPath, "cut_vertex_on_unknown_path"},
    {ReasonCode::ProtectedObligationIncluded, "protected_obligation_included"},
    {ReasonCode::SharedRiskDomainFence, "shared_risk_domain_fence"},
    {ReasonCode::PolicyMandatedInclusion, "policy_mandated_inclusion"},
    {ReasonCode::UnknownEdgesPresent, "unknown_edges_present"},
    {ReasonCode::IncompleteAdjacencyOnFrontier, "incomplete_adjacency_on_frontier"},
    {ReasonCode::EvidenceStale, "evidence_stale"},
    {ReasonCode::EvidenceConflicting, "evidence_conflicting"},
    {ReasonCode::EvidenceGenerationRegressed, "evidence_generation_regressed"},
    {ReasonCode::AdjacencyCompletenessUnknown, "adjacency_completeness_unknown"},
    {ReasonCode::OptimalityProven, "optimality_proven"},
    {ReasonCode::FeasibilityProven, "feasibility_proven"},
    {ReasonCode::SearchLimitReached, "search_limit_reached"},
    {ReasonCode::InfeasibleCertificateIneligiblePath, "infeasible_certificate_ineligible_path"},
    {ReasonCode::InfeasibleByExhaustiveSearch, "infeasible_by_exhaustive_search"},
    {ReasonCode::NoFeasibleSolutionFound, "no_feasible_solution_found"},
    {ReasonCode::BoundedSolutionNotMinimal, "bounded_solution_not_minimal"},
    {ReasonCode::HeuristicConstructionUsed, "heuristic_construction_used"},
    {ReasonCode::LocalMinimalityRestored, "local_minimality_restored"},
    {ReasonCode::TopologyEmpty, "topology_empty"},
    {ReasonCode::NoFailureSources, "no_failure_sources"},
    {ReasonCode::NoProtectedObligations, "no_protected_obligations"},
    {ReasonCode::SourceNotContainable, "source_not_containable"},
    {ReasonCode::ProtectedObligationNotContainable, "protected_obligation_not_containable"},
    {ReasonCode::WeightBudgetExceeded, "weight_budget_exceeded"},
    {ReasonCode::MemberBudgetExceeded, "member_budget_exceeded"},
    {ReasonCode::InstanceReduced, "instance_reduced"},
    {ReasonCode::FailureSourceNotInTopology, "failure_source_not_in_topology"},
    {ReasonCode::EvidenceNotConfirmedSinceRestart, "evidence_not_confirmed_since_restart"},
    {ReasonCode::AuthorityValidated, "authority_validated"},
    {ReasonCode::AuthorityStaleEpoch, "authority_stale_epoch"},
    {ReasonCode::AuthorityStaleBoot, "authority_stale_boot"},
    {ReasonCode::AuthorityStalePolicy, "authority_stale_policy"},
    {ReasonCode::AuthorityStaleTopology, "authority_stale_topology"},
    {ReasonCode::AuthorityStaleEvidence, "authority_stale_evidence"},
    {ReasonCode::AuthorityFenced, "authority_fenced"},
    {ReasonCode::AuthorizationRequired, "authorization_required"},
    {ReasonCode::ApplySubmitted, "apply_submitted"},
    {ReasonCode::ApplyAcknowledged, "apply_acknowledged"},
    {ReasonCode::EffectVerified, "effect_verified"},
    {ReasonCode::EffectAmbiguous, "effect_ambiguous"},
    {ReasonCode::EffectNotApplied, "effect_not_applied"},
    {ReasonCode::EffectVerificationDigestMismatch, "effect_verification_digest_mismatch"},
    {ReasonCode::DuplicateCompletionRejected, "duplicate_completion_rejected"},
    {ReasonCode::AttemptFencedByRestart, "attempt_fenced_by_restart"},
    {ReasonCode::AttemptSequenceRegressed, "attempt_sequence_regressed"},
    {ReasonCode::ApplierEpochStale, "applier_epoch_stale"},
    {ReasonCode::TransitionAccepted, "transition_accepted"},
    {ReasonCode::TransitionDeniedUnverifiedEffect, "transition_denied_unverified_effect"},
    {ReasonCode::TransitionDeniedActiveFailures, "transition_denied_active_failures"},
    {ReasonCode::TransitionStaleGeneration, "transition_stale_generation"},
    {ReasonCode::TransitionSupersededByNewerPlan, "transition_superseded_by_newer_plan"},
    {ReasonCode::TransitionReleaseRequiresEmptyBoundary, "transition_release_requires_empty_boundary"},
    {ReasonCode::TransitionContractNotASubset, "transition_contract_not_a_subset"},
    {ReasonCode::TransitionExpandNotASuperset, "transition_expand_not_a_superset"},
    {ReasonCode::TransitionSuperseded, "transition_superseded"},
    {ReasonCode::StoreRecovered, "store_recovered"},
    {ReasonCode::TornTailRecovered, "torn_tail_recovered"},
    {ReasonCode::StoreRefusedCorruption, "store_refused_corruption"},
    {ReasonCode::StoreVersionUnsupported, "store_version_unsupported"},
    {ReasonCode::RestartEpochAdvanced, "restart_epoch_advanced"},
    {ReasonCode::PreRestartAttemptsFenced, "pre_restart_attempts_fenced"},
    {ReasonCode::PreRestartLeasesFenced, "pre_restart_leases_fenced"},
    {ReasonCode::SnapshotCommitted, "snapshot_committed"},
    {ReasonCode::EvidenceFreshnessNotRestored, "evidence_freshness_not_restored"},
};

}  // namespace

const char* to_string(ReasonCode code) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (entry.code == code) {
      return entry.token;
    }
  }
  return "unrecognised_reason";
}

bool parse_reason_code(std::string_view token, ReasonCode& out) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (token == entry.token) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::string ExplanationItem::render() const {
  std::string out = to_string(code);
  if (has_subject) {
    out += " subject=";
    out += subject.value();
  }
  if (has_value) {
    out += " value=";
    out += std::to_string(value);
  }
  if (!text.empty()) {
    out += " ";
    out += text;
  }
  return out;
}

bool Explanation::add(ReasonCode code, std::string_view text) {
  if (items_.size() >= kMaxExplanationItems) {
    truncated_ = true;
    return false;
  }
  ExplanationItem item;
  item.code = code;
  const std::size_t limit = std::min(text.size(), kMaxExplanationTextLength);
  item.text.assign(text.substr(0, limit));
  items_.push_back(std::move(item));
  return true;
}

bool Explanation::add_resource(ReasonCode code, const ResourceId& subject, std::string_view text) {
  if (!add(code, text)) {
    return false;
  }
  items_.back().subject = subject;
  items_.back().has_subject = true;
  return true;
}

bool Explanation::add_value(ReasonCode code, std::uint64_t value, std::string_view text) {
  if (!add(code, text)) {
    return false;
  }
  items_.back().value = value;
  items_.back().has_value = true;
  return true;
}

bool Explanation::add_item(const ExplanationItem& item) {
  if (items_.size() >= kMaxExplanationItems) {
    truncated_ = true;
    return false;
  }
  ExplanationItem copy = item;
  if (copy.text.size() > kMaxExplanationTextLength) {
    copy.text.resize(kMaxExplanationTextLength);
  }
  items_.push_back(std::move(copy));
  return true;
}

bool Explanation::contains(ReasonCode code) const noexcept {
  return std::any_of(items_.begin(), items_.end(), [code](const ExplanationItem& item) {
    return item.code == code;
  });
}

std::string Explanation::render() const {
  std::string out;
  bool first = true;
  for (const ExplanationItem& item : items_) {
    if (!first) {
      out += "; ";
    }
    first = false;
    out += item.render();
  }
  if (truncated_) {
    out += "; [truncated]";
  }
  return out;
}

}  // namespace fcfn::model
