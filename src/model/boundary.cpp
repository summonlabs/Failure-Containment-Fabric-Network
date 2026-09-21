// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/boundary.hpp"

#include <algorithm>
#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/json.hpp"

namespace fcfn::model {
namespace {

struct ReasonToken {
  InclusionReason value;
  const char* token;
};

constexpr ReasonToken kInclusionTokens[] = {
    {InclusionReason::FailureSource, "failure_source"},
    {InclusionReason::CutVertexOnProvenPath, "cut_vertex_on_proven_path"},
    {InclusionReason::CutVertexOnUnknownPath, "cut_vertex_on_unknown_path"},
    {InclusionReason::PolicyMandatedSource, "policy_mandated_source"},
    {InclusionReason::ProtectedObligationIsolation, "protected_obligation_isolation"},
    {InclusionReason::SharedRiskDomainFence, "shared_risk_domain_fence"},
};

}  // namespace

const char* to_string(InclusionReason value) noexcept {
  for (const ReasonToken& entry : kInclusionTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_inclusion_reason";
}

bool parse_inclusion_reason(std::string_view token, InclusionReason& out) noexcept {
  for (const ReasonToken& entry : kInclusionTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

Result<ContainmentBoundary> ContainmentBoundary::create(BoundaryGeneration generation,
                                                         std::vector<BoundaryMember> members) {
  if (generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "boundary generation must be non-zero"};
  }
  if (members.size() > kMaxBoundaryMembers) {
    return Status{StatusCode::LimitExceeded, "boundary member count exceeds bound"};
  }
  std::sort(members.begin(), members.end(), [](const BoundaryMember& a, const BoundaryMember& b) {
    return a.resource < b.resource;
  });
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].resource.empty()) {
      return Status{StatusCode::InvalidArgument, "boundary member has empty resource id"};
    }
    if (members[i].weight > kMaxNodeWeight) {
      return Status{StatusCode::InvalidArgument, "boundary member weight out of range"};
    }
    if (members[i].witness_path.size() > kMaxCutCertificatePathNodes) {
      return Status{StatusCode::LimitExceeded, "witness path exceeds bound"};
    }
    if (i > 0 && members[i - 1].resource == members[i].resource) {
      return Status{StatusCode::AlreadyExists, "duplicate boundary member"};
    }
  }
  ContainmentBoundary boundary;
  boundary.generation_ = generation;
  boundary.members_ = std::move(members);
  boundary.recompute();
  return boundary;
}

void ContainmentBoundary::recompute() {
  CanonicalWriter writer;
  writer.strong(generation_);
  writer.u32(static_cast<std::uint32_t>(members_.size()));
  std::uint64_t total = 0;
  for (const BoundaryMember& member : members_) {
    writer.resource_id(member.resource);
    writer.u8(static_cast<std::uint8_t>(member.reason));
    writer.boolean(member.protected_obligation);
    writer.u64(member.weight);
    writer.boolean(member.witness_uses_unknown);
    writer.u32(static_cast<std::uint32_t>(member.witness_path.size()));
    for (const ResourceId& step : member.witness_path) {
      writer.resource_id(step);
    }
    total += member.weight;
  }
  writer.u64(total);
  digest_ = writer.digest();
}

bool ContainmentBoundary::contains(const ResourceId& resource) const noexcept {
  return find(resource) != nullptr;
}

const BoundaryMember* ContainmentBoundary::find(const ResourceId& resource) const noexcept {
  const auto it = std::lower_bound(members_.begin(), members_.end(), resource,
                                   [](const BoundaryMember& member, const ResourceId& key) {
                                     return member.resource < key;
                                   });
  if (it == members_.end() || !(it->resource == resource)) {
    return nullptr;
  }
  return &*it;
}

std::uint64_t ContainmentBoundary::total_weight() const noexcept {
  std::uint64_t total = 0;
  for (const BoundaryMember& member : members_) {
    total += member.weight;
  }
  return total;
}

