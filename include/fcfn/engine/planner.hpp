// FCFN - containment planner.
//
// The planner is a pure function of topology, evidence, and policy. It produces
// a sealed plan carrying an explicit claim strength, deterministic explanations,
// and the full generation binding. It never mutates runtime state and never
// invents authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_ENGINE_PLANNER_HPP
#define FCFN_ENGINE_PLANNER_HPP

#include <utility>
#include <vector>

#include "fcfn/core/result.hpp"
#include "fcfn/engine/cut.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/model/policy.hpp"
#include "fcfn/model/topology.hpp"

namespace fcfn::engine {

/// Evidence freshness facts owned by the runtime, not by the evidence vector.
struct FreshnessView {
  /// Boot incarnation in which each Present-evidence resource was last confirmed.
  const std::vector<std::pair<model::ResourceId, BootId>>* confirmations{nullptr};
  BootIdentity current_boot{};
  /// When true, Present evidence not confirmed in the current boot degrades the
  /// containment claim to INDETERMINATE (conservative restart semantics).
  bool require_current_boot_confirmation{true};
};

struct PlanInputs {
  const model::Topology* topology{nullptr};
  const PropagationGraph* graph{nullptr};
  const model::EvidenceVector* evidence{nullptr};
  const model::ContainmentPolicy* policy{nullptr};
  FreshnessView freshness{};
  /// Boundary generation the runtime assigns to the produced boundary.
  BoundaryGeneration boundary_generation{};
};

struct PlanComputation {
  model::ContainmentPlan plan{};
  ContainmentProblem problem{};
  bool problem_valid{false};
};

/// Compute a containment plan. The authority vector and plan generation are
/// supplied by the caller (the runtime) so the planner stays free of state.
[[nodiscard]] Result<PlanComputation> compute_plan(const PlanInputs& inputs,
                                                   const model::AuthorityVector& authority,
                                                   PlanGeneration generation);

}  // namespace fcfn::engine

#endif  // FCFN_ENGINE_PLANNER_HPP
