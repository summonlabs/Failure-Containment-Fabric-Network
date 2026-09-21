// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/policy.hpp"

#include "fcfn/core/canonical.hpp"

namespace fcfn::model {

Digest ContainmentPolicy::digest() const {
  CanonicalWriter writer;
  writer.strong(generation);
  writer.boolean(require_failure_source_inclusion);
  writer.boolean(allow_protected_inclusion);
  writer.boolean(minimize_protected_inclusions);
  writer.u64(max_boundary_weight);
  writer.u64(static_cast<std::uint64_t>(max_boundary_members));
  writer.u64(exact_search_budget);
  writer.u64(heuristic_budget);
  writer.u64(static_cast<std::uint64_t>(exact_instance_node_limit));
  return writer.digest();
}

std::vector<std::byte> ContainmentPolicy::encode() const {
  CanonicalWriter writer;
  writer.strong(generation);
  writer.boolean(require_failure_source_inclusion);
  writer.boolean(allow_protected_inclusion);
  writer.boolean(minimize_protected_inclusions);
  writer.u64(max_boundary_weight);
  writer.u64(static_cast<std::uint64_t>(max_boundary_members));
  writer.u64(exact_search_budget);
  writer.u64(heuristic_budget);
  writer.u64(static_cast<std::uint64_t>(exact_instance_node_limit));
  return writer.data();
}

Result<ContainmentPolicy> ContainmentPolicy::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  ContainmentPolicy policy;
  policy.generation = reader.strong<PolicyGenerationTag>();
  policy.require_failure_source_inclusion = reader.boolean();
  policy.allow_protected_inclusion = reader.boolean();
  policy.minimize_protected_inclusions = reader.boolean();
  policy.max_boundary_weight = reader.u64();
  policy.max_boundary_members = static_cast<std::size_t>(reader.u64());
  policy.exact_search_budget = reader.u64();
  policy.heuristic_budget = reader.u64();
  policy.exact_instance_node_limit = static_cast<std::size_t>(reader.u64());
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  const VoidResult valid = validate_policy(policy);
  if (!valid.ok()) {
    return valid.status();
  }
  return policy;
}

VoidResult validate_policy(const ContainmentPolicy& policy) {
  if (policy.generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "policy generation must be non-zero"};
  }
  if (policy.max_boundary_members == 0 || policy.max_boundary_members > kMaxBoundaryMembers) {
    return Status{StatusCode::InvalidArgument, "policy member bound out of range"};
  }
  if (policy.max_boundary_weight == 0 || policy.max_boundary_weight > kMaxPlanWeightSum) {
    return Status{StatusCode::InvalidArgument, "policy weight bound out of range"};
  }
  if (policy.exact_search_budget == 0 || policy.heuristic_budget == 0) {
    return Status{StatusCode::InvalidArgument, "policy search budget must be non-zero"};
  }
  if (policy.exact_instance_node_limit == 0 || policy.exact_instance_node_limit > kMaxTopologyNodes) {
    return Status{StatusCode::InvalidArgument, "policy instance node limit out of range"};
  }
  return ok_result();
}

ContainmentPolicy default_policy() {
  ContainmentPolicy policy;
  policy.generation = PolicyGeneration{1};
  return policy;
}

}  // namespace fcfn::model
