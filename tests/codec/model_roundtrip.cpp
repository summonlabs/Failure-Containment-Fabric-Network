// FCFN codec suite: canonical round trips and total decoding.
//
// Product proposition proved here: a durable decision document survives the
// canonical codec byte-for-byte, and no damaged document is ever accepted as a
// partial or rounded-up object. In particular an UNKNOWN propagation edge stays
// UNKNOWN, a protected obligation stays protected, and a PROVEN_CONTAINMENT
// claim is never weakened or strengthened by a codec round trip.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "codec_support.hpp"

namespace {

using namespace fcfn::test::codec;

}  // namespace

FCFN_TEST(codec, topology_round_trips_with_evidence_classes_intact) {
  const auto& codec = topology_codec();
  const fcfn::model::Topology value = sample_topology();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().definition_digest() == value.definition_digest());
  FCFN_CHECK_EQ(decoded.value().node_count(), value.node_count());
  FCFN_CHECK_EQ(decoded.value().edge_count(), value.edge_count());
  // Canonical node order is lexicographic, independent of insertion order.
  FCFN_CHECK_EQ(decoded.value().resource(0).value(), std::string("alpha"));
  FCFN_CHECK_EQ(decoded.value().resource(3).value(), std::string("gamma"));
  // Weight, eligibility, protection, and adjacency completeness survive.
  // Canonical order is alpha, beta, delta, gamma.
  FCFN_CHECK_EQ(decoded.value().node(0).weight, static_cast<std::uint64_t>(3));
  FCFN_CHECK(decoded.value().node(0).containable);
  FCFN_CHECK(decoded.value().node(3).protected_obligation);
  FCFN_CHECK(!decoded.value().node(3).containable);
  FCFN_CHECK(decoded.value().node(1).completeness == fcfn::model::AdjacencyCompleteness::Partial);
  FCFN_CHECK(decoded.value().node(2).completeness == fcfn::model::AdjacencyCompleteness::Unknown);
  // Propagation evidence classes are preserved exactly: an UNKNOWN edge is never
  // rounded to PROVEN or REFUTED by serialization.
  FCFN_CHECK(decoded.value().edge_by_index(0).evidence == fcfn::model::EdgeEvidence::Proven);
  FCFN_CHECK(decoded.value().edge_by_index(1).evidence == fcfn::model::EdgeEvidence::Unknown);
  FCFN_CHECK(decoded.value().edge_by_index(2).evidence == fcfn::model::EdgeEvidence::Refuted);
  FCFN_CHECK_EQ(decoded.value().edge_by_index(1).generation.value(), static_cast<std::uint64_t>(12));
  FCFN_CHECK_EQ(decoded.value().generation().value(), static_cast<std::uint64_t>(7));
}

FCFN_TEST(codec, evidence_vector_round_trips_and_keeps_unknown_evidence_unknown) {
  const auto& codec = evidence_codec();
  const fcfn::model::EvidenceVector value = sample_evidence();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == value.digest());
  FCFN_CHECK_EQ(decoded.value().entries().size(), static_cast<std::size_t>(3));
  const fcfn::model::EvidenceEntry* gamma = decoded.value().find(rid("gamma"));
  FCFN_CHECK(gamma != nullptr);
  FCFN_CHECK(gamma->state == fcfn::model::EvidenceState::Unknown);
  FCFN_CHECK_EQ(gamma->generation.value(), static_cast<std::uint64_t>(3));
  // Unknown evidence is never reported as present: only alpha carries failure
  // evidence, so only alpha may appear in the present set.
  const std::vector<fcfn::model::ResourceId> present = decoded.value().present_resources();
  FCFN_CHECK_EQ(present.size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(present.front().value(), std::string("alpha"));
}

FCFN_TEST(codec, containment_policy_round_trips_every_bound) {
  const auto& codec = policy_codec();
  const fcfn::model::ContainmentPolicy value = sample_policy();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == value.digest());
  FCFN_CHECK(decoded.value().require_failure_source_inclusion ==
             value.require_failure_source_inclusion);
  FCFN_CHECK(decoded.value().allow_protected_inclusion == value.allow_protected_inclusion);
  FCFN_CHECK(decoded.value().minimize_protected_inclusions == value.minimize_protected_inclusions);
  FCFN_CHECK_EQ(decoded.value().max_boundary_weight, value.max_boundary_weight);
  FCFN_CHECK_EQ(decoded.value().max_boundary_members, value.max_boundary_members);
  FCFN_CHECK_EQ(decoded.value().exact_search_budget, value.exact_search_budget);
  FCFN_CHECK_EQ(decoded.value().heuristic_budget, value.heuristic_budget);
  FCFN_CHECK_EQ(decoded.value().exact_instance_node_limit, value.exact_instance_node_limit);
  FCFN_CHECK_EQ(decoded.value().generation.value(), static_cast<std::uint64_t>(2));
}

