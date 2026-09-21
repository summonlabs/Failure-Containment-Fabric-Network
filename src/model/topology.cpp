// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/topology.hpp"

#include <algorithm>
#include <utility>

namespace fcfn::model {
namespace {

struct EdgeToken {
  EdgeEvidence value;
  const char* token;
};

constexpr EdgeToken kEdgeTokens[] = {
    {EdgeEvidence::Proven, "proven"},
    {EdgeEvidence::Unknown, "unknown"},
    {EdgeEvidence::Refuted, "refuted"},
};

struct CompletenessToken {
  AdjacencyCompleteness value;
  const char* token;
};

constexpr CompletenessToken kCompletenessTokens[] = {
    {AdjacencyCompleteness::Complete, "complete"},
    {AdjacencyCompleteness::Partial, "partial"},
    {AdjacencyCompleteness::Unknown, "unknown"},
};

}  // namespace

const char* to_string(EdgeEvidence value) noexcept {
  for (const EdgeToken& entry : kEdgeTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_edge_evidence";
}

bool parse_edge_evidence(std::string_view token, EdgeEvidence& out) noexcept {
  for (const EdgeToken& entry : kEdgeTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

const char* to_string(AdjacencyCompleteness value) noexcept {
  for (const CompletenessToken& entry : kCompletenessTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_adjacency_completeness";
}

bool parse_adjacency_completeness(std::string_view token, AdjacencyCompleteness& out) noexcept {
  for (const CompletenessToken& entry : kCompletenessTokens) {
    if (token == entry.token) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

Result<Topology> Topology::build(TopologySpec spec) {
  if (spec.generation.is_zero()) {
    return Status{StatusCode::InvalidArgument, "topology generation must be non-zero"};
  }
  if (spec.nodes.size() > kMaxTopologyNodes) {
    return Status{StatusCode::LimitExceeded, "topology node count exceeds bound"};
  }
  if (spec.edges.size() > kMaxTopologyEdges) {
    return Status{StatusCode::LimitExceeded, "topology edge count exceeds bound"};
  }
  if (spec.nodes.empty()) {
    return Status{StatusCode::InvalidArgument, "topology must contain at least one node"};
  }

  // Canonical node order makes the index space independent of insertion order.
  std::sort(spec.nodes.begin(), spec.nodes.end(), [](const NodeSpec& a, const NodeSpec& b) {
    return a.id < b.id;
  });

  Topology topology;
  topology.generation_ = spec.generation;
  topology.nodes_ = std::move(spec.nodes);

  for (std::size_t i = 0; i < topology.nodes_.size(); ++i) {
    const NodeSpec& node = topology.nodes_[i];
    // The same canonical grammar the canonical decoder enforces, so a definition
    // that is accepted here can always be encoded and recovered.
    if (!is_valid_resource_id(node.id.value())) {
      return Status{StatusCode::InvalidArgument, "node id is not in canonical form"};
    }
    if (i > 0 && topology.nodes_[i - 1].id == node.id) {
      return Status{StatusCode::InvalidArgument, "duplicate node id"};
    }
    if (node.weight == 0 || node.weight > kMaxNodeWeight) {
      return Status{StatusCode::InvalidArgument, "node weight out of range"};
    }
    if (node.protected_obligation && node.containable) {
      return Status{StatusCode::InvalidArgument, "protected obligation may not be pre-marked containable"};
    }
    topology.lookup_.emplace(node.id.value(), static_cast<NodeIndex>(i));
  }

  struct RawEdge {
    NodeIndex from{kInvalidNode};
    NodeIndex to{kInvalidNode};
    EdgeEvidence evidence{EdgeEvidence::Unknown};
    EvidenceGeneration generation{};
  };

  std::vector<RawEdge> raw;
  raw.reserve(spec.edges.size());
  for (const EdgeSpec& edge : spec.edges) {
    const auto from = topology.lookup_.find(edge.from.value());
    const auto to = topology.lookup_.find(edge.to.value());
    if (from == topology.lookup_.end()) {
      return Status{StatusCode::NotFound, "edge endpoint is not a known node"};
    }
    if (to == topology.lookup_.end()) {
      return Status{StatusCode::NotFound, "edge endpoint is not a known node"};
    }
    if (from->second == to->second) {
      return Status{StatusCode::InvalidArgument, "self-referential propagation edge"};
    }
    if (edge.generation.is_zero()) {
      return Status{StatusCode::InvalidArgument, "edge evidence generation must be non-zero"};
    }
    raw.push_back(RawEdge{from->second, to->second, edge.evidence, edge.generation});
  }

  std::sort(raw.begin(), raw.end(), [](const RawEdge& a, const RawEdge& b) {
    if (a.from != b.from) {
      return a.from < b.from;
    }
    return a.to < b.to;
  });

  for (std::size_t i = 1; i < raw.size(); ++i) {
    if (raw[i].from == raw[i - 1].from && raw[i].to == raw[i - 1].to) {
      return Status{StatusCode::AlreadyExists, "duplicate propagation edge"};
    }
  }

  topology.edges_.reserve(raw.size());
  for (const RawEdge& edge : raw) {
    topology.edges_.push_back(Edge{edge.from, edge.to, edge.evidence, edge.generation});
  }

  topology.finalize();
  return topology;
}

void Topology::finalize() {
  out_.assign(nodes_.size(), {});
  in_.assign(nodes_.size(), {});
  for (std::size_t i = 0; i < edges_.size(); ++i) {
    const auto index = static_cast<EdgeIndex>(i);
    out_[edges_[i].from].push_back(index);
    in_[edges_[i].to].push_back(index);
  }
  const std::vector<std::byte> encoded = encode();
  DigestBuilder builder;
  builder.update(std::span<const std::byte>(encoded.data(), encoded.size()));
  definition_digest_ = builder.finish();
}

std::optional<NodeIndex> Topology::find(const ResourceId& id) const {
  const auto it = lookup_.find(id.value());
  if (it == lookup_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::span<const EdgeIndex> Topology::out_edges(NodeIndex index) const {
  return {out_[index].data(), out_[index].size()};
}

std::span<const EdgeIndex> Topology::in_edges(NodeIndex index) const {
  return {in_[index].data(), in_[index].size()};
}

std::vector<std::byte> Topology::encode() const {
  CanonicalWriter writer;
  writer.strong(generation_);
  writer.u32(static_cast<std::uint32_t>(nodes_.size()));
  for (const NodeSpec& node : nodes_) {
    writer.resource_id(node.id);
    writer.boolean(node.containable);
    writer.u64(node.weight);
    writer.boolean(node.protected_obligation);
    writer.u8(static_cast<std::uint8_t>(node.completeness));
  }
  writer.u32(static_cast<std::uint32_t>(edges_.size()));
  for (const Edge& edge : edges_) {
    writer.u32(edge.from);
    writer.u32(edge.to);
    writer.u8(static_cast<std::uint8_t>(edge.evidence));
    writer.strong(edge.generation);
  }
  return writer.data();
}

Result<Topology> Topology::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  TopologySpec spec;
  spec.generation = reader.strong<TopologyGenerationTag>();
  const std::uint32_t node_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (node_count > kMaxTopologyNodes) {
    return Status{StatusCode::LimitExceeded, "encoded node count exceeds bound"};
  }
  spec.nodes.reserve(node_count);
  for (std::uint32_t i = 0; i < node_count; ++i) {
    NodeSpec node;
    auto id = reader.resource_id();
    if (!reader.ok()) {
      return reader.status();
    }
    if (!id.ok()) {
      return id.status();
    }
    node.id = std::move(id.value());
    node.containable = reader.boolean();
    node.weight = reader.u64();
    node.protected_obligation = reader.boolean();
    const std::uint8_t completeness = reader.u8();
    AdjacencyCompleteness parsed{};
    if (!reader.ok() || completeness > static_cast<std::uint8_t>(AdjacencyCompleteness::Unknown)) {
      reader.poison(StatusCode::InvalidArgument, "invalid adjacency completeness value");
      return reader.status();
    }
    parsed = static_cast<AdjacencyCompleteness>(completeness);
    node.completeness = parsed;
    spec.nodes.push_back(std::move(node));
  }

  const std::uint32_t edge_count = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (edge_count > kMaxTopologyEdges) {
    return Status{StatusCode::LimitExceeded, "encoded edge count exceeds bound"};
  }
  spec.edges.reserve(edge_count);
  for (std::uint32_t i = 0; i < edge_count; ++i) {
    const std::uint32_t from = reader.u32();
    const std::uint32_t to = reader.u32();
    const std::uint8_t evidence = reader.u8();
    const EvidenceGeneration generation = reader.strong<EvidenceGenerationTag>();
    if (!reader.ok()) {
      return reader.status();
    }
    if (from >= node_count || to >= node_count) {
      return Status{StatusCode::InvalidArgument, "encoded edge endpoint out of range"};
    }
    if (evidence > static_cast<std::uint8_t>(EdgeEvidence::Refuted)) {
      return Status{StatusCode::InvalidArgument, "invalid edge evidence value"};
    }
    EdgeSpec edge;
    edge.from = spec.nodes[from].id;
    edge.to = spec.nodes[to].id;
    edge.evidence = static_cast<EdgeEvidence>(evidence);
    edge.generation = generation;
    spec.edges.push_back(std::move(edge));
  }
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return Topology::build(std::move(spec));
}

}  // namespace fcfn::model
