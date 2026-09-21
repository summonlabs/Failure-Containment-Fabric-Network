// FCFN codec suite support: canonical samples and the shared proof driver.
//
// Every sample is built from explicit literals so a failure is reproducible from
// the test name alone. The driver proves, for one model type at a time:
//   * decode(encode(value)) succeeds and re-encodes to the identical bytes
//   * decoding is total: every strict prefix of a valid document is refused
//   * appending bytes is refused as TrailingGarbage, never absorbed
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_CODEC_SUPPORT_HPP
#define FCFN_TEST_CODEC_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/ids.hpp"
#include "fcfn/model/authority.hpp"
#include "fcfn/model/boundary.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/explanation.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/model/policy.hpp"
#include "fcfn/model/topology.hpp"
#include "test_harness.hpp"

namespace fcfn::test::codec {

// Names the suite refers to unqualified from using-directive scopes.
using fcfn::CanonicalWriter;
using fcfn::kMaxBoundaryMembers;
using fcfn::kMaxCutCertificatePathNodes;
using fcfn::kMaxExplanationItems;
using fcfn::kMaxFailureSources;
using fcfn::kMaxNodeWeight;
using fcfn::kMaxPlanWeightSum;
using fcfn::kMaxRecordPayloadBytes;
using fcfn::kMaxTopologyNodes;
using fcfn::Result;
using fcfn::StatusCode;

using fcfn::model::AdjacencyCompleteness;
using fcfn::model::BoundaryGeneration;
using fcfn::model::BoundaryMember;
using fcfn::model::ContainmentBoundary;
using fcfn::model::ContainmentClaim;
using fcfn::model::ContainmentPlan;
using fcfn::model::ContainmentPolicy;
using fcfn::model::CoordinatorEpoch;
using fcfn::model::Digest;
using fcfn::model::EdgeEvidence;
using fcfn::model::EvidenceCurrency;
using fcfn::model::EvidenceEntry;
using fcfn::model::EvidenceGeneration;
using fcfn::model::EvidenceRevision;
using fcfn::model::EvidenceState;
using fcfn::model::EvidenceVector;
using fcfn::model::FeasibilityStatus;
using fcfn::model::InclusionReason;
using fcfn::model::NodeSpec;
using fcfn::model::OptimalityStatus;
using fcfn::model::PlanGeneration;
using fcfn::model::PolicyGeneration;
using fcfn::model::ResourceId;
using fcfn::model::kMaxReasonCodeValue;
using fcfn::model::SearchCounters;
using fcfn::model::Topology;
using fcfn::model::TopologyGeneration;
using fcfn::model::TopologySpec;

inline ResourceId rid(const char* text) { return ResourceId::unchecked(text); }

inline void require(const char* what, const VoidResult& result) {
  if (!result.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("sample ") + what + " failed: " + result.status().to_string());
  }
}

template <class T>
T require_value(const char* what, Result<T> result) {
  if (!result.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("sample ") + what + " failed: " + result.status().to_string());
  }
  return std::move(result.value());
}

inline std::string hex_of(std::span<const std::byte> bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::byte value : bytes) {
    const auto raw = static_cast<std::uint8_t>(value);
    out.push_back(kDigits[raw >> 4]);
    out.push_back(kDigits[raw & 0x0fu]);
  }
  return out;
}

inline bool same_bytes(std::span<const std::byte> left, std::span<const std::byte> right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i] != right[i]) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Sample documents
// ---------------------------------------------------------------------------

/// Topology with PROVEN, UNKNOWN, and REFUTED propagation evidence, a partial
/// and an unknown adjacency frontier, and one protected obligation.
inline Topology sample_topology() {
  TopologySpec spec;
  spec.generation = TopologyGeneration{7};
  auto node = [&spec](const char* id, bool containable, std::uint64_t weight, bool protected_node,
                      AdjacencyCompleteness completeness) {
    NodeSpec entry;
    entry.id = rid(id);
    entry.containable = containable;
    entry.weight = weight;
    entry.protected_obligation = protected_node;
    entry.completeness = completeness;
    spec.nodes.push_back(std::move(entry));
  };
  auto edge = [&spec](const char* from, const char* to, EdgeEvidence evidence, std::uint64_t generation) {
    fcfn::model::EdgeSpec entry;
    entry.from = rid(from);
    entry.to = rid(to);
    entry.evidence = evidence;
    entry.generation = EvidenceGeneration{generation};
    spec.edges.push_back(std::move(entry));
  };
  node("alpha", true, 3, false, AdjacencyCompleteness::Complete);
  node("beta", false, 5, false, AdjacencyCompleteness::Partial);
  node("gamma", false, 2, true, AdjacencyCompleteness::Complete);
  node("delta", true, 1, false, AdjacencyCompleteness::Unknown);
  edge("alpha", "beta", EdgeEvidence::Proven, 11);
  edge("beta", "gamma", EdgeEvidence::Unknown, 12);
  edge("delta", "gamma", EdgeEvidence::Refuted, 13);
  return require_value("topology", Topology::build(std::move(spec)));
}

