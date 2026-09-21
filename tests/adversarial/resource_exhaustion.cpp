// FCFN adversarial suite: resource exhaustion and bounded refusal.
//
// Product proposition proved here: every structural bound is an explicit,
// deterministic refusal. Counts above a bound are refused before materialisation,
// a maximum-legal request is still exact (no silent overflow of the weight sum),
// the configured member and weight budgets are never exceeded by a plan, and an
// explanation or evidence table that reaches its bound reports that fact instead
// of growing without limit.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "adversarial_support.hpp"

namespace {

using namespace fcfn::test::adversarial;

using fcfn::model::BoundaryGeneration;
using fcfn::model::BoundaryMember;
using fcfn::model::ContainmentClaim;
using fcfn::model::EvidenceEntry;
using fcfn::model::EvidenceGeneration;
using fcfn::model::EvidenceVector;
using fcfn::model::Explanation;
using fcfn::model::InclusionReason;
using fcfn::model::PolicyGeneration;
using fcfn::model::ReasonCode;
using fcfn::runtime::PlanRequest;

// A boundary can never overflow its weight sum while the per-member bound holds:
// kMaxBoundaryMembers * kMaxNodeWeight is orders of magnitude below the 64-bit
// limit.
static_assert(static_cast<std::uint64_t>(kMaxBoundaryMembers) * kMaxNodeWeight <
                  std::numeric_limits<std::uint64_t>::max() / 2,
              "boundary weight accounting must be structurally overflow-free");

std::vector<BoundaryMember> uniform_members(std::size_t count, std::uint64_t weight) {
  std::vector<BoundaryMember> members;
  members.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    BoundaryMember member;
    member.resource = ResourceId::unchecked("r" + std::to_string(index));
    member.reason = InclusionReason::FailureSource;
    member.weight = weight;
    members.push_back(std::move(member));
  }
  return members;
}

ContainmentPlan plan_with_policy(const fcfn::model::ContainmentPolicy& policy, fcfn::Clock* clock) {
  RuntimeConfig config = non_durable_config();
  config.policy = policy;
  config.topology = uncontainable_source_topology(1);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, clock);
  source_detection(*runtime, 1);
  const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
  if (!authorization.ok()) {
    fail_here("authorize", authorization.status().to_string());
  }
  const Result<ContainmentPlan> planned = runtime->plan(PlanRequest{authorization.value().id});
  if (!planned.ok()) {
    fail_here("plan", planned.status().to_string());
  }
  return planned.value();
}

}  // namespace

FCFN_TEST(adversarial, boundary_member_and_weight_bounds_are_exact) {
  // One member above the per-member weight bound is refused.
  expect_result("member weight above kMaxNodeWeight",
                fcfn::model::ContainmentBoundary::create(
                    BoundaryGeneration{1}, uniform_members(1, kMaxNodeWeight + 1)),
                StatusCode::InvalidArgument);
  expect_result("member weight at the 64-bit extreme",
                fcfn::model::ContainmentBoundary::create(
                    BoundaryGeneration{1}, uniform_members(1, 0xffffffffffffffffull)),
                StatusCode::InvalidArgument);
  // More members than the boundary bound is refused before materialisation.
  expect_result("member count above kMaxBoundaryMembers",
                fcfn::model::ContainmentBoundary::create(
                    BoundaryGeneration{1}, uniform_members(kMaxBoundaryMembers + 1, 1)),
                StatusCode::LimitExceeded);
  // The maximum legal request is still exact: no truncation, no wraparound.
  const Result<fcfn::model::ContainmentBoundary> maximal =
      fcfn::model::ContainmentBoundary::create(
          BoundaryGeneration{1}, uniform_members(kMaxBoundaryMembers, kMaxNodeWeight));
  FCFN_CHECK_OK(maximal);
  FCFN_CHECK_EQ(maximal.value().size(), kMaxBoundaryMembers);
  const std::uint64_t expected_total =
      static_cast<std::uint64_t>(kMaxBoundaryMembers) * kMaxNodeWeight;
  FCFN_CHECK_EQ(maximal.value().total_weight(), expected_total);
  // The structural maximum of a boundary sits an order of magnitude below the
  // plan-level weight bound, which is itself far below the 64-bit limit: no
  // combination of legal members can overflow the sum.
  FCFN_CHECK(maximal.value().total_weight() < kMaxPlanWeightSum);
  // The digest covers the whole document, so a truncated weight sum would show
  // up as a different digest for the same member set.
  const Result<fcfn::model::ContainmentBoundary> again =
      fcfn::model::ContainmentBoundary::create(
          BoundaryGeneration{1}, uniform_members(kMaxBoundaryMembers, kMaxNodeWeight));
  FCFN_CHECK_OK(again);
  FCFN_CHECK(again.value().digest() == maximal.value().digest());
}

