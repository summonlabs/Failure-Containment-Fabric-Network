// FCFN - containment runtime coordinator.
//
// The coordinator is the only component that holds authority. It ingests
// authoritative failure evidence, issues bounded authorizations, computes
// containment plans, records generation-bound transitions, and tracks the apply
// protocol through to verified effect. Every externally visible answer binds the
// exact generations that made it legal, and every durable mutation follows the
// append -> flush -> publish ordering.
//
// Concurrency: one internal mutex guards all mutable state. No callback or
// user-provided code is ever invoked while holding it, because FCFN has no
// callback API: observers poll for events instead.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_RUNTIME_COORDINATOR_HPP
#define FCFN_RUNTIME_COORDINATOR_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/core/ids.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/model/attempt.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/boundary.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/model/policy.hpp"
#include "fcfn/model/topology.hpp"
#include "fcfn/model/transition.hpp"

namespace fcfn::runtime {

/// Authorization identity lives in the model layer; the runtime re-exports it.
using AuthorizationId = model::AuthorizationId;
struct EventSequenceTag;
using EventSequence = StrongId<EventSequenceTag>;

/// Bounded runtime configuration. Nothing here is dynamic authority.
struct RuntimeConfig {
  std::filesystem::path store_root{};
  /// When false the runtime keeps no durable state (used by focused unit tests).
  bool persist{true};
  /// Flush durable writes to stable storage before publishing them.
  bool durable_commit{true};
  /// Recover a partial record at the end of the log.
  bool allow_torn_tail_recovery{true};
  model::ContainmentPolicy policy{};
  /// Initial topology definition. Empty means "no topology yet".
  model::TopologySpec topology{};
  model::TopologyGeneration topology_generation{};
  std::uint64_t authorization_lease_millis{30000};
  /// Present evidence that has not been confirmed in the current boot degrades a
  /// plan to INDETERMINATE. Conservative restart semantics; disable explicitly.
  bool require_evidence_confirmation_in_current_boot{true};
  std::size_t max_retained_plans{kMaxRetainedPlans};
  std::size_t max_retained_attempts{kMaxRetainedAttempts};
  std::size_t max_retained_transitions{kMaxRetainedTransitions};
  std::size_t max_retained_detections{kMaxRetainedDetections};
  std::size_t max_active_authorizations{64};
};

/// What the runtime did while starting.
struct StartupReport {
  bool fresh{false};
  bool recovered{false};
  bool torn_tail_recovered{false};
  std::uint64_t torn_tail_bytes{0};
  std::size_t replayed_records{0};
  std::size_t skipped_records{0};
  std::size_t fenced_attempts{0};
  std::size_t fenced_authorizations{0};
  std::size_t restored_plans{0};
  std::size_t restored_detections{0};
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  TopologyGeneration topology{};
  PolicyGeneration policy{};
  Sequence last_sequence{};
  SnapshotSequence snapshot_sequence{};
  SnapshotSequence wal_segment{};
  model::Explanation explanation{};
};

/// An authority-bound authorization. Authorization is not application: it only
/// permits the holder to request a plan or an apply attempt while the bound
/// generations remain current.
struct AuthorizationRecord {
  AuthorizationId id{};
  model::AuthorityVector authority{};
  Sequence issued_sequence{};
  std::uint64_t issued_at_millis{0};
  std::uint64_t expires_at_millis{0};
  bool allows_apply{true};
};

struct PlanRequest {
  AuthorizationId authorization{};
};

struct SubmitApplyRequest {
  AuthorizationId authorization{};
  PlanGeneration plan{};
  Digest plan_digest{};
};

struct RuntimeStats {
  std::uint64_t detections_accepted{0};
  std::uint64_t detections_unchanged{0};
  std::uint64_t detections_rejected{0};
  std::uint64_t plans_computed{0};
  std::uint64_t plans_rejected{0};
  std::uint64_t transitions_accepted{0};
  std::uint64_t transitions_denied{0};
  std::uint64_t apply_attempts{0};
  std::uint64_t acknowledgements{0};
  std::uint64_t verifications{0};
  std::uint64_t duplicate_completions_rejected{0};
  std::uint64_t authority_rejections{0};
  std::uint64_t durable_commits{0};
  std::uint64_t snapshot_commits{0};
};

/// The containment runtime. One instance per process incarnation.
class ContainmentRuntime {
 public:
  ~ContainmentRuntime();
  ContainmentRuntime(const ContainmentRuntime&) = delete;
  ContainmentRuntime& operator=(const ContainmentRuntime&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<ContainmentRuntime>> open(const RuntimeConfig& config,
                                                                       Clock* clock);

  [[nodiscard]] const StartupReport& startup() const noexcept;
  [[nodiscard]] model::AuthorityVector authority() const;
  [[nodiscard]] model::ContainmentPolicy policy() const;
  [[nodiscard]] RuntimeStats stats() const;

  /// Current topology generation, or UNSUPPORTED when no topology is loaded.
  [[nodiscard]] Result<model::TopologyGeneration> topology_generation() const;
  [[nodiscard]] Result<model::Topology> topology() const;
  [[nodiscard]] Result<model::EvidenceVector> evidence() const;

  // ---- ingestion ---------------------------------------------------------
  [[nodiscard]] Result<model::TopologyGeneration> apply_topology(model::TopologySpec spec);
  [[nodiscard]] Result<model::PolicyGeneration> apply_policy(model::ContainmentPolicy policy);
  [[nodiscard]] Result<model::DetectionRecord> record_detection(
      const model::FailureObservation& observation);

  // ---- authority ---------------------------------------------------------
  [[nodiscard]] Result<AuthorizationRecord> authorize();

  // ---- planning ----------------------------------------------------------
  [[nodiscard]] Result<model::ContainmentPlan> plan(const PlanRequest& request);
  [[nodiscard]] Result<model::ContainmentPlan> plan_by_generation(PlanGeneration generation) const;

  // ---- transitions -------------------------------------------------------
  [[nodiscard]] Result<model::TransitionDecision> transition(const model::TransitionRequest& request);
  [[nodiscard]] Result<model::TransitionDecision> transition_by_generation(
      TransitionGeneration generation) const;
  [[nodiscard]] Result<model::ContainmentBoundary> current_boundary() const;
  [[nodiscard]] model::EffectState current_effect_state() const;

  // ---- apply protocol ----------------------------------------------------
  [[nodiscard]] Result<model::ApplyAttempt> submit_apply(const SubmitApplyRequest& request);
  [[nodiscard]] Result<model::ApplyAttempt> acknowledge(const model::ApplyAcknowledgement& ack);
  [[nodiscard]] Result<model::ApplyAttempt> verify_effect(const model::EffectVerification& report);
  [[nodiscard]] Result<model::ApplyAttempt> attempt(PlanGeneration plan,
                                                    AttemptSequence sequence) const;
  [[nodiscard]] std::vector<model::ApplyAttempt> attempts() const;

  /// Write a durable snapshot of the current durable state and rotate the log.
  [[nodiscard]] VoidResult checkpoint();

 private:
  explicit ContainmentRuntime(std::unique_ptr<struct RuntimeState> state);
  std::unique_ptr<struct RuntimeState> state_;
};

/// Render a startup report as deterministic JSON.
[[nodiscard]] std::string to_json(const StartupReport& report);
/// Render a plan as deterministic JSON (alias of ContainmentPlan::to_json).
[[nodiscard]] std::string to_json(const model::ContainmentPlan& plan);
/// Render an attempt as deterministic JSON.
[[nodiscard]] std::string to_json(const model::ApplyAttempt& attempt);
/// Render an authorization as deterministic JSON.
[[nodiscard]] std::string to_json(const AuthorizationRecord& authorization);
/// Render runtime statistics as deterministic JSON.
[[nodiscard]] std::string to_json(const RuntimeStats& stats);

}  // namespace fcfn::runtime

#endif  // FCFN_RUNTIME_COORDINATOR_HPP