inline EvidenceVector sample_evidence() {
  std::vector<EvidenceEntry> entries;
  auto entry = [](const char* resource, EvidenceState state, std::uint64_t generation,
                  std::uint64_t hi, std::uint64_t lo) {
    EvidenceEntry value;
    value.resource = rid(resource);
    value.state = state;
    value.generation = EvidenceGeneration{generation};
    value.source_digest = Digest{hi, lo};
    return value;
  };
  entries.push_back(entry("alpha", EvidenceState::Present, 1, 0x1111u, 0x2222u));
  entries.push_back(entry("beta", EvidenceState::Absent, 2, 0x3333u, 0x4444u));
  entries.push_back(entry("gamma", EvidenceState::Unknown, 3, 0x5555u, 0x6666u));
  return require_value("evidence", EvidenceVector::build(std::move(entries)));
}

inline ContainmentPolicy sample_policy() {
  ContainmentPolicy policy;
  policy.generation = PolicyGeneration{2};
  policy.require_failure_source_inclusion = true;
  policy.allow_protected_inclusion = false;
  policy.minimize_protected_inclusions = true;
  policy.max_boundary_weight = 1000000;
  policy.max_boundary_members = 128;
  policy.exact_search_budget = 5000;
  policy.heuristic_budget = 5000;
  policy.exact_instance_node_limit = 512;
  return policy;
}

inline fcfn::model::AuthorityVector sample_authority() {
  fcfn::model::AuthorityVector authority;
  authority.epoch = CoordinatorEpoch{4};
  authority.boot = fcfn::model::BootIdentity{fcfn::model::BootId{0x1122334455667788ull}, 4242};
  authority.policy = PolicyGeneration{2};
  authority.topology = TopologyGeneration{7};
  authority.evidence_revision = EvidenceRevision{3};
  authority.evidence_digest = Digest{0xdeadbeefcafebabeull, 0x0f0e0d0c0b0a0908ull};
  authority.issued_sequence = fcfn::model::Sequence{21};
  return authority;
}

inline ContainmentBoundary sample_boundary() {
  std::vector<BoundaryMember> members;
  BoundaryMember source;
  source.resource = rid("alpha");
  source.reason = InclusionReason::FailureSource;
  source.protected_obligation = false;
  source.weight = 3;
  members.push_back(std::move(source));

  BoundaryMember cut;
  cut.resource = rid("beta");
  cut.reason = InclusionReason::CutVertexOnUnknownPath;
  cut.protected_obligation = false;
  cut.weight = 5;
  cut.witness_path = {rid("beta"), rid("gamma")};
  cut.witness_uses_unknown = true;
  members.push_back(std::move(cut));

  return require_value("boundary", ContainmentBoundary::create(BoundaryGeneration{9}, std::move(members)));
}

inline ContainmentPlan sample_plan() {
  ContainmentPlan plan;
  plan.generation = PlanGeneration{5};
  plan.boundary = sample_boundary();
  plan.claim = ContainmentClaim::ProvenContainment;
  plan.feasibility = FeasibilityStatus::ProvenFeasible;
  plan.optimality = OptimalityStatus::ProvenOptimal;
  plan.currency = EvidenceCurrency::Current;
  plan.authority = sample_authority();
  plan.topology_digest = Digest{0x0102030405060708ull, 0x1112131415161718ull};
  plan.policy_digest = Digest{0x2122232425262728ull, 0x3132333435363738ull};
  plan.evidence_digest = Digest{0x4142434445464748ull, 0x5152535455565758ull};
  plan.instance_digest = Digest{0x6162636465666768ull, 0x7172737475767778ull};
  plan.failure_sources = {rid("alpha")};
  plan.protected_obligations = {rid("gamma")};
  plan.uncontainable_sources = {};
  plan.proven_necessary_members = {rid("beta")};
  plan.indeterminate_necessity = {rid("delta")};
  plan.total_weight = 8;
  plan.member_count = 2;
  plan.counters = SearchCounters{10, 5, 3, 2, 200000, false, false};
  plan.infeasibility.present = false;
  (void)plan.explanation.add(fcfn::model::ReasonCode::FailureSourceContained, "alpha carries failure evidence");
  (void)plan.explanation.add_resource(fcfn::model::ReasonCode::ProtectedObligationIncluded, rid("gamma"),
                                      "protected obligation inside the fence");
  (void)plan.explanation.add_value(fcfn::model::ReasonCode::OptimalityProven, 8, "minimal total weight");
  plan.seal();
  return plan;
}

