// FCFN - dependency topology and propagation evidence.
//
// The topology is a definition, not liveness: it survives restart and carries a
// generation. Every propagation edge carries its own evidence class and the
// evidence generation that classified it. Edges classified UNKNOWN are never
// assumed absent; they are included conservatively in every safety claim.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_TOPOLOGY_HPP
#define FCFN_MODEL_TOPOLOGY_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/limits.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::model {

/// Dense node index inside a materialised topology.
using NodeIndex = std::uint32_t;
/// Dense edge index inside a materialised topology.
using EdgeIndex = std::uint32_t;

inline constexpr NodeIndex kInvalidNode = static_cast<NodeIndex>(0xffffffffu);

/// Evidence class of a propagation edge.
enum class EdgeEvidence : std::uint8_t {
  /// Propagation is known possible at the recorded generation.
  Proven = 0,
  /// Propagation may or may not be possible. Conservatively treated as possible.
  Unknown = 1,
  /// Propagation is known impossible at the recorded generation.
  Refuted = 2,
};

[[nodiscard]] const char* to_string(EdgeEvidence value) noexcept;
[[nodiscard]] bool parse_edge_evidence(std::string_view token, EdgeEvidence& out) noexcept;

/// How complete the adjacency information for a node is believed to be.
enum class AdjacencyCompleteness : std::uint8_t {
  /// The recorded edges are believed to be the complete adjacency.
  Complete = 0,
  /// Some adjacency may be missing; safety claims over this node degrade.
  Partial = 1,
  /// Adjacency is unknown; safety claims over this node degrade.
  Unknown = 2,
};

[[nodiscard]] const char* to_string(AdjacencyCompleteness value) noexcept;
[[nodiscard]] bool parse_adjacency_completeness(std::string_view token, AdjacencyCompleteness& out) noexcept;

/// External description of one node.
struct NodeSpec {
  ResourceId id{};
  /// Eligibility: may this runtime contain (isolate) the resource?
  bool containable{false};
  /// Containment cost. Blast-radius accounting minimises the sum of weights.
  std::uint64_t weight{1};
  /// Protected obligation: must remain healthy; inclusion requires a reason.
  bool protected_obligation{false};
  AdjacencyCompleteness completeness{AdjacencyCompleteness::Complete};
};

/// External description of one propagation edge: failure at "from" may reach "to".
struct EdgeSpec {
  ResourceId from{};
  ResourceId to{};
  EdgeEvidence evidence{EdgeEvidence::Unknown};
  EvidenceGeneration generation{};
};

/// External description of a whole topology.
struct TopologySpec {
  TopologyGeneration generation{};
  std::vector<NodeSpec> nodes{};
  std::vector<EdgeSpec> edges{};
};

/// Immutable, validated topology definition.
class Topology {
 public:
  Topology() = default;

  /// Validate and materialise. Rejects: out-of-bound sizes, empty/duplicate node
  /// ids, unknown edge endpoints, duplicate edges, self loops, zero generation,
  /// zero or out-of-range weights, and zero edge evidence generations.
  [[nodiscard]] static Result<Topology> build(TopologySpec spec);

  [[nodiscard]] const TopologyGeneration& generation() const noexcept { return generation_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

  [[nodiscard]] std::optional<NodeIndex> find(const ResourceId& id) const;
  [[nodiscard]] const ResourceId& resource(NodeIndex index) const { return nodes_[index].id; }
  [[nodiscard]] const NodeSpec& node(NodeIndex index) const { return nodes_[index]; }

  struct Edge {
    NodeIndex from{kInvalidNode};
    NodeIndex to{kInvalidNode};
    EdgeEvidence evidence{EdgeEvidence::Unknown};
    EvidenceGeneration generation{};
  };

  [[nodiscard]] const Edge& edge_by_index(EdgeIndex index) const { return edges_[index]; }
  [[nodiscard]] std::span<const EdgeIndex> out_edges(NodeIndex index) const;
  [[nodiscard]] std::span<const EdgeIndex> in_edges(NodeIndex index) const;

  /// Canonical digest of the whole definition (node ids, weights, eligibility,
  /// completeness, and every edge with its evidence class and generation).
  [[nodiscard]] const Digest& definition_digest() const noexcept { return definition_digest_; }

  /// Canonical byte encoding of the definition.
  [[nodiscard]] std::vector<std::byte> encode() const;

  /// Decode a canonical encoding produced by encode(). Total: malformed input
  /// yields a classified Status and never a partially built topology.
  [[nodiscard]] static Result<Topology> decode(std::span<const std::byte> bytes);

 private:
  void finalize();

  TopologyGeneration generation_{};
  std::vector<NodeSpec> nodes_{};
  std::vector<Edge> edges_{};
  std::vector<std::vector<EdgeIndex>> out_{};
  std::vector<std::vector<EdgeIndex>> in_{};
  std::unordered_map<std::string, NodeIndex> lookup_{};
  Digest definition_digest_{};
};

}  // namespace fcfn::model

#endif  // FCFN_MODEL_TOPOLOGY_HPP