FCFN_TEST(codec, authority_vector_round_trips_every_binding) {
  const auto& codec = authority_codec();
  const fcfn::model::AuthorityVector value = sample_authority();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  // Every generation binding survives byte-exactly; a codec round trip can never
  // launder one incarnation's authority into another's.
  FCFN_CHECK(decoded.value().digest() == value.digest());
  FCFN_CHECK(decoded.value().epoch == value.epoch);
  FCFN_CHECK(decoded.value().boot == value.boot);
  FCFN_CHECK(decoded.value().policy == value.policy);
  FCFN_CHECK(decoded.value().topology == value.topology);
  FCFN_CHECK(decoded.value().evidence_revision == value.evidence_revision);
  FCFN_CHECK(decoded.value().evidence_digest == value.evidence_digest);
  FCFN_CHECK(decoded.value().issued_sequence == value.issued_sequence);
  // A decoded authority vector is only current when it also validates.
  const fcfn::model::AuthorityCheck check =
      fcfn::model::validate_authority(decoded.value(), value);
  FCFN_CHECK(check.ok());
}

FCFN_TEST(codec, containment_boundary_round_trips_members_and_witnesses) {
  const auto& codec = boundary_codec();
  const fcfn::model::ContainmentBoundary value = sample_boundary();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == value.digest());
  FCFN_CHECK_EQ(decoded.value().size(), static_cast<std::size_t>(2));
  FCFN_CHECK_EQ(decoded.value().total_weight(), static_cast<std::uint64_t>(8));
  FCFN_CHECK_EQ(decoded.value().protected_member_count(), static_cast<std::size_t>(0));
  const fcfn::model::BoundaryMember* source = decoded.value().find(rid("alpha"));
  FCFN_CHECK(source != nullptr);
  FCFN_CHECK(source->reason == fcfn::model::InclusionReason::FailureSource);
  const fcfn::model::BoundaryMember* cut = decoded.value().find(rid("beta"));
  FCFN_CHECK(cut != nullptr);
  FCFN_CHECK(cut->reason == fcfn::model::InclusionReason::CutVertexOnUnknownPath);
  FCFN_CHECK(cut->witness_uses_unknown);
  FCFN_CHECK_EQ(cut->witness_path.size(), static_cast<std::size_t>(2));
  FCFN_CHECK_EQ(cut->witness_path[1].value(), std::string("gamma"));
}

FCFN_TEST(codec, containment_plan_round_trips_claim_authority_and_digest) {
  const auto& codec = plan_codec();
  const fcfn::model::ContainmentPlan value = sample_plan();
  prove_canonical_codec(codec, value);

  const auto decoded = codec.decode(codec.encode(value));
  FCFN_CHECK_OK(decoded);
  FCFN_CHECK(decoded.value().digest() == value.digest());
  FCFN_CHECK(decoded.value().digest() == decoded.value().compute_digest());
  FCFN_CHECK(decoded.value().claim == fcfn::model::ContainmentClaim::ProvenContainment);
  FCFN_CHECK(decoded.value().feasibility == fcfn::model::FeasibilityStatus::ProvenFeasible);
  FCFN_CHECK(decoded.value().optimality == fcfn::model::OptimalityStatus::ProvenOptimal);
  FCFN_CHECK(decoded.value().currency == fcfn::model::EvidenceCurrency::Current);
  FCFN_CHECK(decoded.value().authority.digest() == value.authority.digest());
  FCFN_CHECK(decoded.value().boundary.digest() == value.boundary.digest());
  FCFN_CHECK(decoded.value().topology_digest == value.topology_digest);
  FCFN_CHECK(decoded.value().policy_digest == value.policy_digest);
  FCFN_CHECK(decoded.value().evidence_digest == value.evidence_digest);
  FCFN_CHECK(decoded.value().instance_digest == value.instance_digest);
  FCFN_CHECK_EQ(decoded.value().failure_sources.size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(decoded.value().failure_sources.front().value(), std::string("alpha"));
  FCFN_CHECK_EQ(decoded.value().protected_obligations.size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(decoded.value().proven_necessary_members.size(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(decoded.value().member_count, static_cast<std::size_t>(2));
  FCFN_CHECK_EQ(decoded.value().total_weight, static_cast<std::uint64_t>(8));
  FCFN_CHECK_EQ(decoded.value().explanation.size(), static_cast<std::size_t>(3));
  FCFN_CHECK(!decoded.value().is_releasable());
}

FCFN_TEST(codec, an_empty_document_is_never_a_valid_model) {
  const std::span<const std::byte> empty{};
  expect_status("Topology", topology_codec().decode(empty), StatusCode::InvalidArgument);
  expect_status("EvidenceVector", evidence_codec().decode(empty), StatusCode::InvalidArgument);
  expect_status("ContainmentPolicy", policy_codec().decode(empty), StatusCode::InvalidArgument);
  expect_status("AuthorityVector", authority_codec().decode(empty), StatusCode::InvalidArgument);
  expect_status("ContainmentBoundary", boundary_codec().decode(empty), StatusCode::InvalidArgument);
  expect_status("ContainmentPlan", plan_codec().decode(empty), StatusCode::InvalidArgument);
}
