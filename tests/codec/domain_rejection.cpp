// FCFN codec suite: hand-crafted out-of-domain encodings.
//
// Product proposition proved here: the canonical decoder is a gate, not a
// repair shop. Out-of-range enums, zero generations, out-of-bound counts,
// absurd declared lengths, duplicate and malformed identifiers are each refused
// with an explicit status; nothing is silently normalised into a valid
// decision. Where the library instead documents canonical-form normalisation
// (ordering) that contract is asserted explicitly and named as such.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec_support.hpp"

namespace {

using namespace fcfn::test::codec;

// ---------------------------------------------------------------------------
// Document builders
// ---------------------------------------------------------------------------

void write_node(CanonicalWriter& writer, std::string_view id, bool containable, std::uint64_t weight,
                bool protected_obligation, std::uint8_t completeness) {
  writer.text(id);
  writer.boolean(containable);
  writer.u64(weight);
  writer.boolean(protected_obligation);
  writer.u8(completeness);
}

void write_edge(CanonicalWriter& writer, std::uint32_t from, std::uint32_t to, std::uint8_t evidence,
                std::uint64_t generation) {
  writer.u32(from);
  writer.u32(to);
  writer.u8(evidence);
  writer.u64(generation);
}

/// One well-formed node and no edges.
std::vector<std::byte> one_node_topology(std::uint64_t generation, std::string_view id) {
  CanonicalWriter writer;
  writer.u64(generation);
  writer.u32(1);
  write_node(writer, id, true, 1, false, 0);
  writer.u32(0);
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

std::vector<std::byte> one_entry_evidence(std::string_view resource, std::uint8_t state,
                                          std::uint64_t generation, std::uint32_t length_override) {
  CanonicalWriter writer;
  writer.u32(1);
  if (length_override == 0) {
    writer.text(resource);
  } else {
    writer.u32(length_override);
  }
  writer.u8(state);
  writer.u64(generation);
  writer.digest(Digest{1, 2});
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

std::vector<std::byte> policy_document(std::uint64_t generation, std::uint8_t inclusion,
                                       std::uint8_t allow_protected, std::uint8_t minimize,
                                       std::uint64_t max_weight, std::uint64_t max_members,
                                       std::uint64_t exact_budget, std::uint64_t heuristic_budget,
                                       std::uint64_t node_limit) {
  CanonicalWriter writer;
  writer.u64(generation);
  writer.u8(inclusion);
  writer.u8(allow_protected);
  writer.u8(minimize);
  writer.u64(max_weight);
  writer.u64(max_members);
  writer.u64(exact_budget);
  writer.u64(heuristic_budget);
  writer.u64(node_limit);
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

void write_member(CanonicalWriter& writer, std::string_view resource, std::uint8_t reason,
                  std::uint64_t weight, std::uint32_t steps) {
  writer.text(resource);
  writer.u8(reason);
  writer.boolean(false);
  writer.u64(weight);
  writer.boolean(false);
  writer.u32(steps);
  for (std::uint32_t step = 0; step < steps; ++step) {
    writer.text("gamma");
  }
}

std::vector<std::byte> boundary_document(std::uint64_t generation, std::uint32_t declared_count,
                                         std::string_view resource, std::uint8_t reason,
                                         std::uint64_t weight, std::uint32_t steps) {
  CanonicalWriter writer;
  writer.u64(generation);
  writer.u32(declared_count);
  if (declared_count != 0) {
    write_member(writer, resource, reason, weight, steps);
  }
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

/// Writes the plan fields that precede the identifier lists.
void write_plan_head(CanonicalWriter& writer, const fcfn::model::ContainmentPlan& plan,
                     std::uint8_t claim, std::uint8_t feasibility, std::uint8_t optimality,
                     std::uint8_t currency) {
  writer.u64(plan.generation.value());
  const std::vector<std::byte> boundary_bytes = plan.boundary.encode();
  writer.blob(boundary_bytes);
  writer.u8(claim);
  writer.u8(feasibility);
  writer.u8(optimality);
  writer.u8(currency);
  const std::vector<std::byte> authority_bytes = plan.authority.encode();
  writer.blob(authority_bytes);
  writer.digest(plan.topology_digest);
  writer.digest(plan.policy_digest);
  writer.digest(plan.evidence_digest);
  writer.digest(plan.instance_digest);
}

/// Writes every plan field between the authority binding and the explanation.
void write_empty_plan_tail(CanonicalWriter& writer) {
  for (int list = 0; list < 5; ++list) {
    writer.u32(0);
  }
  writer.u64(0);  // total_weight
  writer.u64(0);  // member_count
  for (int counter = 0; counter < 5; ++counter) {
    writer.u64(0);
  }
  writer.boolean(false);  // budget_exhausted
  writer.boolean(false);  // heuristic_used
  writer.boolean(false);  // infeasibility.present
  writer.u32(0);          // infeasibility.path
}

// ---------------------------------------------------------------------------
// Domain rejection tests
// ---------------------------------------------------------------------------

void check_topology_refusals() {
  const Codec<fcfn::model::Topology>& codec = topology_codec();

  expect_status("topology zero generation", codec.decode(one_node_topology(0, "alpha")),
                StatusCode::InvalidArgument);
  expect_status("topology empty node id", codec.decode(one_node_topology(1, "")),
                StatusCode::InvalidArgument);
  expect_status("topology id with path separator", codec.decode(one_node_topology(1, "rack/leaf")),
                StatusCode::InvalidArgument);
  expect_status("topology id with parent traversal", codec.decode(one_node_topology(1, "..")),
                StatusCode::InvalidArgument);
  expect_status("topology id with embedded traversal",
                codec.decode(one_node_topology(1, "a..b")), StatusCode::InvalidArgument);
  // An identifier longer than the canonical blob bound is refused by length
  // before the domain grammar is even consulted.
  expect_status("topology id longer than the domain bound",
                codec.decode(one_node_topology(1, std::string(64, 'x'))),
                StatusCode::LimitExceeded);

  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(static_cast<std::uint32_t>(kMaxTopologyNodes + 1));
    expect_status("topology node count above bound", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(0xffffffffu);
    expect_status("topology node count 0xFFFFFFFF", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    writer.u32(0xffffffffu);  // node id length
    expect_status("topology absurd id length", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "alpha", true, 1, false, 0);
    write_node(writer, "alpha", true, 1, false, 0);
    writer.u32(0);
    expect_status("topology duplicate node id", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    write_node(writer, "alpha", true, 0, false, 0);
    writer.u32(0);
    expect_status("topology zero weight", codec.decode(writer.span()), StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    write_node(writer, "alpha", true, kMaxNodeWeight + 1, false, 0);
    writer.u32(0);
    expect_status("topology weight above bound", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    write_node(writer, "alpha", true, 1, false, 3);
    writer.u32(0);
    expect_status("topology out-of-range completeness enum", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    writer.text("alpha");
    writer.u8(2);  // containable byte out of the boolean domain
    writer.u64(1);
    writer.boolean(false);
    writer.u8(0);
    writer.u32(0);
    expect_status("topology boolean byte out of domain", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    write_node(writer, "alpha", true, 1, true, 0);  // protected and containable
    writer.u32(0);
    expect_status("topology protected node marked containable", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(0);
    writer.u32(0);
    expect_status("topology with zero nodes", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "alpha", true, 1, false, 0);
    write_node(writer, "beta", true, 1, false, 0);
    writer.u32(1);
    write_edge(writer, 0, 1, 3, 1);  // evidence enum out of range
    expect_status("topology out-of-range edge evidence enum", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "alpha", true, 1, false, 0);
    write_node(writer, "beta", true, 1, false, 0);
    writer.u32(1);
    write_edge(writer, 0, 7, 0, 1);  // endpoint out of range
    expect_status("topology edge endpoint out of range", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    write_node(writer, "alpha", true, 1, false, 0);
    writer.u32(1);
    write_edge(writer, 0, 0, 0, 1);  // self loop
    expect_status("topology self loop", codec.decode(writer.span()), StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "alpha", true, 1, false, 0);
    write_node(writer, "beta", true, 1, false, 0);
    writer.u32(1);
    write_edge(writer, 0, 1, 0, 0);  // zero evidence generation
    expect_status("topology zero edge generation", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "alpha", true, 1, false, 0);
    write_node(writer, "beta", true, 1, false, 0);
    writer.u32(2);
    write_edge(writer, 0, 1, 0, 1);
    write_edge(writer, 0, 1, 1, 2);
    expect_status("topology duplicate edge", codec.decode(writer.span()),
                  StatusCode::AlreadyExists);
  }
  {
    std::vector<std::byte> document = one_node_topology(1, "alpha");
    document.push_back(std::byte{0x5a});
    expect_status("topology trailing garbage", codec.decode(document),
                  StatusCode::TrailingGarbage);
  }
}

void check_evidence_refusals() {
  const Codec<fcfn::model::EvidenceVector>& codec = evidence_codec();
  {
    CanonicalWriter writer;
    writer.u32(0xffffffffu);
    expect_status("evidence count 0xFFFFFFFF", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u32(static_cast<std::uint32_t>(kMaxTopologyNodes + 1));
    expect_status("evidence count above bound", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  expect_status("evidence zero generation", codec.decode(one_entry_evidence("alpha", 0, 0, 0)),
                StatusCode::InvalidArgument);
  expect_status("evidence out-of-range state enum",
                codec.decode(one_entry_evidence("alpha", 3, 1, 0)), StatusCode::InvalidArgument);
  expect_status("evidence empty resource id", codec.decode(one_entry_evidence("", 0, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("evidence resource id with path separator",
                codec.decode(one_entry_evidence("rack/leaf", 0, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("evidence parent traversal id", codec.decode(one_entry_evidence("..", 0, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("evidence absurd id length", codec.decode(one_entry_evidence("alpha", 0, 1, 0xffffffffu)),
                StatusCode::LimitExceeded);
  {
    CanonicalWriter writer;
    writer.u32(2);
    writer.text("beta");
    writer.u8(0);
    writer.u64(5);
    writer.digest(Digest{1, 2});
    writer.text("beta");
    writer.u8(1);
    writer.u64(6);
    writer.digest(Digest{3, 4});
    expect_status("evidence duplicate resource", codec.decode(writer.span()),
                  StatusCode::AlreadyExists);
  }
  {
    std::vector<std::byte> document = sample_evidence().encode();
    document.push_back(std::byte{0x00});
    expect_status("evidence trailing garbage", codec.decode(document), StatusCode::TrailingGarbage);
  }
}

void check_policy_refusals() {
  const Codec<fcfn::model::ContainmentPolicy>& codec = policy_codec();
  const std::uint64_t members = 128;
  const std::uint64_t weight = 1000000;
  expect_status("policy zero generation",
                codec.decode(policy_document(0, 1, 0, 1, weight, members, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy zero member bound",
                codec.decode(policy_document(1, 1, 0, 1, weight, 0, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy member bound above kMaxBoundaryMembers",
                codec.decode(policy_document(1, 1, 0, 1, weight, kMaxBoundaryMembers + 1, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy member bound 0xFFFFFFFFFFFFFFFF",
                codec.decode(policy_document(1, 1, 0, 1, weight, 0xffffffffffffffffull, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy zero weight bound",
                codec.decode(policy_document(1, 1, 0, 1, 0, members, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy weight bound above kMaxPlanWeightSum",
                codec.decode(policy_document(1, 1, 0, 1, kMaxPlanWeightSum + 1, members, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy zero exact search budget",
                codec.decode(policy_document(1, 1, 0, 1, weight, members, 0, 5000, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy zero heuristic budget",
                codec.decode(policy_document(1, 1, 0, 1, weight, members, 5000, 0, 512)),
                StatusCode::InvalidArgument);
  expect_status("policy zero instance node limit",
                codec.decode(policy_document(1, 1, 0, 1, weight, members, 5000, 5000, 0)),
                StatusCode::InvalidArgument);
  expect_status("policy instance node limit above kMaxTopologyNodes",
                codec.decode(policy_document(1, 1, 0, 1, weight, members, 5000, 5000, kMaxTopologyNodes + 1)),
                StatusCode::InvalidArgument);
  expect_status("policy boolean byte out of domain",
                codec.decode(policy_document(1, 2, 0, 1, weight, members, 5000, 5000, 512)),
                StatusCode::InvalidArgument);
  {
    std::vector<std::byte> document = sample_policy().encode();
    document.insert(document.end(), 3, std::byte{0x11});
    expect_status("policy trailing garbage", codec.decode(document), StatusCode::TrailingGarbage);
  }
}

void check_authority_refusals() {
  const Codec<fcfn::model::AuthorityVector>& codec = authority_codec();
  // The authority codec is a pure transport: it has no domain of its own, and
  // the all-zero vector is decodable by design. What must never happen is that
  // such a vector is treated as current, so the assertion is on validation, not
  // on decoding.
  {
    CanonicalWriter writer;
    writer.u64(0);  // epoch
    writer.u64(0);  // boot id
    writer.u32(0);  // process id
    writer.u64(0);  // policy generation
    writer.u64(0);  // topology generation
    writer.u64(0);  // evidence revision
    writer.digest(Digest{0, 0});
    writer.u64(0);  // issued sequence
    const auto decoded = codec.decode(writer.span());
    FCFN_CHECK_OK(decoded);
    FCFN_CHECK(decoded.value().epoch.is_zero());
    FCFN_CHECK(decoded.value().policy.is_zero());
    const fcfn::model::AuthorityCheck check =
        fcfn::model::validate_authority(decoded.value(), sample_authority());
    FCFN_CHECK(!check.ok());
    FCFN_CHECK(check.status_code() == StatusCode::Fenced);
  }
  {
    std::vector<std::byte> document = sample_authority().encode();
    document.insert(document.end(), 1, std::byte{0x7f});
    expect_status("authority trailing garbage", codec.decode(document),
                  StatusCode::TrailingGarbage);
  }
  {
    std::vector<std::byte> document = sample_authority().encode();
    document.resize(document.size() - 1);
    expect_status("authority truncated", codec.decode(document), StatusCode::InvalidArgument);
  }
}

void check_boundary_refusals() {
  const Codec<fcfn::model::ContainmentBoundary>& codec = boundary_codec();
  expect_status("boundary zero generation", codec.decode(boundary_document(0, 0, "alpha", 0, 0, 0)),
                StatusCode::InvalidArgument);
  expect_status("boundary member count above kMaxBoundaryMembers",
                codec.decode(boundary_document(1, static_cast<std::uint32_t>(kMaxBoundaryMembers + 1),
                                               "alpha", 0, 0, 0)),
                StatusCode::LimitExceeded);
  expect_status("boundary member count 0xFFFFFFFF",
                codec.decode(boundary_document(1, 0xffffffffu, "alpha", 0, 0, 0)),
                StatusCode::LimitExceeded);
  expect_status("boundary out-of-range inclusion reason enum",
                codec.decode(boundary_document(1, 1, "alpha", 6, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("boundary weight above kMaxNodeWeight",
                codec.decode(boundary_document(1, 1, "alpha", 0, kMaxNodeWeight + 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("boundary empty member id", codec.decode(boundary_document(1, 1, "", 0, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("boundary member id with path separator",
                codec.decode(boundary_document(1, 1, "rack/leaf", 0, 1, 0)),
                StatusCode::InvalidArgument);
  expect_status("boundary witness path above kMaxCutCertificatePathNodes",
                codec.decode(boundary_document(1, 1, "alpha", 0, 1,
                                               static_cast<std::uint32_t>(kMaxCutCertificatePathNodes + 1))),
                StatusCode::LimitExceeded);
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    writer.u32(0xffffffffu);  // member id length
    expect_status("boundary absurd member id length", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_member(writer, "alpha", 0, 1, 0);
    write_member(writer, "alpha", 0, 1, 0);
    expect_status("boundary duplicate member", codec.decode(writer.span()),
                  StatusCode::AlreadyExists);
  }
  {
    std::vector<std::byte> document = sample_boundary().encode();
    document.push_back(std::byte{0xff});
    expect_status("boundary trailing garbage", codec.decode(document),
                  StatusCode::TrailingGarbage);
  }
}

void check_plan_refusals() {
  const Codec<fcfn::model::ContainmentPlan>& codec = plan_codec();
  const fcfn::model::ContainmentPlan plan = sample_plan();
  const auto base = [&plan](std::uint8_t claim, std::uint8_t feasibility, std::uint8_t optimality,
                            std::uint8_t currency) {
    CanonicalWriter writer;
    write_plan_head(writer, plan, claim, feasibility, optimality, currency);
    return writer;
  };

  expect_status("plan out-of-range claim enum", codec.decode(base(6, 0, 0, 0).span()),
                StatusCode::InvalidArgument);
  expect_status("plan out-of-range feasibility enum", codec.decode(base(0, 3, 0, 0).span()),
                StatusCode::InvalidArgument);
  expect_status("plan out-of-range optimality enum", codec.decode(base(0, 0, 4, 0).span()),
                StatusCode::InvalidArgument);
  expect_status("plan out-of-range currency enum", codec.decode(base(0, 0, 0, 4).span()),
                StatusCode::InvalidArgument);
  {
    CanonicalWriter writer;
    writer.u64(4);
    writer.u32(0xffffffffu);  // boundary blob length
    expect_status("plan absurd boundary blob length", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    write_plan_head(writer, plan, 0, 0, 0, 0);
    writer.u32(static_cast<std::uint32_t>(kMaxFailureSources + 1));
    expect_status("plan failure source count above bound", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    write_plan_head(writer, plan, 0, 0, 0, 0);
    write_empty_plan_tail(writer);
    writer.u32(static_cast<std::uint32_t>(kMaxExplanationItems + 1));
    expect_status("plan explanation above kMaxExplanationItems", codec.decode(writer.span()),
                StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    write_plan_head(writer, plan, 0, 0, 0, 0);
    write_empty_plan_tail(writer);
    writer.u32(1);
    writer.u16(static_cast<std::uint16_t>(kMaxReasonCodeValue + 1));
    writer.text("alpha");
    writer.boolean(false);
    writer.u64(0);
    writer.boolean(false);
    writer.text("");
    expect_status("plan explanation reason code above the domain", codec.decode(writer.span()),
                  StatusCode::InvalidArgument);
  }
  {
    // An authority binding larger than its declared bound must be refused
    // before it is materialised.
    CanonicalWriter writer;
    writer.u64(4);
    const std::vector<std::byte> boundary_bytes = plan.boundary.encode();
    writer.blob(boundary_bytes);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    std::vector<std::byte> oversized(2048, std::byte{0x01});
    writer.blob(oversized);
    expect_status("plan authority binding above its bound", codec.decode(writer.span()),
                  StatusCode::LimitExceeded);
  }
  {
    std::vector<std::byte> document = plan.encode();
    document[document.size() - 1] = static_cast<std::byte>(
        static_cast<std::uint8_t>(document[document.size() - 1]) ^ 0x01u);
    expect_status("plan digest mismatch", codec.decode(document), StatusCode::Corrupt);
  }
  {
    std::vector<std::byte> document = plan.encode();
    document.insert(document.end(), 8, std::byte{0x00});
    expect_status("plan trailing garbage", codec.decode(document), StatusCode::TrailingGarbage);
  }
}

}  // namespace

FCFN_TEST(domain, topology_refuses_every_out_of_domain_encoding) { check_topology_refusals(); }

FCFN_TEST(domain, evidence_refuses_every_out_of_domain_encoding) { check_evidence_refusals(); }

FCFN_TEST(domain, policy_refuses_every_out_of_domain_encoding) { check_policy_refusals(); }

FCFN_TEST(domain, authority_decodes_zero_ids_but_never_validates_them_as_current) {
  check_authority_refusals();
}

FCFN_TEST(domain, boundary_refuses_every_out_of_domain_encoding) { check_boundary_refusals(); }

FCFN_TEST(domain, plan_refuses_every_out_of_domain_encoding) { check_plan_refusals(); }

FCFN_TEST(domain, unordered_documents_are_normalised_into_the_canonical_order) {
  // The headers document canonical-form normalisation: Topology sorts its nodes
  // and edges, EvidenceVector sorts its entries, ContainmentBoundary sorts its
  // members. A decoded document is therefore canonical even when the input was
  // not, and re-encoding is stable. Nothing else is normalised: every other
  // out-of-domain value is refused by the cases above.
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(2);
    write_node(writer, "gamma", false, 2, true, 0);
    write_node(writer, "alpha", true, 1, false, 0);
    writer.u32(0);
    const auto decoded = fcfn::model::Topology::decode(writer.span());
    FCFN_CHECK_OK(decoded);
    FCFN_CHECK_EQ(decoded.value().resource(0).value(), std::string("alpha"));
    FCFN_CHECK_EQ(decoded.value().resource(1).value(), std::string("gamma"));
    const std::vector<std::byte> canonical = decoded.value().encode();
    const auto again = fcfn::model::Topology::decode(canonical);
    FCFN_CHECK_OK(again);
    FCFN_CHECK(same_bytes(again.value().encode(), canonical));
  }
  {
    CanonicalWriter writer;
    writer.u32(2);
    writer.text("gamma");
    writer.u8(0);
    writer.u64(2);
    writer.digest(Digest{3, 4});
    writer.text("alpha");
    writer.u8(1);
    writer.u64(1);
    writer.digest(Digest{1, 2});
    const auto decoded = fcfn::model::EvidenceVector::decode(writer.span());
    FCFN_CHECK_OK(decoded);
    FCFN_CHECK_EQ(decoded.value().entries().front().resource.value(), std::string("alpha"));
    FCFN_CHECK_EQ(decoded.value().entries().back().resource.value(), std::string("gamma"));
    const std::vector<std::byte> canonical = decoded.value().encode();
    const auto again = fcfn::model::EvidenceVector::decode(canonical);
    FCFN_CHECK_OK(again);
    FCFN_CHECK(same_bytes(again.value().encode(), canonical));
  }
  {
    CanonicalWriter writer;
    writer.u64(3);
    writer.u32(2);
    write_member(writer, "gamma", 1, 2, 0);
    write_member(writer, "alpha", 0, 1, 0);
    const auto decoded = fcfn::model::ContainmentBoundary::decode(writer.span());
    FCFN_CHECK_OK(decoded);
    FCFN_CHECK_EQ(decoded.value().members().front().resource.value(), std::string("alpha"));
    FCFN_CHECK_EQ(decoded.value().members().back().resource.value(), std::string("gamma"));
    const std::vector<std::byte> canonical = decoded.value().encode();
    const auto again = fcfn::model::ContainmentBoundary::decode(canonical);
    FCFN_CHECK_OK(again);
    FCFN_CHECK(same_bytes(again.value().encode(), canonical));
  }
}
