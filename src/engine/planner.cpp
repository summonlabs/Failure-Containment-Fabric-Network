// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/engine/planner.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "fcfn/core/canonical.hpp"
#include "fcfn/engine/necessity.hpp"
#include "fcfn/engine/solver.hpp"

namespace fcfn::engine {
namespace {

/// Above this candidate count the planner skips necessity analysis (bounded work).
constexpr std::size_t kNecessityCandidateLimit = 64;

bool confirmed_in_current_boot(const FreshnessView& freshness, const model::ResourceId& resource) {
  if (freshness.confirmations == nullptr) {
    return false;
  }
  for (const auto& entry : *freshness.confirmations) {
    if (entry.first == resource) {
      return entry.second == freshness.current_boot.id;
    }
  }
  return false;
}

void add_reason(model::Explanation& explanation, model::ReasonCode code, std::string_view text) {
  // Explanation growth is bounded by the Explanation type itself, which records
  // truncation when the bound is reached.
  (void)explanation.add(code, text);
}

}  // namespace

Result<PlanComputation> compute_plan(const PlanInputs& inputs, const model::AuthorityVector& authority,
                                     PlanGeneration generation) {
  if (inputs.topology == nullptr || inputs.graph == nullptr || inputs.evidence == nullptr ||
      inputs.policy == nullptr) {
    return Status{StatusCode::InvalidArgument, "planner requires topology, graph, evidence, and policy"};
  }
  const model::Topology& topology = *inputs.topology;
  const model::ContainmentPolicy& policy = *inputs.policy;
  const model::EvidenceVector& evidence = *inputs.evidence;

  PlanComputation computation;
  model::ContainmentPlan& plan = computation.plan;
  plan.generation = generation;
  plan.authority = authority;
  plan.topology_digest = topology.definition_digest();
  plan.policy_digest = policy.digest();
  plan.evidence_digest = evidence.digest();

  // ---------------------------------------------------------------------
  // Sources and protected obligations
  // ---------------------------------------------------------------------
  std::vector<model::ResourceId> sources;
  std::vector<model::ResourceId> missing_sources;
  bool not_confirmed_since_restart = false;
  for (const model::EvidenceEntry& entry : evidence.entries()) {
    if (entry.state != model::EvidenceState::Present) {
      continue;
    }
    if (topology.find(entry.resource).has_value()) {
      sources.push_back(entry.resource);
    } else {
      missing_sources.push_back(entry.resource);
    }
    if (inputs.freshness.require_current_boot_confirmation &&
        !confirmed_in_current_boot(inputs.freshness, entry.resource)) {
      not_confirmed_since_restart = true;
    }
  }
  std::sort(sources.begin(), sources.end());
  std::sort(missing_sources.begin(), missing_sources.end());

  std::vector<model::ResourceId> obligations;
  for (std::size_t i = 0; i < topology.node_count(); ++i) {
    const model::NodeSpec& node = topology.node(static_cast<model::NodeIndex>(i));
    if (node.protected_obligation) {
      obligations.push_back(node.id);
    }
  }

  plan.failure_sources = sources;
  plan.protected_obligations = obligations;

  // ---------------------------------------------------------------------
  // Evidence currency
  // ---------------------------------------------------------------------
  model::EvidenceCurrency currency = model::EvidenceCurrency::Current;
  if (missing_sources.size() > 0) {
    currency = model::EvidenceCurrency::IncompleteFrontier;
  }
  if (not_confirmed_since_restart && currency == model::EvidenceCurrency::Current) {
    currency = model::EvidenceCurrency::Stale;
  }

  // ---------------------------------------------------------------------
  // No active failures: the containment requirement is satisfied vacuously and
  // the current boundary may be released.
  // ---------------------------------------------------------------------
  if (sources.empty() && missing_sources.empty()) {
    auto boundary = model::ContainmentBoundary::create(inputs.boundary_generation, {});
    if (!boundary.ok()) {
      return boundary.status();
    }
    plan.boundary = std::move(boundary.value());
    plan.claim = model::ContainmentClaim::ProvenContainment;
    plan.feasibility = model::FeasibilityStatus::ProvenFeasible;
    plan.optimality = model::OptimalityStatus::ProvenOptimal;
    plan.currency = model::EvidenceCurrency::Current;
    add_reason(plan.explanation, model::ReasonCode::NoFailureSources,
               "no authoritative failure evidence is current");
    add_reason(plan.explanation, model::ReasonCode::FeasibilityProven, "empty containment satisfies the policy");
    add_reason(plan.explanation, model::ReasonCode::OptimalityProven, "empty containment is minimal");
    {
      CanonicalWriter writer;
      writer.digest(topology.definition_digest());
      writer.boolean(true);
      plan.instance_digest = writer.digest();
    }
    plan.seal();
    return computation;
  }

  // ---------------------------------------------------------------------
  // Instance construction
  // ---------------------------------------------------------------------
  ContainmentInstanceSpec spec;
  spec.failure_sources = sources;
  spec.protected_obligations = obligations;
  spec.require_source_inclusion = policy.require_failure_source_inclusion;
  spec.allow_protected_inclusion = policy.allow_protected_inclusion;
  spec.minimize_protected_inclusions = policy.minimize_protected_inclusions;
  spec.max_total_weight = policy.max_boundary_weight;
  spec.max_members = policy.max_boundary_members;

  if (sources.empty()) {
    // Every failure source is outside the known topology: nothing can be
    // proven about propagation.
    auto boundary = model::ContainmentBoundary::create(inputs.boundary_generation, {});
    if (!boundary.ok()) {
      return boundary.status();
    }
    plan.boundary = std::move(boundary.value());
    plan.claim = model::ContainmentClaim::Indeterminate;
    plan.feasibility = model::FeasibilityStatus::Unknown;
    plan.optimality = model::OptimalityStatus::NotAttempted;
    plan.currency = model::EvidenceCurrency::IncompleteFrontier;
    for (const model::ResourceId& id : missing_sources) {
      plan.explanation.add_resource(model::ReasonCode::FailureSourceNotInTopology, id,
                                    "failure source is absent from the known topology");
    }
    plan.seal();
    return computation;
  }

  if (obligations.empty()) {
    // Nothing to protect: contain every eligible failure source and no more.
    std::vector<model::BoundaryMember> members;
    for (const model::ResourceId& id : sources) {
      const auto index = topology.find(id);
      if (!index.has_value()) {
        continue;
      }
      const model::NodeSpec& node = topology.node(*index);
      if (!node.containable) {
        plan.uncontainable_sources.push_back(id);
        continue;
      }
      model::BoundaryMember member;
      member.resource = id;
      member.reason = model::InclusionReason::FailureSource;
      member.weight = node.weight;
      members.push_back(std::move(member));
    }
    auto boundary = model::ContainmentBoundary::create(inputs.boundary_generation, std::move(members));
    if (!boundary.ok()) {
      return boundary.status();
    }
    plan.boundary = std::move(boundary.value());
    plan.total_weight = plan.boundary.total_weight();
    plan.member_count = plan.boundary.size();
    plan.currency = currency;
    add_reason(plan.explanation, model::ReasonCode::NoProtectedObligations,
               "no protected obligations are declared for this topology");
    add_reason(plan.explanation, model::ReasonCode::OptimalityProven,
               "only mandatory failure sources are contained");
    const bool sources_ok =
        !policy.require_failure_source_inclusion || plan.uncontainable_sources.empty();
    if (sources_ok && currency == model::EvidenceCurrency::Current) {
      plan.claim = model::ContainmentClaim::ProvenContainment;
      plan.feasibility = model::FeasibilityStatus::ProvenFeasible;
      plan.optimality = model::OptimalityStatus::ProvenOptimal;
    } else {
      plan.claim = model::ContainmentClaim::Indeterminate;
      plan.feasibility = currency == model::EvidenceCurrency::Current
                             ? model::FeasibilityStatus::ProvenFeasible
                             : model::FeasibilityStatus::Unknown;
      plan.optimality = model::OptimalityStatus::ProvenOptimal;
      for (const model::ResourceId& id : plan.uncontainable_sources) {
        plan.explanation.add_resource(model::ReasonCode::SourceNotContainable, id,
                                      "failure source is not eligible for containment");
      }
      if (currency != model::EvidenceCurrency::Current) {
        add_reason(plan.explanation, model::ReasonCode::IncompleteAdjacencyOnFrontier,
                   "evidence currency is not current");
      }
    }
    plan.seal();
    return computation;
  }

  const Result<ContainmentProblem> built = ContainmentProblem::build(*inputs.graph, spec);
  if (!built.ok()) {
    return built.status();
  }
  computation.problem = built.value();
  computation.problem_valid = true;
  const ContainmentProblem& problem = computation.problem;
  plan.instance_digest = problem.instance_digest();

  // ---------------------------------------------------------------------
  // Frontier completeness: an incomplete adjacency anywhere on the propagation
  // frontier means paths may be missing, so no containment can be proven.
  // ---------------------------------------------------------------------
  std::size_t incomplete_frontier = 0;
  std::size_t unknown_edges_on_frontier = 0;
  for (const model::NodeIndex node : problem.frontier()) {
    if (topology.node(node).completeness != model::AdjacencyCompleteness::Complete) {
      ++incomplete_frontier;
    }
    if (inputs.graph->touches_unknown_edge(node)) {
      ++unknown_edges_on_frontier;
    }
  }
  if (incomplete_frontier > 0 && currency == model::EvidenceCurrency::Current) {
    currency = model::EvidenceCurrency::IncompleteFrontier;
    plan.explanation.add_value(model::ReasonCode::IncompleteAdjacencyOnFrontier,
                               static_cast<std::uint64_t>(incomplete_frontier),
                               "adjacency information is incomplete on the propagation frontier");
  }
  if (unknown_edges_on_frontier > 0) {
    plan.explanation.add_value(model::ReasonCode::UnknownEdgesPresent,
                               static_cast<std::uint64_t>(inputs.graph->unknown_edge_count()),
                               "UNKNOWN edges are conservatively treated as present");
  }
  plan.currency = currency;

  // ---------------------------------------------------------------------
  // Solve
  // ---------------------------------------------------------------------
  SolveBudget budget;
  budget.max_explored_nodes = policy.exact_search_budget;
  budget.max_iterations = policy.heuristic_budget;
  const CutSolution solution = solve(problem, budget, policy.exact_instance_node_limit);
  plan.counters.candidate_nodes = solution.counters.candidate_nodes;
  plan.counters.explored_nodes = solution.counters.explored_nodes;
  plan.counters.pruned_branches = solution.counters.pruned_branches;
  plan.counters.residual_paths_checked = solution.counters.residual_paths_checked;
  plan.counters.budget = solution.counters.budget;
  plan.counters.budget_exhausted = solution.counters.budget_exhausted;
  plan.counters.heuristic_used = solution.counters.heuristic_used;
  if (solution.counters.heuristic_used) {
    add_reason(plan.explanation, model::ReasonCode::HeuristicConstructionUsed,
               "instance exceeds the exact-search node limit");
  }

  for (const model::NodeIndex source : problem.uncontainable_sources()) {
    const model::ResourceId& id = problem.resource_ids()[source];
    plan.uncontainable_sources.push_back(id);
    plan.explanation.add_resource(model::ReasonCode::SourceNotContainable, id,
                                  "failure source is not eligible for containment");
  }

  switch (solution.status) {
    case SolveStatus::ProvenInfeasible: {
      plan.feasibility = model::FeasibilityStatus::ProvenInfeasible;
      plan.optimality = model::OptimalityStatus::NotApplicable;
      plan.claim = model::ContainmentClaim::ProvenInfeasible;
      plan.infeasibility.present = solution.has_infeasibility_witness;
      for (const model::NodeIndex node : solution.infeasibility_path) {
        plan.infeasibility.path.push_back(problem.resource_ids()[node]);
      }
      if (solution.has_infeasibility_witness) {
        add_reason(plan.explanation, model::ReasonCode::InfeasibleCertificateIneligiblePath,
                   "a source-to-obligation path carries no eligible containment member");
      } else {
        add_reason(plan.explanation, model::ReasonCode::InfeasibleByExhaustiveSearch,
                   "exhaustive search found no feasible containment set");
      }
      break;
    }
    case SolveStatus::LimitReachedNoSolution: {
      plan.feasibility = model::FeasibilityStatus::Unknown;
      plan.optimality = model::OptimalityStatus::NotAttempted;
      plan.claim = model::ContainmentClaim::Indeterminate;
      add_reason(plan.explanation, model::ReasonCode::SearchLimitReached,
                 "search ended without a feasible containment set");
      add_reason(plan.explanation, model::ReasonCode::NoFeasibleSolutionFound,
                 "feasibility is unknown; this is not an infeasibility proof");
      break;
    }
    case SolveStatus::InvalidProblem: {
      plan.feasibility = model::FeasibilityStatus::Unknown;
      plan.optimality = model::OptimalityStatus::NotAttempted;
      plan.claim = model::ContainmentClaim::Invalid;
      add_reason(plan.explanation, model::ReasonCode::TopologyEmpty, "instance could not be built");
      break;
    }
    case SolveStatus::ProvenOptimal:
    case SolveStatus::FeasibleNotProvenOptimal: {
      std::vector<model::NodeIndex> members = solution.members;

      // Minimality restoration keeps only load-bearing members, so every
      // non-source member has a witness path through it.
      std::sort(members.begin(), members.end());
      for (std::size_t i = 0; i < members.size();) {
        const model::NodeIndex candidate = members[i];
        if (problem.is_source(candidate) && problem.require_source_inclusion()) {
          ++i;
          continue;
        }
        std::vector<model::NodeIndex> trial;
        trial.reserve(members.size() - 1);
        for (const model::NodeIndex member : members) {
          if (member != candidate) {
            trial.push_back(member);
          }
        }
        if (verify_cut(problem, trial).ok()) {
          members = std::move(trial);
        } else {
          ++i;
        }
      }

      std::vector<model::BoundaryMember> boundary_members;
      boundary_members.reserve(members.size());
      for (const model::NodeIndex member : members) {
        model::BoundaryMember entry;
        entry.resource = problem.resource_ids()[member];
        entry.weight = problem.weight(member);
        entry.protected_obligation = problem.is_protected(member) != 0;
        if (problem.is_source(member)) {
          entry.reason = model::InclusionReason::FailureSource;
        } else {
          entry.reason = model::InclusionReason::CutVertexOnProvenPath;
        }
        // Witness: a shortest source-to-obligation path that survives removing
        // every other member. It must pass through this member, otherwise the
        // member would not be load-bearing.
        std::vector<model::NodeIndex> others;
        others.reserve(members.size());
        for (const model::NodeIndex other : members) {
          if (other != member) {
            others.push_back(other);
          }
        }
        bool uses_unknown = false;
        const std::vector<model::NodeIndex> path =
            find_witness_path(problem, others, &uses_unknown);
        if (!path.empty() && std::find(path.begin(), path.end(), member) != path.end()) {
          entry.witness_path.reserve(path.size());
          for (const model::NodeIndex step : path) {
            entry.witness_path.push_back(problem.resource_ids()[step]);
          }
          entry.witness_uses_unknown = uses_unknown;
          if (!problem.is_source(member)) {
            entry.reason = uses_unknown ? model::InclusionReason::CutVertexOnUnknownPath
                                        : model::InclusionReason::CutVertexOnProvenPath;
          }
        }
        boundary_members.push_back(std::move(entry));
      }

      auto boundary = model::ContainmentBoundary::create(inputs.boundary_generation,
                                                         std::move(boundary_members));
      if (!boundary.ok()) {
        return boundary.status();
      }
      plan.boundary = std::move(boundary.value());
      plan.total_weight = plan.boundary.total_weight();
      plan.member_count = plan.boundary.size();
      plan.feasibility = model::FeasibilityStatus::ProvenFeasible;

      const bool optimal = solution.status == SolveStatus::ProvenOptimal;
      plan.optimality = optimal ? model::OptimalityStatus::ProvenOptimal
                                : model::OptimalityStatus::BoundedNotProven;
      add_reason(plan.explanation, model::ReasonCode::FeasibilityProven,
                 "an independent verifier accepted the containment set");
      if (optimal) {
        add_reason(plan.explanation, model::ReasonCode::OptimalityProven,
                   "exhaustive search proved the objective is minimal");
      } else {
        add_reason(plan.explanation, model::ReasonCode::BoundedSolutionNotMinimal,
                   "search budget ended before optimality was proven");
        add_reason(plan.explanation, model::ReasonCode::SearchLimitReached,
                   "optimality is not claimed for this plan");
      }
      if (!solution.counters.heuristic_used) {
        add_reason(plan.explanation, model::ReasonCode::LocalMinimalityRestored,
                   "each member is load-bearing");
      }

      const bool source_inclusion_required = policy.require_failure_source_inclusion;
      if (currency != model::EvidenceCurrency::Current) {
        plan.claim = model::ContainmentClaim::Indeterminate;
        if (currency == model::EvidenceCurrency::Stale) {
          add_reason(plan.explanation, model::ReasonCode::EvidenceNotConfirmedSinceRestart,
                     "present evidence has not been confirmed in this boot");
        } else {
          add_reason(plan.explanation, model::ReasonCode::IncompleteAdjacencyOnFrontier,
                     "adjacency completeness prevents a containment claim");
        }
      } else if (source_inclusion_required && !plan.uncontainable_sources.empty()) {
        // The propagation cut is proven, but a policy requirement that every
        // eligible failure source be contained cannot be satisfied.
        plan.claim = model::ContainmentClaim::Indeterminate;
        add_reason(plan.explanation, model::ReasonCode::SourceNotContainable,
                   "a required failure source cannot be contained");
      } else if (!optimal) {
        plan.claim = model::ContainmentClaim::ProvenFeasibleNotMinimal;
      } else {
        plan.claim = model::ContainmentClaim::ProvenContainment;
      }

      // Necessity analysis, bounded by candidate count.
      if (problem.candidates().size() <= kNecessityCandidateLimit) {
        SolveBudget necessity_budget = budget;
        const NecessityResult necessity = analyze_necessity(problem, members, necessity_budget);
        for (const model::NodeIndex node : necessity.proven_necessary) {
          plan.proven_necessary_members.push_back(problem.resource_ids()[node]);
        }
        for (const model::NodeIndex node : necessity.indeterminate) {
          plan.indeterminate_necessity.push_back(problem.resource_ids()[node]);
        }
      }
      break;
    }
  }

  plan.seal();
  return computation;
}

}  // namespace fcfn::engine
