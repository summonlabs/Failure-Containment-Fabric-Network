// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal runtime state. Not installed: the public surface is coordinator.hpp.
#ifndef FCFN_SRC_RUNTIME_RUNTIME_STATE_HPP
#define FCFN_SRC_RUNTIME_RUNTIME_STATE_HPP

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/store/store.hpp"

namespace fcfn::runtime {

/// Everything a live coordinator incarnation owns. All access is serialised by
/// mutex(); no external code is invoked while the lock is held.
struct RuntimeState {
  RuntimeConfig config{};
  Clock* clock{nullptr};
  std::unique_ptr<store::DurableStore> store{};

  // Authority-bearing identity of this incarnation.
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  Sequence sequence{};
  model::EvidenceRevision evidence_revision{};

  // Durable definitions and evidence.
  std::optional<model::Topology> topology{};
  std::unique_ptr<engine::PropagationGraph> graph{};
  model::ContainmentPolicy policy{};
  model::EvidenceVector evidence{};
  /// Boot in which each Present-evidence resource was last confirmed. Dynamic:
  /// never restored from disk.
  std::map<model::ResourceId, BootId> evidence_confirmation{};
  /// Set when a detector reported contradictory evidence; cleared by a strictly
  /// newer generation for the same resource.
  bool evidence_conflict{false};

  // Monotone counters (durable).
  PlanGeneration plan_counter{};
  TransitionGeneration transition_counter{};
  AttemptSequence attempt_counter{};
  BoundaryGeneration boundary_counter{};
  Sequence authorization_counter{};

  // Current containment intent.
  std::optional<model::ContainmentBoundary> boundary{};
  PlanGeneration boundary_plan{};
  Digest boundary_plan_digest{};
  model::EffectState effect_state{model::EffectState::NotRequested};
  /// Set on restart: the effect of the surviving boundary must be re-verified by
  /// the enforcement plane under this incarnation before it is treated as current.
  bool effect_requires_reverification{false};
  /// Boot identity observed in the recovered log; fenced on startup.
  std::optional<BootIdentity> last_known_boot{};

  // Bounded tables.
  std::deque<model::ContainmentPlan> plans{};
  std::deque<model::ApplyAttempt> attempts{};
  std::deque<model::TransitionDecision> transitions{};
  std::deque<model::DetectionRecord> detections{};

  struct ActiveAuthorization {
    AuthorizationRecord record{};
    bool fenced{false};
  };
  std::deque<ActiveAuthorization> authorizations{};

  // Fences: process incarnations whose authority is no longer current.
  std::vector<BootId> fenced_boots{};

  StartupReport startup{};
  RuntimeStats stats{};

  mutable std::mutex lock{};

  [[nodiscard]] model::AuthorityVector current_authority() const;
  [[nodiscard]] StatusResult ensure_topology() const;

  /// Append a durable record and publish the new sequence. The caller must have
  /// already applied the change to memory or must apply it after this returns.
  [[nodiscard]] VoidResult commit(store::RecordType type, std::span<const std::byte> payload);

  [[nodiscard]] model::ContainmentPlan* find_plan(PlanGeneration generation);
  [[nodiscard]] const model::ContainmentPlan* find_plan(PlanGeneration generation) const;
  [[nodiscard]] model::ApplyAttempt* find_attempt(const model::ApplyAttemptId& id);
  [[nodiscard]] const model::ApplyAttempt* find_attempt(const model::ApplyAttemptId& id) const;
  [[nodiscard]] ActiveAuthorization* find_authorization(AuthorizationId id);
  void evict_bounded_tables();
  void recompute_effect_state();
};

/// Validate an authorization against the current authority and lease. Defined in
/// coordinator.cpp and shared with the apply protocol.
[[nodiscard]] Result<AuthorizationRecord> validate_authorization(RuntimeState& state, AuthorizationId id,
                                                               bool require_apply);

/// Canonical codecs for durable records. Defined in codec.cpp.
namespace codec {

[[nodiscard]] std::vector<std::byte> encode_detection(const model::DetectionRecord& record);
[[nodiscard]] Result<model::DetectionRecord> decode_detection(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_attempt(const model::ApplyAttempt& attempt);
[[nodiscard]] Result<model::ApplyAttempt> decode_attempt(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_acknowledgement(const model::ApplyAcknowledgement& ack);
[[nodiscard]] Result<model::ApplyAcknowledgement> decode_acknowledgement(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_verification(const model::EffectVerification& report);
[[nodiscard]] Result<model::EffectVerification> decode_verification(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_transition(const model::TransitionDecision& decision);
[[nodiscard]] Result<model::TransitionDecision> decode_transition(std::span<const std::byte> bytes);

/// Durable boundary decision: which plan produced the boundary, and the boundary
/// itself. An empty boundary payload records a release.
[[nodiscard]] std::vector<std::byte> encode_boundary_decision(PlanGeneration plan, const Digest& plan_digest,
                                                             bool released,
                                                             std::span<const std::byte> boundary_bytes);
struct BoundaryDecision {
  PlanGeneration plan{};
  Digest plan_digest{};
  bool released{false};
  std::vector<std::byte> boundary_bytes{};
};
[[nodiscard]] Result<BoundaryDecision> decode_boundary_decision(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_epoch_advance(CoordinatorEpoch epoch, const BootIdentity& boot);
[[nodiscard]] Result<std::pair<CoordinatorEpoch, BootIdentity>> decode_epoch_advance(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_fence(const std::vector<BootId>& boots);
[[nodiscard]] Result<std::vector<BootId>> decode_fence(std::span<const std::byte> bytes);

/// Full durable state snapshot payload.
struct DurableState {
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  Sequence sequence{};
  model::EvidenceRevision evidence_revision{};
  PlanGeneration plan_counter{};
  TransitionGeneration transition_counter{};
  AttemptSequence attempt_counter{};
  BoundaryGeneration boundary_counter{};
  Sequence authorization_counter{};
  std::vector<std::byte> topology_bytes{};
  std::vector<std::byte> policy_bytes{};
  std::vector<std::byte> evidence_bytes{};
  std::vector<std::byte> boundary_bytes{};
  PlanGeneration boundary_plan{};
  Digest boundary_plan_digest{};
  std::uint8_t effect_state{0};
  std::vector<model::ContainmentPlan> plans{};
  std::vector<model::ApplyAttempt> attempts{};
  std::vector<model::TransitionDecision> transitions{};
  std::vector<model::DetectionRecord> detections{};
  std::vector<BootId> fenced_boots{};
  /// Authorizations active at snapshot time. They are never restored as active:
  /// the count is reported as fenced so operators see what must be re-issued.
  std::uint32_t active_authorizations{0};
};

[[nodiscard]] std::vector<std::byte> encode_durable_state(const DurableState& state);
[[nodiscard]] Result<DurableState> decode_durable_state(std::span<const std::byte> bytes);

}  // namespace codec

}  // namespace fcfn::runtime

#endif  // FCFN_SRC_RUNTIME_RUNTIME_STATE_HPP
