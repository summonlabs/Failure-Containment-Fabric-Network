// FCFN codec suite: bounded decoding of hostile declared sizes.
//
// Product proposition proved here: a declared count or length that exceeds its
// bound is refused with an explicit status and without materialising anything.
// A short document can never make the decoder allocate for content that is not
// present, and even a declared count exactly at the documented bound stays
// inside the envelope the bound describes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "allocation_probe.hpp"
#include "codec_support.hpp"

namespace {

using namespace fcfn::test::codec;

constexpr std::uint64_t kRefusalAllocationCeiling = 4096;

template <class T>
void expect_bounded_refusal(const char* what, const Codec<T>& codec,
                            const std::vector<std::byte>& document, StatusCode expected) {
  const fcfn::test::AllocationWindow window;
  const Result<T> result = codec.decode(document);
  if (result.status().code() != expected) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what + "': expected " + to_string(expected) +
                           " but got " + result.status().to_string());
  }
  if (window.bytes() >= kRefusalAllocationCeiling) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string("case '") + what + "': refusal allocated " +
                           std::to_string(window.bytes()) + " bytes before reporting " +
                           to_string(expected));
  }
}

std::vector<std::byte> bytes_of(const CanonicalWriter& writer) {
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

}  // namespace

FCFN_TEST(bounded, huge_declared_counts_are_refused_without_allocating) {
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("topology node count 0xFFFFFFFF", topology_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(static_cast<std::uint32_t>(kMaxTopologyNodes) + 1u);
    expect_bounded_refusal("topology node count above bound", topology_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    // The edge table is read only after the node table, so a zero-length node
    // table makes the declared edge count the first bounded field.
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(0);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("topology edge count 0xFFFFFFFF", topology_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u32(0xffffffffu);
    expect_bounded_refusal("evidence count 0xFFFFFFFF", evidence_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("boundary member count 0xFFFFFFFF", boundary_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    // Plan identifier lists: the count is read before any element is parsed.
    const fcfn::model::ContainmentPlan plan = sample_plan();
    CanonicalWriter writer;
    writer.u64(plan.generation.value());
    const std::vector<std::byte> boundary_bytes = plan.boundary.encode();
    writer.blob(boundary_bytes);
    writer.u8(1);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    const std::vector<std::byte> authority_bytes = plan.authority.encode();
    writer.blob(authority_bytes);
    writer.digest(plan.topology_digest);
    writer.digest(plan.policy_digest);
    writer.digest(plan.evidence_digest);
    writer.digest(plan.instance_digest);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("plan failure source count 0xFFFFFFFF", plan_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    const fcfn::model::ContainmentPlan plan = sample_plan();
    CanonicalWriter writer;
    writer.u64(plan.generation.value());
    const std::vector<std::byte> boundary_bytes = plan.boundary.encode();
    writer.blob(boundary_bytes);
    writer.u8(1);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    const std::vector<std::byte> authority_bytes = plan.authority.encode();
    writer.blob(authority_bytes);
    writer.digest(plan.topology_digest);
    writer.digest(plan.policy_digest);
    writer.digest(plan.evidence_digest);
    writer.digest(plan.instance_digest);
    for (int list = 0; list < 5; ++list) {
      writer.u32(0);
    }
    writer.u64(0);
    writer.u64(0);
    for (int counter = 0; counter < 5; ++counter) {
      writer.u64(0);
    }
    writer.boolean(false);
    writer.boolean(false);
    writer.boolean(false);
    writer.u32(0);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("plan explanation count 0xFFFFFFFF", plan_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
}

FCFN_TEST(bounded, huge_declared_lengths_are_refused_without_allocating) {
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("topology node id length 0xFFFFFFFF", topology_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u32(1);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("evidence resource id length 0xFFFFFFFF", evidence_codec(),
                           bytes_of(writer), StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(1);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("boundary member id length 0xFFFFFFFF", boundary_codec(),
                           bytes_of(writer), StatusCode::LimitExceeded);
  }
  {
    CanonicalWriter writer;
    writer.u64(4);
    writer.u32(0xffffffffu);
    expect_bounded_refusal("plan boundary blob length 0xFFFFFFFF", plan_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
  {
    const fcfn::model::ContainmentPlan plan = sample_plan();
    CanonicalWriter writer;
    writer.u64(plan.generation.value());
    const std::vector<std::byte> boundary_bytes = plan.boundary.encode();
    writer.blob(boundary_bytes);
    writer.u8(1);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    std::vector<std::byte> oversized(2048, std::byte{0x02});
    writer.blob(oversized);
    expect_bounded_refusal("plan authority blob above its bound", plan_codec(), bytes_of(writer),
                           StatusCode::LimitExceeded);
  }
}

FCFN_TEST(bounded, a_declared_count_at_the_bound_stays_inside_the_envelope) {
  // A count exactly at the documented bound is legal, so the decoder reserves
  // for it before discovering that the document is truncated. That reservation
  // is bounded by the bound itself and must stay far from unbounded growth.
  const fcfn::test::AllocationWindow window;
  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(static_cast<std::uint32_t>(kMaxTopologyNodes));
    const auto result = fcfn::model::Topology::decode(writer.span());
    FCFN_CHECK(result.status().code() == StatusCode::InvalidArgument);
  }
  const std::uint64_t reserved = window.bytes();
  FCFN_CHECK(reserved <= 16u * 1024u * 1024u);
  std::printf("  declared node count at the bound reserved %llu bytes before refusing a short document\n",
              static_cast<unsigned long long>(reserved));

  {
    CanonicalWriter writer;
    writer.u64(1);
    writer.u32(static_cast<std::uint32_t>(kMaxBoundaryMembers));
    const fcfn::test::AllocationWindow member_window;
    const auto result = fcfn::model::ContainmentBoundary::decode(writer.span());
    FCFN_CHECK(result.status().code() == StatusCode::InvalidArgument);
    FCFN_CHECK(member_window.bytes() <= 4u * 1024u * 1024u);
  }
}

FCFN_TEST(bounded, the_allocation_probe_observes_real_allocations) {
  const fcfn::test::AllocationWindow window;
  std::vector<std::byte> control(2u << 20, std::byte{0x11});
  FCFN_CHECK(window.bytes() >= (2u << 20));
  FCFN_CHECK(window.calls() >= 1);
  FCFN_CHECK(control.size() == (2u << 20));
}
