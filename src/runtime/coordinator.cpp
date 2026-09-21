// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/runtime/coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <utility>

#include "fcfn/core/json.hpp"
#include "fcfn/engine/planner.hpp"
#include "runtime_state.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fcfn::runtime {
namespace {

std::uint32_t current_process_id() {
#ifdef _WIN32
  return static_cast<std::uint32_t>(::_getpid());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

BootId generate_boot_id() {
  std::random_device device;
  std::mt19937_64 generator(static_cast<std::uint64_t>(device()) ^
                            static_cast<std::uint64_t>(
                                std::chrono::steady_clock::now().time_since_epoch().count()));
  std::uniform_int_distribution<std::uint64_t> distribution(1, UINT64_MAX);
  return BootId{distribution(generator)};
}

void add_reason(model::Explanation& explanation, model::ReasonCode code, std::string_view text) {
  (void)explanation.add(code, text);
}

/// Lifecycle rank used when replaying attempt records: a later rank never
/// regresses to an earlier one.
int effect_rank(model::EffectState state) {
  switch (state) {
    case model::EffectState::NotRequested:
      return 0;
    case model::EffectState::Submitted:
      return 1;
    case model::EffectState::Acknowledged:
      return 2;
    case model::EffectState::Ambiguous:
      return 3;
    case model::EffectState::Failed:
      return 4;
    case model::EffectState::Fenced:
      return 5;
    case model::EffectState::Superseded:
      return 6;
    case model::EffectState::VerifiedApplied:
      return 7;
    case model::EffectState::VerifiedReleased:
      return 8;
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// RuntimeState
// ---------------------------------------------------------------------------

model::AuthorityVector RuntimeState::current_authority() const {
  model::AuthorityVector vector;
  vector.epoch = epoch;
  vector.boot = boot;
  vector.policy = policy.generation;
  vector.topology = topology.has_value() ? topology->generation() : TopologyGeneration{};
  vector.evidence_revision = evidence_revision;
  vector.evidence_digest = evidence.digest();
  vector.issued_sequence = sequence;
  return vector;
}

StatusResult RuntimeState::ensure_topology() const {
  if (!topology.has_value() || graph == nullptr) {
    return Status{StatusCode::Unsupported, "no topology definition is loaded"};
  }
  return ok_result();
}

VoidResult RuntimeState::commit(store::RecordType type, std::span<const std::byte> payload) {
  if (store != nullptr) {
    const VoidResult appended = store->commit(type, payload);
    if (!appended.ok()) {
      return appended.status();
    }
    sequence = store->last_sequence();
    ++stats.durable_commits;
  } else {
    sequence = Sequence{sequence.value() + 1};
  }
  return ok_result();
}

model::ContainmentPlan* RuntimeState::find_plan(PlanGeneration generation) {
  for (model::ContainmentPlan& plan : plans) {
    if (plan.generation == generation) {
      return &plan;
    }
  }
  return nullptr;
}

const model::ContainmentPlan* RuntimeState::find_plan(PlanGeneration generation) const {
  for (const model::ContainmentPlan& plan : plans) {
    if (plan.generation == generation) {
      return &plan;
    }
  }
  return nullptr;
}

model::ApplyAttempt* RuntimeState::find_attempt(const model::ApplyAttemptId& id) {
  for (model::ApplyAttempt& attempt : attempts) {
    if (attempt.id == id) {
      return &attempt;
    }
  }
  return nullptr;
}

const model::ApplyAttempt* RuntimeState::find_attempt(const model::ApplyAttemptId& id) const {
  for (const model::ApplyAttempt& attempt : attempts) {
    if (attempt.id == id) {
      return &attempt;
    }
  }
  return nullptr;
}

RuntimeState::ActiveAuthorization* RuntimeState::find_authorization(AuthorizationId id) {
  for (ActiveAuthorization& entry : authorizations) {
    if (entry.record.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

void RuntimeState::evict_bounded_tables() {
  while (plans.size() > config.max_retained_plans) {
    plans.pop_front();
  }
  while (transitions.size() > config.max_retained_transitions) {
    transitions.pop_front();
  }
  while (detections.size() > config.max_retained_detections) {
    detections.pop_front();
  }
  while (attempts.size() > config.max_retained_attempts) {
    // Never evict the newest attempt: it carries the current effect state.
    attempts.pop_front();
  }
  while (fenced_boots.size() > kMaxFencedBoots) {
    fenced_boots.erase(fenced_boots.begin());
  }
}

void RuntimeState::recompute_effect_state() {
  if (effect_requires_reverification) {
    effect_state = model::EffectState::Ambiguous;
    return;
  }
  if (boundary_plan.is_zero() && !boundary.has_value()) {
    effect_state = model::EffectState::NotRequested;
    return;
  }
  const model::ApplyAttempt* newest = nullptr;
  for (const model::ApplyAttempt& attempt : attempts) {
    if (!(attempt.id.plan == boundary_plan)) {
      continue;
    }
    if (newest == nullptr || newest->id.sequence < attempt.id.sequence) {
      newest = &attempt;
    }
  }
  if (newest == nullptr) {
    effect_state = model::EffectState::NotRequested;
    return;
  }
  effect_state = newest->state;
}

// ---------------------------------------------------------------------------
// Authorization validation (shared with the apply protocol)
// ---------------------------------------------------------------------------

Result<AuthorizationRecord> validate_authorization(RuntimeState& state, AuthorizationId id,
                                                   bool require_apply) {
  if (id.is_zero()) {
    return Status{StatusCode::Unauthorized, "no authorization presented"};
  }
  RuntimeState::ActiveAuthorization* entry = state.find_authorization(id);
  if (entry == nullptr) {
    return Status{StatusCode::Unauthorized, "authorization is not active"};
  }
  if (entry->fenced) {
    return Status{StatusCode::Fenced, "authorization was fenced by a later incarnation"};
  }
  const model::AuthorityCheck check = model::validate_authority(entry->record.authority,
                                                                state.current_authority());
  if (!check.ok()) {
    entry->fenced = true;
    ++state.stats.authority_rejections;
    return Status{check.status_code(), check.reason()};
  }
  if (state.clock != nullptr) {
    const std::uint64_t now = state.clock->now_millis();
    if (now >= entry->record.expires_at_millis) {
      entry->fenced = true;
      return Status{StatusCode::StaleGeneration, "authorization lease expired"};
    }
  }
  if (require_apply && !entry->record.allows_apply) {
    return Status{StatusCode::Denied, "authorization does not permit apply submissions"};
  }
  return entry->record;
}

// ---------------------------------------------------------------------------
// Startup and recovery
// ---------------------------------------------------------------------------

namespace {

/// Replay one durable record. Never silently drops a record it cannot decode:
/// an undecodable record means the log and this build disagree, which is a
/// refusal, not an absence.
StatusResult apply_replayed_frame(RuntimeState& state, const store::Frame& frame) {
  switch (frame.type) {
    case store::RecordType::TopologyDefinition: {
      auto decoded = model::Topology::decode(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      if (!state.topology.has_value() ||
          state.topology->generation() < decoded.value().generation()) {
        state.topology = std::move(decoded.value());
      }
      break;
    }
    case store::RecordType::PolicyDefinition: {
      auto decoded = model::ContainmentPolicy::decode(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      if (state.policy.generation < decoded.value().generation) {
        state.policy = decoded.value();
      }
      break;
    }
    case store::RecordType::DetectionRecord:
    case store::RecordType::EvidenceReconfirmed: {
      auto decoded = codec::decode_detection(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      auto merged = state.evidence.merged(model::FailureObservation{
          decoded.value().resource, decoded.value().state, decoded.value().generation,
          decoded.value().source_digest, 0});
      if (merged.ok()) {
        state.evidence = std::move(merged.value());
      }
      state.detections.push_back(decoded.value());
      ++state.startup.restored_detections;
      break;
    }
    case store::RecordType::PlanDecision: {
      auto decoded = model::ContainmentPlan::decode(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      state.plans.push_back(std::move(decoded.value()));
      ++state.startup.restored_plans;
      break;
    }
    case store::RecordType::BoundaryDecision: {
      auto decoded = codec::decode_boundary_decision(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      state.boundary_plan = decoded.value().plan;
      state.boundary_plan_digest = decoded.value().plan_digest;
      if (decoded.value().released) {
        state.boundary.reset();
        break;
      }
      auto boundary = model::ContainmentBoundary::decode(decoded.value().boundary_bytes);
      if (boundary.ok()) {
        state.boundary = std::move(boundary.value());
      }
      break;
    }
    case store::RecordType::TransitionDecision: {
      auto decoded = codec::decode_transition(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      model::TransitionDecision decision = std::move(decoded.value());
      if (state.transition_counter < decision.generation) {
        state.transition_counter = decision.generation;
      }
      state.transitions.push_back(std::move(decision));
      break;
    }
    case store::RecordType::ApplyAttempt: {
      auto decoded = codec::decode_attempt(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      model::ApplyAttempt* existing = state.find_attempt(decoded.value().id);
      if (existing == nullptr) {
        state.attempts.push_back(std::move(decoded.value()));
      } else if (effect_rank(existing->state) <= effect_rank(decoded.value().state)) {
        *existing = std::move(decoded.value());
      }
      break;
    }
    case store::RecordType::ApplyAcknowledgement:
    case store::RecordType::EffectVerification: {
      // Both record types durably carry the attempt state *after* the event, so
      // replay decodes them with the same codec used to write them. Decoding
      // them as bare messages would drop the applied state silently.
      auto decoded = codec::decode_attempt(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      model::ApplyAttempt* existing = state.find_attempt(decoded.value().id);
      if (existing == nullptr) {
        state.attempts.push_back(std::move(decoded.value()));
      } else if (effect_rank(existing->state) <= effect_rank(decoded.value().state)) {
        *existing = std::move(decoded.value());
      }
      break;
    }
    case store::RecordType::EpochAdvance: {
      auto decoded = codec::decode_epoch_advance(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      if (state.epoch < decoded.value().first) {
        state.epoch = decoded.value().first;
        state.last_known_boot = decoded.value().second;
      }
      break;
    }
    case store::RecordType::Fence: {
      auto decoded = codec::decode_fence(frame.payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      for (const BootId boot : decoded.value()) {
        if (std::find(state.fenced_boots.begin(), state.fenced_boots.end(), boot) ==
            state.fenced_boots.end()) {
          state.fenced_boots.push_back(boot);
        }
      }
      break;
    }
    case store::RecordType::AuthorizationIssued:
    case store::RecordType::SnapshotPayload:
    case store::RecordType::StartupMarker:
    case store::RecordType::ShutdownMarker:
      break;
  }
  return ok_result();
}

}  // namespace

ContainmentRuntime::ContainmentRuntime(std::unique_ptr<RuntimeState> state) : state_(std::move(state)) {}

ContainmentRuntime::~ContainmentRuntime() = default;

Result<std::unique_ptr<ContainmentRuntime>> ContainmentRuntime::open(const RuntimeConfig& config,
                                                                     Clock* clock) {
  if (clock == nullptr) {
    return Status{StatusCode::InvalidArgument, "runtime requires a clock"};
  }
  if (config.persist && config.store_root.empty()) {
    return Status{StatusCode::InvalidArgument, "durable runtime requires a store root"};
  }
  if (config.max_retained_plans == 0 || config.max_retained_plans > kMaxRetainedPlans) {
    return Status{StatusCode::InvalidArgument, "plan retention bound out of range"};
  }
  if (config.max_retained_attempts == 0 || config.max_retained_attempts > kMaxRetainedAttempts) {
    return Status{StatusCode::InvalidArgument, "attempt retention bound out of range"};
  }
  if (config.max_active_authorizations == 0 || config.max_active_authorizations > kMaxSessions) {
    return Status{StatusCode::InvalidArgument, "authorization bound out of range"};
  }

  auto state = std::make_unique<RuntimeState>();
  state->config = config;
  state->clock = clock;
  state->policy = config.policy;
  if (state->policy.generation.is_zero()) {
    state->policy = model::default_policy();
  }

  if (config.persist) {
    store::DurableStore::Options options;
    options.root = config.store_root;
    options.durable_commit = config.durable_commit;
    options.allow_torn_tail_recovery = config.allow_torn_tail_recovery;
    auto opened = store::DurableStore::open(options);
    if (!opened.ok()) {
      return opened.status();
    }
    state->store = std::move(opened.value());
    const store::DurableStore::RecoveryReport& report = state->store->recovery();
    state->startup.fresh = report.fresh;
    state->startup.recovered = !report.fresh;
    state->startup.torn_tail_recovered = report.torn_tail;
    state->startup.torn_tail_bytes = report.torn_tail_bytes;
    state->startup.replayed_records = report.replayed_records;
    state->startup.skipped_records = report.skipped_records;
    state->startup.snapshot_sequence = report.snapshot_sequence;
    state->startup.wal_segment = report.wal_segment;
    state->sequence = report.last_sequence;
    if (report.torn_tail) {
      add_reason(state->startup.explanation, model::ReasonCode::TornTailRecovered,
                 "a partial record at the end of the log was discarded");
    }

    if (!report.snapshot_payload.empty()) {
      auto durable = codec::decode_durable_state(report.snapshot_payload);
      if (!durable.ok()) {
        return durable.status();
      }
      codec::DurableState& restored = durable.value();
      state->epoch = restored.epoch;
      state->sequence = restored.sequence > state->sequence ? restored.sequence : state->sequence;
      state->evidence_revision = restored.evidence_revision;
      state->plan_counter = restored.plan_counter;
      state->transition_counter = restored.transition_counter;
      state->attempt_counter = restored.attempt_counter;
      state->boundary_counter = restored.boundary_counter;
      state->authorization_counter = restored.authorization_counter;
      state->boundary_plan = restored.boundary_plan;
      state->boundary_plan_digest = restored.boundary_plan_digest;
      state->effect_state = static_cast<model::EffectState>(restored.effect_state);
      state->plans.assign(std::make_move_iterator(restored.plans.begin()),
                          std::make_move_iterator(restored.plans.end()));
      state->attempts.assign(std::make_move_iterator(restored.attempts.begin()),
                             std::make_move_iterator(restored.attempts.end()));
      state->transitions.assign(std::make_move_iterator(restored.transitions.begin()),
                                std::make_move_iterator(restored.transitions.end()));
      state->detections.assign(std::make_move_iterator(restored.detections.begin()),
                               std::make_move_iterator(restored.detections.end()));
      state->fenced_boots = std::move(restored.fenced_boots);
      state->startup.restored_detections = state->detections.size();
      state->startup.restored_plans = state->plans.size();
      state->startup.fenced_authorizations = restored.active_authorizations;
      if (!restored.topology_bytes.empty()) {
        auto topology = model::Topology::decode(restored.topology_bytes);
        if (!topology.ok()) {
          return topology.status();
        }
        state->topology = std::move(topology.value());
      }
      if (!restored.policy_bytes.empty()) {
        auto policy = model::ContainmentPolicy::decode(restored.policy_bytes);
        if (!policy.ok()) {
          return policy.status();
        }
        if (state->policy.generation < policy.value().generation) {
          state->policy = policy.value();
        }
      }
      if (!restored.evidence_bytes.empty()) {
        auto evidence = model::EvidenceVector::decode(restored.evidence_bytes);
        if (!evidence.ok()) {
          return evidence.status();
        }
        state->evidence = std::move(evidence.value());
      }
      if (!restored.boundary_bytes.empty()) {
        auto boundary = model::ContainmentBoundary::decode(restored.boundary_bytes);
        if (!boundary.ok()) {
          return boundary.status();
        }
        state->boundary = std::move(boundary.value());
      }
    }

    for (const store::Frame& frame : state->store->replayed_frames()) {
      const StatusResult replayed = apply_replayed_frame(*state, frame);
      if (!replayed.ok()) {
        return replayed.status();
      }
    }
  } else {
    state->startup.fresh = true;
  }

  // Authorizations are never durable: the count reported at startup is what the
  // previous incarnation was holding when it snapshotted, never this empty table.
  state->authorizations.clear();

  // A new incarnation always advances the epoch and takes a fresh boot identity.
  const CoordinatorEpoch previous_epoch = state->epoch;
  state->epoch = CoordinatorEpoch{previous_epoch.value() + 1};
  state->boot = BootIdentity{generate_boot_id(), current_process_id()};
  if (state->last_known_boot.has_value() && state->last_known_boot->id != state->boot.id) {
    if (std::find(state->fenced_boots.begin(), state->fenced_boots.end(),
                  state->last_known_boot->id) == state->fenced_boots.end()) {
      state->fenced_boots.push_back(state->last_known_boot->id);
    }
  }
  if (state->startup.recovered) {
    add_reason(state->startup.explanation, model::ReasonCode::RestartEpochAdvanced,
               "coordinator epoch advanced for this incarnation");
    add_reason(state->startup.explanation, model::ReasonCode::EvidenceFreshnessNotRestored,
               "evidence freshness is not restored; reconfirmation is required");
  }

  // Derive the effect state from the replayed history before fencing anything, so
  // a log-only recovery agrees with a snapshot recovery.
  state->recompute_effect_state();

  // Fence everything that was in flight before this incarnation.
  for (model::ApplyAttempt& attempt : state->attempts) {
    if (!attempt.terminal()) {
      attempt.state = model::EffectState::Fenced;
      attempt.explanation = model::Explanation{};
      add_reason(attempt.explanation, model::ReasonCode::AttemptFencedByRestart,
                 "attempt was in flight when the previous incarnation stopped");
      ++state->startup.fenced_attempts;
    }
  }
  if (state->effect_state == model::EffectState::VerifiedApplied ||
      state->effect_state == model::EffectState::VerifiedReleased ||
      state->effect_state == model::EffectState::Submitted ||
      state->effect_state == model::EffectState::Acknowledged) {
    // Effect state is never restored as current: only the enforcement plane can
    // re-establish it, under this incarnation's authority.
    state->effect_state = model::EffectState::Ambiguous;
    state->effect_requires_reverification = true;
    add_reason(state->startup.explanation, model::ReasonCode::PreRestartAttemptsFenced,
               "pre-restart effect state requires fresh verification");
  }
  add_reason(state->startup.explanation, model::ReasonCode::PreRestartLeasesFenced,
             "pre-restart authorizations are not restored");

  if (state->topology.has_value()) {
    state->graph = std::make_unique<engine::PropagationGraph>(
        engine::PropagationGraph::build(state->topology.value()));
  }
  if (!config.topology.nodes.empty() && !state->topology.has_value()) {
    auto topology = model::Topology::build(config.topology);
    if (!topology.ok()) {
      return topology.status();
    }
    state->topology = std::move(topology.value());
    state->graph = std::make_unique<engine::PropagationGraph>(
        engine::PropagationGraph::build(state->topology.value()));
    if (state->store != nullptr) {
      const std::vector<std::byte> encoded = state->topology->encode();
      const VoidResult committed =
          state->commit(store::RecordType::TopologyDefinition, encoded);
      if (!committed.ok()) {
        return committed.status();
      }
    }
  }
  if (state->store != nullptr) {
    if (state->policy.generation.is_zero()) {
      state->policy = model::default_policy();
    }
    const std::vector<std::byte> policy_bytes = state->policy.encode();
    const VoidResult policy_committed =
        state->commit(store::RecordType::PolicyDefinition, policy_bytes);
    if (!policy_committed.ok()) {
      return policy_committed.status();
    }
    const std::vector<std::byte> epoch_bytes = codec::encode_epoch_advance(state->epoch, state->boot);
    const VoidResult epoch_committed = state->commit(store::RecordType::EpochAdvance, epoch_bytes);
    if (!epoch_committed.ok()) {
      return epoch_committed.status();
    }
    const std::vector<std::byte> fence_bytes = codec::encode_fence(state->fenced_boots);
    const VoidResult fence_committed = state->commit(store::RecordType::Fence, fence_bytes);
    if (!fence_committed.ok()) {
      return fence_committed.status();
    }
  }

  state->startup.epoch = state->epoch;
  state->startup.boot = state->boot;
  state->startup.topology = state->topology.has_value() ? state->topology->generation()
                                                        : TopologyGeneration{};
  state->startup.policy = state->policy.generation;
  state->startup.last_sequence = state->sequence;
  state->evict_bounded_tables();

  return std::unique_ptr<ContainmentRuntime>(new ContainmentRuntime(std::move(state)));
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const StartupReport& ContainmentRuntime::startup() const noexcept { return state_->startup; }

model::AuthorityVector ContainmentRuntime::authority() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return state_->current_authority();
}

model::ContainmentPolicy ContainmentRuntime::policy() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return state_->policy;
}

RuntimeStats ContainmentRuntime::stats() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return state_->stats;
}

Result<model::TopologyGeneration> ContainmentRuntime::topology_generation() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (!state_->topology.has_value()) {
    return Status{StatusCode::Unsupported, "no topology definition is loaded"};
  }
  return state_->topology->generation();
}

Result<model::Topology> ContainmentRuntime::topology() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (!state_->topology.has_value()) {
    return Status{StatusCode::Unsupported, "no topology definition is loaded"};
  }
  return state_->topology.value();
}

Result<model::EvidenceVector> ContainmentRuntime::evidence() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return state_->evidence;
}

// ---------------------------------------------------------------------------
// Ingestion
// ---------------------------------------------------------------------------

Result<model::TopologyGeneration> ContainmentRuntime::apply_topology(model::TopologySpec spec) {
  auto topology = model::Topology::build(std::move(spec));
  if (!topology.ok()) {
    return topology.status();
  }
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (state_->topology.has_value() && !(state_->topology->generation() < topology.value().generation())) {
    return Status{StatusCode::StaleGeneration, "topology generation did not advance"};
  }
  const std::vector<std::byte> encoded = topology.value().encode();
  const VoidResult committed = state_->commit(store::RecordType::TopologyDefinition, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  state_->topology = std::move(topology.value());
  state_->graph = std::make_unique<engine::PropagationGraph>(
      engine::PropagationGraph::build(state_->topology.value()));
  return state_->topology->generation();
}

Result<model::PolicyGeneration> ContainmentRuntime::apply_policy(model::ContainmentPolicy policy) {
  // The same domain rules the durable decoder enforces: a policy accepted here
  // must always be encodable, storable, and recoverable.
  const VoidResult valid = model::validate_policy(policy);
  if (!valid.ok()) {
    return valid.status();
  }
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (!(state_->policy.generation < policy.generation)) {
    return Status{StatusCode::StaleGeneration, "policy generation did not advance"};
  }
  const std::vector<std::byte> encoded = policy.encode();
  const VoidResult committed = state_->commit(store::RecordType::PolicyDefinition, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  state_->policy = policy;
  return state_->policy.generation;
}

Result<model::DetectionRecord> ContainmentRuntime::record_detection(
    const model::FailureObservation& observation) {
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (observation.generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "observation generation must be non-zero"};
  }
  const Result<model::EvidenceVector> merged = state_->evidence.merged(observation);
  if (!merged.ok()) {
    if (merged.status().code() == StatusCode::Conflict) {
      state_->evidence_conflict = true;
    }
    ++state_->stats.detections_rejected;
    return merged.status();
  }

  model::DetectionRecord record;
  record.resource = observation.resource;
  record.state = observation.state;
  record.generation = observation.generation;
  record.source_digest = observation.source_digest;
  record.epoch = state_->epoch;
  record.boot = state_->boot;
  record.sequence = Sequence{state_->sequence.value() + 1};
  record.record_digest = record.compute_digest();

  const bool changed = !(merged.value().digest() == state_->evidence.digest());
  if (changed) {
    const std::vector<std::byte> encoded = codec::encode_detection(record);
    const VoidResult committed = state_->commit(store::RecordType::DetectionRecord, encoded);
    if (!committed.ok()) {
      return committed.status();
    }
    if (!(state_->sequence == record.sequence)) {
      return Status{StatusCode::Internal, "durable sequence diverged from the published sequence"};
    }
    state_->evidence = merged.value();
    state_->evidence_revision = model::EvidenceRevision{state_->evidence_revision.value() + 1};
    state_->detections.push_back(record);
    state_->evict_bounded_tables();
    state_->evidence_conflict = false;
    ++state_->stats.detections_accepted;
  } else {
    ++state_->stats.detections_unchanged;
  }
  // Freshness is dynamic state: it is refreshed here and never restored.
  state_->evidence_confirmation[observation.resource] = state_->boot.id;
  return record;
}

// ---------------------------------------------------------------------------
// Authorization
// ---------------------------------------------------------------------------

Result<AuthorizationRecord> ContainmentRuntime::authorize() {
  const std::lock_guard<std::mutex> guard(state_->lock);
  const std::uint64_t now = state_->clock->now_millis();
  while (!state_->authorizations.empty() &&
         state_->authorizations.front().record.expires_at_millis <= now) {
    state_->authorizations.pop_front();
  }
  if (state_->authorizations.size() >= state_->config.max_active_authorizations) {
    return Status{StatusCode::Exhausted, "authorization table is full"};
  }
  state_->authorization_counter = Sequence{state_->authorization_counter.value() + 1};
  AuthorizationRecord record;
  record.id = AuthorizationId{state_->authorization_counter.value()};
  record.authority = state_->current_authority();
  record.issued_sequence = state_->sequence;
  record.issued_at_millis = now;
  record.expires_at_millis = now + state_->config.authorization_lease_millis;
  record.allows_apply = true;

  RuntimeState::ActiveAuthorization entry;
  entry.record = record;
  state_->authorizations.push_back(entry);
  return record;
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

Result<model::ContainmentPlan> ContainmentRuntime::plan(const PlanRequest& request) {
  const std::lock_guard<std::mutex> guard(state_->lock);
  const Result<AuthorizationRecord> authorization =
      validate_authorization(*state_, request.authorization, false);
  if (!authorization.ok()) {
    ++state_->stats.plans_rejected;
    return authorization.status();
  }
  const StatusResult has_topology = state_->ensure_topology();
  if (!has_topology.ok()) {
    ++state_->stats.plans_rejected;
    return has_topology.status();
  }

  engine::PlanInputs inputs;
  inputs.topology = &state_->topology.value();
  inputs.graph = state_->graph.get();
  inputs.evidence = &state_->evidence;
  inputs.policy = &state_->policy;
  inputs.freshness.confirmations = nullptr;
  inputs.freshness.current_boot = state_->boot;
  inputs.freshness.require_current_boot_confirmation =
      state_->config.require_evidence_confirmation_in_current_boot;
  std::vector<std::pair<model::ResourceId, BootId>> confirmations;
  confirmations.reserve(state_->evidence_confirmation.size());
  for (const auto& entry : state_->evidence_confirmation) {
    confirmations.push_back(entry);
  }
  inputs.freshness.confirmations = &confirmations;
  // The boundary generation of a produced plan is the plan generation itself, so
  // a boundary generation always identifies the exact decision that produced it.
  inputs.boundary_generation = BoundaryGeneration{state_->plan_counter.value() + 1};

  const model::AuthorityVector authority = state_->current_authority();
  const PlanGeneration generation{state_->plan_counter.value() + 1};
  auto computed = engine::compute_plan(inputs, authority, generation);
  if (!computed.ok()) {
    ++state_->stats.plans_rejected;
    return computed.status();
  }
  model::ContainmentPlan plan = std::move(computed.value().plan);

  if (state_->evidence_conflict) {
    plan.currency = model::EvidenceCurrency::Conflicting;
    if (plan.claim != model::ContainmentClaim::Invalid &&
        plan.claim != model::ContainmentClaim::Unsupported) {
      plan.claim = model::ContainmentClaim::Indeterminate;
    }
    add_reason(plan.explanation, model::ReasonCode::EvidenceConflicting,
               "contradictory evidence was rejected for at least one resource");
    plan.seal();
  }

  const std::vector<std::byte> encoded = plan.encode();
  const VoidResult committed = state_->commit(store::RecordType::PlanDecision, encoded);
  if (!committed.ok()) {
    ++state_->stats.plans_rejected;
    return committed.status();
  }
  state_->plan_counter = generation;
  state_->plans.push_back(plan);
  state_->evict_bounded_tables();
  ++state_->stats.plans_computed;
  return plan;
}

Result<model::ContainmentPlan> ContainmentRuntime::plan_by_generation(PlanGeneration generation) const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  const model::ContainmentPlan* plan = state_->find_plan(generation);
  if (plan == nullptr) {
    return Status{StatusCode::NotFound, "plan generation is not retained"};
  }
  return *plan;
}

Result<model::ContainmentBoundary> ContainmentRuntime::current_boundary() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (!state_->boundary.has_value()) {
    return Status{StatusCode::NotFound, "no containment boundary is current"};
  }
  return state_->boundary.value();
}

model::EffectState ContainmentRuntime::current_effect_state() const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  return state_->effect_state;
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

Result<model::TransitionDecision> ContainmentRuntime::transition(
    const model::TransitionRequest& request) {
  const std::lock_guard<std::mutex> guard(state_->lock);

  state_->transition_counter = TransitionGeneration{state_->transition_counter.value() + 1};
  model::TransitionDecision decision;
  decision.generation = state_->transition_counter;
  decision.kind = request.kind;
  decision.plan_digest = request.plan_digest;
  decision.epoch = state_->epoch;
  decision.boot = state_->boot;

  auto fail = [&](model::TransitionStatus status, StatusCode code, model::ReasonCode reason,
                  std::string_view text) -> Result<model::TransitionDecision> {
    decision.status = status;
    decision.code = code;
    add_reason(decision.explanation, reason, text);
    decision.sequence = state_->sequence;
    const std::vector<std::byte> encoded = codec::encode_transition(decision);
    const VoidResult committed = state_->commit(store::RecordType::TransitionDecision, encoded);
    if (!committed.ok()) {
      return committed.status();
    }
    decision.sequence = state_->sequence;
    state_->transitions.push_back(decision);
    state_->evict_bounded_tables();
    ++state_->stats.transitions_denied;
    return decision;
  };

  const Result<AuthorizationRecord> authorization =
      validate_authorization(*state_, request.authorization, false);
  if (!authorization.ok()) {
    ++state_->stats.authority_rejections;
    const model::ReasonCode reason =
        authorization.status().code() == StatusCode::Fenced ? model::ReasonCode::AuthorityFenced
                                                            : model::ReasonCode::AuthorityStaleEpoch;
    return fail(model::TransitionStatus::Stale, authorization.status().code(), reason,
                authorization.status().message());
  }

  const model::ContainmentPlan* plan = state_->find_plan(request.plan);
  if (plan == nullptr) {
    return fail(model::TransitionStatus::Stale, StatusCode::NotFound,
                model::ReasonCode::TransitionSupersededByNewerPlan,
                "authorizing plan is not retained");
  }
  if (!(plan->digest() == request.plan_digest)) {
    return fail(model::TransitionStatus::Invalid, StatusCode::Conflict,
                model::ReasonCode::TransitionSupersededByNewerPlan,
                "plan digest does not match the retained plan");
  }

  decision.from_generation = state_->boundary.has_value() ? state_->boundary->generation()
                                                          : BoundaryGeneration{};
  decision.from_digest = state_->boundary.has_value() ? state_->boundary->digest() : Digest{};
  decision.observed_effect_state = state_->effect_state;

  if (!(decision.from_generation == request.expected_current_boundary)) {
    return fail(model::TransitionStatus::Stale, StatusCode::StaleGeneration,
                model::ReasonCode::TransitionStaleGeneration,
                "current boundary generation does not match the request");
  }
  if (request.expected_released != !state_->boundary.has_value()) {
    return fail(model::TransitionStatus::Stale, StatusCode::StaleGeneration,
                model::ReasonCode::TransitionStaleGeneration,
                "current boundary presence does not match the request");
  }

  if (state_->boundary.has_value()) {
    if (!model::is_verified_effect(state_->effect_state)) {
      decision.required_effect_state = model::EffectState::VerifiedApplied;
      return fail(model::TransitionStatus::Indeterminate, StatusCode::Ambiguous,
                  model::ReasonCode::TransitionDeniedUnverifiedEffect,
                  "current boundary effect is not verified");
    }
    decision.effect_verified = true;
  } else {
    decision.effect_verified = true;
  }

  switch (request.kind) {
    case model::TransitionKind::Expand: {
      if (state_->boundary.has_value() && !plan->boundary.is_superset_of(state_->boundary.value())) {
        return fail(model::TransitionStatus::Invalid, StatusCode::InvalidArgument,
                    model::ReasonCode::TransitionExpandNotASuperset,
                    "expansion must contain every current member");
      }
      break;
    }
    case model::TransitionKind::Contract: {
      if (!state_->boundary.has_value() ||
          !plan->boundary.is_subset_of(state_->boundary.value()) ||
          plan->boundary.same_members(state_->boundary.value())) {
        return fail(model::TransitionStatus::Invalid, StatusCode::InvalidArgument,
                    model::ReasonCode::TransitionContractNotASubset,
                    "contraction must be a proper subset of the current boundary");
      }
      break;
    }
    case model::TransitionKind::Release: {
      if (!plan->boundary.empty() || !plan->is_releasable()) {
        return fail(model::TransitionStatus::Denied, StatusCode::Denied,
                    model::ReasonCode::TransitionDeniedActiveFailures,
                    "release requires a proven containment plan with an empty boundary");
      }
      break;
    }
  }

  if (!(plan->generation > state_->boundary_plan) && state_->boundary_plan.value() != 0) {
    return fail(model::TransitionStatus::Stale, StatusCode::StaleGeneration,
                model::ReasonCode::TransitionSupersededByNewerPlan,
                "a newer boundary decision is already in force");
  }

  decision.status = model::TransitionStatus::Accepted;
  decision.code = StatusCode::Ok;
  decision.to_released = request.kind == model::TransitionKind::Release;
  decision.to_generation = decision.to_released ? BoundaryGeneration{}
                                                : plan->boundary.generation();
  decision.to_digest = plan->boundary.digest();
  decision.required_effect_state = decision.to_released ? model::EffectState::VerifiedReleased
                                                        : model::EffectState::VerifiedApplied;
  add_reason(decision.explanation, model::ReasonCode::TransitionAccepted,
             model::to_string(request.kind));

  const std::vector<std::byte> encoded = codec::encode_transition(decision);
  const VoidResult committed = state_->commit(store::RecordType::TransitionDecision, encoded);
  if (!committed.ok()) {
    return committed.status();
  }
  decision.sequence = state_->sequence;

  const std::vector<std::byte> boundary_payload =
      codec::encode_boundary_decision(plan->generation, plan->digest(), decision.to_released,
                                      decision.to_released
                                          ? std::span<const std::byte>{}
                                          : std::span<const std::byte>(plan->boundary.encode()));
  const VoidResult boundary_committed =
      state_->commit(store::RecordType::BoundaryDecision, boundary_payload);
  if (!boundary_committed.ok()) {
    return boundary_committed.status();
  }
  if (decision.to_released) {
    state_->boundary.reset();
  } else {
    state_->boundary = plan->boundary;
  }
  state_->boundary_plan = plan->generation;
  state_->boundary_plan_digest = plan->digest();
  state_->boundary_counter = state_->boundary.has_value()
                                 ? state_->boundary->generation()
                                 : BoundaryGeneration{state_->boundary_counter.value()};
  state_->effect_state = model::EffectState::NotRequested;
  state_->effect_requires_reverification = false;
  state_->transitions.push_back(decision);
  state_->evict_bounded_tables();
  ++state_->stats.transitions_accepted;
  return decision;
}

Result<model::TransitionDecision> ContainmentRuntime::transition_by_generation(
    TransitionGeneration generation) const {
  const std::lock_guard<std::mutex> guard(state_->lock);
  for (const model::TransitionDecision& decision : state_->transitions) {
    if (decision.generation == generation) {
      return decision;
    }
  }
  return Status{StatusCode::NotFound, "transition generation is not retained"};
}

// ---------------------------------------------------------------------------
// Checkpoint
// ---------------------------------------------------------------------------

VoidResult ContainmentRuntime::checkpoint() {
  const std::lock_guard<std::mutex> guard(state_->lock);
  if (state_->store == nullptr) {
    return Status{StatusCode::Unsupported, "runtime is not durable"};
  }
  codec::DurableState durable;
  durable.epoch = state_->epoch;
  durable.boot = state_->boot;
  durable.sequence = state_->sequence;
  durable.evidence_revision = state_->evidence_revision;
  durable.plan_counter = state_->plan_counter;
  durable.transition_counter = state_->transition_counter;
  durable.attempt_counter = state_->attempt_counter;
  durable.boundary_counter = state_->boundary_counter;
  durable.authorization_counter = state_->authorization_counter;
  durable.boundary_plan = state_->boundary_plan;
  durable.boundary_plan_digest = state_->boundary_plan_digest;
  durable.effect_state = static_cast<std::uint8_t>(state_->effect_state);
  if (state_->topology.has_value()) {
    durable.topology_bytes = state_->topology->encode();
  }
  durable.policy_bytes = state_->policy.encode();
  durable.evidence_bytes = state_->evidence.encode();
  if (state_->boundary.has_value()) {
    durable.boundary_bytes = state_->boundary->encode();
  }
  durable.plans.assign(state_->plans.begin(), state_->plans.end());
  durable.attempts.assign(state_->attempts.begin(), state_->attempts.end());
  durable.transitions.assign(state_->transitions.begin(), state_->transitions.end());
  durable.detections.assign(state_->detections.begin(), state_->detections.end());
  durable.fenced_boots = state_->fenced_boots;
  durable.active_authorizations = static_cast<std::uint32_t>(state_->authorizations.size());

  const std::vector<std::byte> payload = codec::encode_durable_state(durable);
  const VoidResult written = state_->store->write_snapshot(payload);
  if (!written.ok()) {
    return written.status();
  }
  ++state_->stats.snapshot_commits;
  state_->sequence = state_->store->last_sequence();
  return ok_result();
}

// ---------------------------------------------------------------------------
// JSON rendering
// ---------------------------------------------------------------------------

std::string to_json(const StartupReport& report) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("fresh", report.fresh);
  writer.member("recovered", report.recovered);
  writer.member("torn_tail_recovered", report.torn_tail_recovered);
  writer.member("torn_tail_bytes", report.torn_tail_bytes);
  writer.member("replayed_records", static_cast<std::uint64_t>(report.replayed_records));
  writer.member("skipped_records", static_cast<std::uint64_t>(report.skipped_records));
  writer.member("fenced_attempts", static_cast<std::uint64_t>(report.fenced_attempts));
  writer.member("fenced_authorizations", static_cast<std::uint64_t>(report.fenced_authorizations));
  writer.member("restored_plans", static_cast<std::uint64_t>(report.restored_plans));
  writer.member("restored_detections", static_cast<std::uint64_t>(report.restored_detections));
  writer.member("epoch", report.epoch.value());
  writer.member("boot_id", report.boot.id.value());
  writer.member("boot_pid", static_cast<std::uint64_t>(report.boot.process_id));
  writer.member("topology_generation", report.topology.value());
  writer.member("policy_generation", report.policy.value());
  writer.member("last_sequence", report.last_sequence.value());
  writer.member("snapshot_sequence", report.snapshot_sequence.value());
  writer.member("wal_segment", report.wal_segment.value());
  writer.key("explanation");
  writer.begin_array();
  for (const model::ExplanationItem& item : report.explanation.items()) {
    writer.begin_object();
    writer.member("code", model::to_string(item.code));
    writer.member("subject", item.subject.value());
    writer.member("value", item.value);
    writer.member("text", item.text);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

std::string to_json(const model::ContainmentPlan& plan) { return plan.to_json(); }

std::string to_json(const model::ApplyAttempt& attempt) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("plan_generation", attempt.id.plan.value());
  writer.member("attempt_sequence", attempt.id.sequence.value());
  writer.member("state", model::to_string(attempt.state));
  writer.member("plan_digest", attempt.plan_digest.hex());
  writer.member("boundary_digest", attempt.boundary_digest.hex());
  writer.member("boundary_generation", attempt.boundary_generation.value());
  writer.member("epoch", attempt.epoch.value());
  writer.member("boot_id", attempt.boot.id.value());
  writer.member("boot_pid", static_cast<std::uint64_t>(attempt.boot.process_id));
  writer.member("submitted_sequence", attempt.submitted_sequence.value());
  writer.member("applier_epoch", attempt.applier_epoch.value());
  writer.member("applier_boot_id", attempt.applier_boot.id.value());
  writer.member("applier_boot_pid", static_cast<std::uint64_t>(attempt.applier_boot.process_id));
  writer.member("observed_boundary_digest", attempt.observed_boundary_digest.hex());
  writer.member("submitted_at_millis", attempt.submitted_at_millis);
  writer.member("acknowledged_at_millis", attempt.acknowledged_at_millis);
  writer.member("verified_at_millis", attempt.verified_at_millis);
  writer.key("explanation");
  writer.begin_array();
  for (const model::ExplanationItem& item : attempt.explanation.items()) {
    writer.begin_object();
    writer.member("code", model::to_string(item.code));
    writer.member("subject", item.subject.value());
    writer.member("value", item.value);
    writer.member("text", item.text);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

std::string to_json(const AuthorizationRecord& authorization) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("authorization_id", authorization.id.value());
  writer.member("issued_sequence", authorization.issued_sequence.value());
  writer.member("issued_at_millis", authorization.issued_at_millis);
  writer.member("expires_at_millis", authorization.expires_at_millis);
  writer.member("allows_apply", authorization.allows_apply);
  writer.member("epoch", authorization.authority.epoch.value());
  writer.member("boot_id", authorization.authority.boot.id.value());
  writer.member("boot_pid", static_cast<std::uint64_t>(authorization.authority.boot.process_id));
  writer.member("policy_generation", authorization.authority.policy.value());
  writer.member("topology_generation", authorization.authority.topology.value());
  writer.member("evidence_revision", authorization.authority.evidence_revision.value());
  writer.member("evidence_digest", authorization.authority.evidence_digest.hex());
  writer.member("authority_digest", authorization.authority.digest().hex());
  writer.end_object();
  return writer.take();
}

std::string to_json(const RuntimeStats& stats) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("detections_accepted", stats.detections_accepted);
  writer.member("detections_unchanged", stats.detections_unchanged);
  writer.member("detections_rejected", stats.detections_rejected);
  writer.member("plans_computed", stats.plans_computed);
  writer.member("plans_rejected", stats.plans_rejected);
  writer.member("transitions_accepted", stats.transitions_accepted);
  writer.member("transitions_denied", stats.transitions_denied);
  writer.member("apply_attempts", stats.apply_attempts);
  writer.member("acknowledgements", stats.acknowledgements);
  writer.member("verifications", stats.verifications);
  writer.member("duplicate_completions_rejected", stats.duplicate_completions_rejected);
  writer.member("authority_rejections", stats.authority_rejections);
  writer.member("durable_commits", stats.durable_commits);
  writer.member("snapshot_commits", stats.snapshot_commits);
  writer.end_object();
  return writer.take();
}

}  // namespace fcfn::runtime