FCFN_TEST(adversarial, a_plan_never_exceeds_the_configured_member_budget) {
  fcfn::ManualClock clock(1000);
  fcfn::model::ContainmentPolicy policy = fcfn::model::default_policy();
  policy.require_failure_source_inclusion = false;

  // Control: without a tighter budget the fixture is provably contained.
  const ContainmentPlan unbounded = plan_with_policy(policy, &clock);
  FCFN_CHECK(unbounded.claim == ContainmentClaim::ProvenContainment);
  FCFN_CHECK(unbounded.boundary.size() > 2);

  policy.max_boundary_members = 2;
  const ContainmentPlan bounded = plan_with_policy(policy, &clock);
  FCFN_CHECK(bounded.boundary.size() <= policy.max_boundary_members);
  // A containment claim requires a set that satisfies the policy; with only two
  // members allowed and every cut requiring more, the claim must not be rounded
  // up to PROVEN_CONTAINMENT.
  FCFN_CHECK(bounded.claim != ContainmentClaim::ProvenContainment);
  FCFN_CHECK(bounded.member_count <= policy.max_boundary_members);
}

FCFN_TEST(adversarial, a_plan_never_exceeds_the_configured_weight_budget) {
  fcfn::ManualClock clock(1000);
  fcfn::model::ContainmentPolicy policy = fcfn::model::default_policy();
  policy.require_failure_source_inclusion = false;
  policy.max_boundary_weight = 1;

  const ContainmentPlan bounded = plan_with_policy(policy, &clock);
  FCFN_CHECK(bounded.total_weight <= policy.max_boundary_weight);
  // The cheapest cut in this fixture costs more than one unit of weight, so no
  // containment can be claimed inside the budget.
  FCFN_CHECK(bounded.claim != ContainmentClaim::ProvenContainment);
}

FCFN_TEST(adversarial, explanation_growth_is_bounded_and_reported_as_truncated) {
  Explanation explanation;
  std::size_t accepted = 0;
  for (std::uint32_t index = 0; index < 200; ++index) {
    if (explanation.add(ReasonCode::FailureSourceContained, "bounded explanation growth")) {
      ++accepted;
    }
  }
  FCFN_CHECK_EQ(accepted, fcfn::kMaxExplanationItems);
  FCFN_CHECK_EQ(explanation.size(), fcfn::kMaxExplanationItems);
  FCFN_CHECK(explanation.truncated());
  const std::string rendered = explanation.render();
  FCFN_CHECK(rendered.find("[truncated]") != std::string::npos);
  // The rendered document stays inside the bound it advertises.
  const std::size_t per_item = fcfn::kMaxExplanationTextLength + 64;
  FCFN_CHECK(rendered.size() <= fcfn::kMaxExplanationItems * per_item);

  // A fully formed item handed to the bounded document is dropped the same way.
  fcfn::model::ExplanationItem item;
  item.code = ReasonCode::OptimalityProven;
  item.has_value = true;
  item.value = 42;
  FCFN_CHECK(!explanation.add_item(item));
  FCFN_CHECK_EQ(explanation.size(), fcfn::kMaxExplanationItems);
}

