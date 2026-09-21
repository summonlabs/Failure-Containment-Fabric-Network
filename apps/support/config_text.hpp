// FCFN app support: line-oriented configuration text formats.
//
// These formats exist so examples, tools, and proofs can build topology,
// evidence, and policy inputs without a JSON parser dependency. They are
// documented in the README and are intentionally boring:
//
//   topology file
//     generation <n>
//     node <id> containable=<0|1> weight=<n> protected=<0|1> completeness=<complete|partial|unknown>
//     edge <from> <to> evidence=<proven|unknown|refuted> generation=<n>
//
//   evidence file
//     <resource> <present|absent|unknown> <generation> [digest=<hex32>]
//
//   policy file
//     generation <n>
//     <field> <value>
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_APPS_SUPPORT_CONFIG_TEXT_HPP
#define FCFN_APPS_SUPPORT_CONFIG_TEXT_HPP

#include <filesystem>
#include <string>
#include <vector>

#include "fcfn/core/result.hpp"
#include "fcfn/model/evidence.hpp"
#include "fcfn/model/policy.hpp"
#include "fcfn/model/topology.hpp"

namespace fcfn::apps {

[[nodiscard]] Result<model::TopologySpec> parse_topology_file(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<model::FailureObservation>> parse_evidence_file(
    const std::filesystem::path& path);
[[nodiscard]] Result<model::ContainmentPolicy> parse_policy_file(const std::filesystem::path& path);

/// Write a topology definition in the same format (used to export fixtures).
[[nodiscard]] VoidResult write_topology_file(const std::filesystem::path& path,
                                             const model::Topology& topology);

/// Extract an unsigned integer field from a deterministic JSON document.
/// Returns false when the field is absent or malformed.
[[nodiscard]] bool json_field_u64(const std::string& document, const std::string& key,
                                  std::uint64_t& out);

}  // namespace fcfn::apps

#endif  // FCFN_APPS_SUPPORT_CONFIG_TEXT_HPP