std::size_t ContainmentBoundary::protected_member_count() const noexcept {
  std::size_t count = 0;
  for (const BoundaryMember& member : members_) {
    if (member.protected_obligation) {
      ++count;
    }
  }
  return count;
}

bool ContainmentBoundary::is_subset_of(const ContainmentBoundary& other) const noexcept {
  for (const BoundaryMember& member : members_) {
    if (!other.contains(member.resource)) {
      return false;
    }
  }
  return true;
}

bool ContainmentBoundary::is_superset_of(const ContainmentBoundary& other) const noexcept {
  return other.is_subset_of(*this);
}

bool ContainmentBoundary::same_members(const ContainmentBoundary& other) const noexcept {
  if (members_.size() != other.members_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < members_.size(); ++i) {
    if (!(members_[i].resource == other.members_[i].resource)) {
      return false;
    }
  }
  return true;
}

std::vector<std::byte> ContainmentBoundary::encode() const {
  CanonicalWriter writer;
  writer.strong(generation_);
  writer.u32(static_cast<std::uint32_t>(members_.size()));
  for (const BoundaryMember& member : members_) {
    writer.resource_id(member.resource);
    writer.u8(static_cast<std::uint8_t>(member.reason));
    writer.boolean(member.protected_obligation);
    writer.u64(member.weight);
    writer.boolean(member.witness_uses_unknown);
    writer.u32(static_cast<std::uint32_t>(member.witness_path.size()));
    for (const ResourceId& step : member.witness_path) {
      writer.resource_id(step);
    }
  }
  return writer.data();
}

Result<ContainmentBoundary> ContainmentBoundary::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  const BoundaryGeneration generation = reader.strong<BoundaryGenerationTag>();
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (count > kMaxBoundaryMembers) {
    return Status{StatusCode::LimitExceeded, "encoded boundary member count exceeds bound"};
  }
  std::vector<BoundaryMember> members;
  members.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    BoundaryMember member;
    auto resource = reader.resource_id();
    if (!reader.ok()) {
      return reader.status();
    }
    if (!resource.ok()) {
      return resource.status();
    }
    member.resource = std::move(resource.value());
    const std::uint8_t reason = reader.u8();
    member.protected_obligation = reader.boolean();
    member.weight = reader.u64();
    member.witness_uses_unknown = reader.boolean();
    const std::uint32_t steps = reader.u32();
    if (!reader.ok()) {
      return reader.status();
    }
    if (reason > static_cast<std::uint8_t>(InclusionReason::SharedRiskDomainFence)) {
      return Status{StatusCode::InvalidArgument, "invalid inclusion reason value"};
    }
    member.reason = static_cast<InclusionReason>(reason);
    if (steps > kMaxCutCertificatePathNodes) {
      return Status{StatusCode::LimitExceeded, "encoded witness path exceeds bound"};
    }
    member.witness_path.reserve(steps);
    for (std::uint32_t step = 0; step < steps; ++step) {
      auto id = reader.resource_id();
      if (!reader.ok()) {
        return reader.status();
      }
      if (!id.ok()) {
        return id.status();
      }
      member.witness_path.push_back(std::move(id.value()));
    }
    members.push_back(std::move(member));
  }
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return ContainmentBoundary::create(generation, std::move(members));
}

std::string ContainmentBoundary::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.member("generation", generation_.value());
  writer.member("digest", digest_.hex());
  writer.member("member_count", static_cast<std::uint64_t>(members_.size()));
  writer.member("total_weight", total_weight());
  writer.key("members");
  writer.begin_array();
  for (const BoundaryMember& member : members_) {
    writer.begin_object();
    writer.member("resource", member.resource.value());
    writer.member("reason", to_string(member.reason));
    writer.member("protected_obligation", member.protected_obligation);
    writer.member("weight", member.weight);
    writer.member("witness_uses_unknown", member.witness_uses_unknown);
    writer.key("witness_path");
    writer.begin_array();
    for (const ResourceId& step : member.witness_path) {
      writer.value(step.value());
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

}  // namespace fcfn::model