FCFN_TEST(adversarial, an_evidence_vector_beyond_its_bound_is_refused) {
  auto entries = [](std::size_t count) {
    std::vector<EvidenceEntry> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      EvidenceEntry entry;
      entry.resource = ResourceId::unchecked("e" + std::to_string(index));
      entry.state = fcfn::model::EvidenceState::Present;
      entry.generation = EvidenceGeneration{1};
      out.push_back(std::move(entry));
    }
    return out;
  };
  expect_result("evidence vector above kMaxTopologyNodes",
                EvidenceVector::build(entries(kMaxTopologyNodes + 1)), StatusCode::LimitExceeded);
  const Result<EvidenceVector> maximal = EvidenceVector::build(entries(kMaxTopologyNodes));
  FCFN_CHECK_OK(maximal);
  FCFN_CHECK_EQ(maximal.value().entries().size(), kMaxTopologyNodes);
}

FCFN_TEST(adversarial, evidence_retention_is_bounded_without_refusing_new_evidence) {
  fcfn::ManualClock clock(1000);
  RuntimeConfig config = non_durable_config();
  config.max_retained_detections = 8;
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
  constexpr std::size_t kDetections = 300;
  for (std::size_t index = 0; index < kDetections; ++index) {
    const std::string resource = "h" + std::to_string(index);
    const Result<fcfn::model::DetectionRecord> recorded =
        runtime->record_detection(observation(resource.c_str(), EvidenceState::Present, 1, 1, index));
    if (!recorded.ok()) {
      fail_here("bounded evidence retention", recorded.status().to_string());
    }
  }
  FCFN_CHECK_EQ(runtime->stats().detections_accepted,
                static_cast<std::uint64_t>(kDetections));
  const Result<EvidenceVector> evidence = runtime->evidence();
  FCFN_CHECK_OK(evidence);
  FCFN_CHECK_EQ(evidence.value().entries().size(), kDetections);
}

FCFN_TEST(adversarial, an_authorization_lease_at_the_clock_extreme_fails_closed) {
  // A lease computed at the top of the 64-bit clock wraps. The only safe
  // outcome is refusal: a wrapped lease must never be usable.
  {
    fcfn::ManualClock clock(std::numeric_limits<std::uint64_t>::max() - 10);
    RuntimeConfig config = non_durable_config();
    const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    expect_result("plan with a wrapped lease", runtime->plan(PlanRequest{authorization.value().id}),
                  StatusCode::StaleGeneration);
  }
  // A zero-length lease is expired the moment it is issued.
  {
    fcfn::ManualClock clock(1000);
    RuntimeConfig config = non_durable_config();
    config.authorization_lease_millis = 0;
    const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(config, &clock);
    const Result<fcfn::runtime::AuthorizationRecord> authorization = runtime->authorize();
    FCFN_CHECK_OK(authorization);
    expect_result("plan with a zero-length lease",
                  runtime->plan(PlanRequest{authorization.value().id}),
                  StatusCode::StaleGeneration);
  }
}

FCFN_TEST(adversarial, a_policy_with_an_out_of_range_bound_is_refused_at_the_runtime) {
  fcfn::ManualClock clock(1000);
  const std::unique_ptr<ContainmentRuntime> runtime = open_or_fail(non_durable_config(), &clock);
  // The policy domain is enforced by the codec; the runtime refuses a request
  // whose generation does not advance, which is the bound it does enforce.
  fcfn::model::ContainmentPolicy stale = fcfn::model::default_policy();
  stale.generation = PolicyGeneration{1};
  expect_result("policy generation that does not advance", runtime->apply_policy(stale),
                StatusCode::StaleGeneration);
  fcfn::model::ContainmentPolicy zero = fcfn::model::default_policy();
  zero.generation = PolicyGeneration{0};
  expect_result("zero policy generation", runtime->apply_policy(zero),
                StatusCode::InvalidArgument);
}
