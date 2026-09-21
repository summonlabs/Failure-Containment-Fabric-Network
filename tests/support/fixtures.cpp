// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fixtures.hpp"

#include <algorithm>
#include <utility>

#include "test_harness.hpp"

namespace fcfn::test {

std::string node_name(std::size_t index) { return "n" + std::to_string(index); }

model::TopologySpec synthetic_topology(const SyntheticGraphOptions& options) {
  Rng rng(options.seed * 0x2545f4914f6cdd1dull + 0x9e3779b97f4a7c15ull);
  model::TopologySpec spec;
  spec.generation = model::TopologyGeneration{options.topology_generation};

  for (std::size_t i = 0; i < options.nodes; ++i) {
    model::NodeSpec node;
    node.id = model::ResourceId::unchecked(node_name(i));
    node.containable = rng.below(100) < options.containable_percent;
    node.weight = 1 + rng.below(options.max_weight);
    node.completeness = rng.below(100) < options.partial_adjacency_percent
                            ? model::AdjacencyCompleteness::Partial
                            : model::AdjacencyCompleteness::Complete;
    spec.nodes.push_back(std::move(node));
  }

  // Protected obligations are chosen from the highest indices so that the source
  // set (low indices) never overlaps them.
  const std::size_t protected_count = std::min(options.protected_count, options.nodes);
  for (std::size_t i = 0; i < protected_count; ++i) {
    const std::size_t index = options.nodes - 1 - i;
    spec.nodes[index].protected_obligation = true;
    spec.nodes[index].containable = false;
  }

  std::size_t attempts = 0;
  const std::size_t attempt_limit = options.edges * 32 + 64;
  while (spec.edges.size() < options.edges && attempts < attempt_limit) {
    ++attempts;
    const std::size_t from = rng.below(options.nodes);
    const std::size_t to = rng.below(options.nodes);
    if (from == to) {
      continue;
    }
    if (from > to && !options.add_cycle) {
      continue;  // keep the base graph acyclic; cycles are added explicitly
    }
    model::EdgeSpec edge;
    edge.from = model::ResourceId::unchecked(node_name(from));
    edge.to = model::ResourceId::unchecked(node_name(to));
    edge.evidence = rng.below(100) < options.unknown_percent ? model::EdgeEvidence::Unknown
                                                             : model::EdgeEvidence::Proven;
    edge.generation = model::EvidenceGeneration{1 + rng.below(4)};
    const bool duplicate = std::any_of(spec.edges.begin(), spec.edges.end(),
                                       [&edge](const model::EdgeSpec& existing) {
                                         return existing.from == edge.from && existing.to == edge.to;
                                       });
    if (!duplicate) {
      spec.edges.push_back(std::move(edge));
    }
  }

  if (options.add_cycle && options.nodes >= 3) {
    // Explicit back edge creating a cycle: n2 -> n0. The generator may already
    // have produced it, so it is added only when absent.
    model::EdgeSpec edge;
    edge.from = model::ResourceId::unchecked(node_name(2));
    edge.to = model::ResourceId::unchecked(node_name(0));
    const bool present = std::any_of(spec.edges.begin(), spec.edges.end(),
                                     [&edge](const model::EdgeSpec& existing) {
                                       return existing.from == edge.from && existing.to == edge.to;
                                     });
    if (!present) {
      edge.evidence = model::EdgeEvidence::Proven;
      edge.generation = model::EvidenceGeneration{1};
      spec.edges.push_back(std::move(edge));
    }
  }
  return spec;
}

model::TopologySpec layered_topology(std::size_t layers, std::size_t width, std::uint64_t seed,
                                     std::uint64_t generation) {
  Rng rng(seed * 0x2545f4914f6cdd1dull + 0x1234567ull);
  model::TopologySpec spec;
  spec.generation = model::TopologyGeneration{generation};
  for (std::size_t layer = 0; layer < layers; ++layer) {
    for (std::size_t column = 0; column < width; ++column) {
      model::NodeSpec node;
      node.id = model::ResourceId::unchecked("l" + std::to_string(layer) + "c" + std::to_string(column));
      node.containable = true;
      node.weight = 1 + rng.below(3);
      node.completeness = model::AdjacencyCompleteness::Complete;
      spec.nodes.push_back(std::move(node));
    }
  }
  // The source cannot be contained: the fixture is designed so that containment
  // must cut a complete intermediate layer.
  spec.nodes[0].containable = false;
  // The last layer holds the protected obligation.
  const std::size_t last = (layers - 1) * width;
  for (std::size_t column = 0; column < width; ++column) {
    spec.nodes[last + column].protected_obligation = true;
    spec.nodes[last + column].containable = false;
  }
  for (std::size_t layer = 0; layer + 1 < layers; ++layer) {
    for (std::size_t column = 0; column < width; ++column) {
      for (std::size_t next_column = 0; next_column < width; ++next_column) {
        model::EdgeSpec edge;
        edge.from = spec.nodes[layer * width + column].id;
        edge.to = spec.nodes[(layer + 1) * width + next_column].id;
        edge.evidence = model::EdgeEvidence::Proven;
        edge.generation = model::EvidenceGeneration{1};
        spec.edges.push_back(std::move(edge));
      }
    }
  }
  return spec;
}

model::Topology require_topology(model::TopologySpec spec) {
  auto topology = model::Topology::build(std::move(spec));
  if (!topology.ok()) {
    throw TestFailure("fixture topology rejected: " + topology.status().to_string());
  }
  return std::move(topology.value());
}

}  // namespace fcfn::test