// ---------------------------------------------------------------------------
// Shared proof driver
// ---------------------------------------------------------------------------

template <class T>
struct Codec {
  const char* name;
  std::vector<std::byte> (*encode)(const T&);
  Result<T> (*decode)(std::span<const std::byte>);
};

inline const Codec<Topology>& topology_codec() {
  static const Codec<Topology> codec{
      "Topology", +[](const Topology& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return Topology::decode(bytes); }};
  return codec;
}

inline const Codec<EvidenceVector>& evidence_codec() {
  static const Codec<EvidenceVector> codec{
      "EvidenceVector", +[](const EvidenceVector& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return EvidenceVector::decode(bytes); }};
  return codec;
}

inline const Codec<ContainmentPolicy>& policy_codec() {
  static const Codec<ContainmentPolicy> codec{
      "ContainmentPolicy", +[](const ContainmentPolicy& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return ContainmentPolicy::decode(bytes); }};
  return codec;
}

inline const Codec<fcfn::model::AuthorityVector>& authority_codec() {
  static const Codec<fcfn::model::AuthorityVector> codec{
      "AuthorityVector", +[](const fcfn::model::AuthorityVector& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return fcfn::model::AuthorityVector::decode(bytes); }};
  return codec;
}

inline const Codec<ContainmentBoundary>& boundary_codec() {
  static const Codec<ContainmentBoundary> codec{
      "ContainmentBoundary", +[](const ContainmentBoundary& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return ContainmentBoundary::decode(bytes); }};
  return codec;
}

inline const Codec<ContainmentPlan>& plan_codec() {
  static const Codec<ContainmentPlan> codec{
      "ContainmentPlan", +[](const ContainmentPlan& value) { return value.encode(); },
      +[](std::span<const std::byte> bytes) { return ContainmentPlan::decode(bytes); }};
  return codec;
}

/// Assert that decoding produced exactly the expected classified refusal.
template <class T>
void expect_status(const char* what, Result<T> result, StatusCode expected) {
  if (result.status().code() != expected) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what + "': expected " + to_string(expected) +
                           " but got " + result.status().to_string());
  }
}

/// Round trip, totality, and trailing-garbage refusal for one model type.
template <class T>
void prove_canonical_codec(const Codec<T>& codec, const T& value) {
  const std::vector<std::byte> encoded = codec.encode(value);
  if (encoded.empty()) {
    ::fcfn::test::fail(__FILE__, __LINE__, std::string(codec.name) + " encoded to zero bytes");
  }

  const Result<T> decoded = codec.decode(encoded);
  if (!decoded.ok()) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(codec.name) + " failed to decode its own encoding: " +
                           decoded.status().to_string() + " bytes=" + hex_of(encoded));
  }
  const std::vector<std::byte> reencoded = codec.encode(decoded.value());
  if (!same_bytes(reencoded, encoded)) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(codec.name) + " round trip is not byte-identical: expected " +
                           hex_of(encoded) + " got " + hex_of(reencoded));
  }
  // Decoding is idempotent: the canonical form of a decoded document is stable.
  const Result<T> again = codec.decode(reencoded);
  if (!again.ok() || !same_bytes(codec.encode(again.value()), encoded)) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(codec.name) + " decode is not idempotent over " + hex_of(encoded));
  }

  // Totality: every strict prefix of a valid document is refused, and a refused
  // decode can never hand back a partially built object (Result carries either a
  // value or a Status, never both).
  for (std::size_t length = 1; length < encoded.size(); ++length) {
    const Result<T> truncated = codec.decode(std::span<const std::byte>(encoded.data(), length));
    if (truncated.ok()) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         std::string(codec.name) + " accepted truncated document of length " +
                             std::to_string(length) + " of " + std::to_string(encoded.size()) +
                             " bytes: " + hex_of(std::span<const std::byte>(encoded.data(), length)));
    }
    if (truncated.status().ok()) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         std::string(codec.name) + " reported success status for a refused decode");
    }
  }

  // Trailing garbage is a distinct, explicit refusal.
  for (const std::size_t extra : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7},
                                  std::size_t{16}, std::size_t{64}}) {
    std::vector<std::byte> padded = encoded;
    padded.insert(padded.end(), extra, std::byte{0xa5});
    const Result<T> result = codec.decode(padded);
    if (result.status().code() != StatusCode::TrailingGarbage) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         std::string(codec.name) + " with " + std::to_string(extra) +
                             " trailing bytes reported " + result.status().to_string() +
                             " instead of TrailingGarbage");
    }
  }
}

}  // namespace fcfn::test::codec

#endif  // FCFN_TEST_CODEC_SUPPORT_HPP
