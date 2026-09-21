// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/engine/graph.hpp"

#include <algorithm>
#include <utility>

namespace fcfn::engine {

PropagationGraph PropagationGraph::build(const model::Topology& topology) {
  PropagationGraph graph;
  graph.topology_ = &topology;

  const std::size_t nodes = topology.node_count();
  graph.conservative_.assign(nodes, {});
  graph.conservative_in_.assign(nodes, {});
  graph.proven_.assign(nodes, {});
  graph.touches_unknown_.assign(nodes, 0);

  for (std::size_t i = 0; i < topology.edge_count(); ++i) {
    const model::Topology::Edge& edge = topology.edge_by_index(static_cast<model::EdgeIndex>(i));
    if (edge.evidence == model::EdgeEvidence::Refuted) {
      continue;
    }
    graph.conservative_[edge.from].push_back(edge.to);
    graph.conservative_in_[edge.to].push_back(edge.from);
    ++graph.conservative_edge_count_;
    if (edge.evidence == model::EdgeEvidence::Proven) {
      graph.proven_[edge.from].push_back(edge.to);
      ++graph.proven_edge_count_;
    } else {
      graph.touches_unknown_[edge.from] = 1;
      graph.touches_unknown_[edge.to] = 1;
      graph.unknown_edges_.emplace_back(edge.from, edge.to);
      ++graph.unknown_edge_count_;
    }
  }

  // Topology edges are stored sorted by (from, to), so adjacency lists are
  // already in canonical ascending order and unknown_edges_ is sorted.
  return graph;
}

bool PropagationGraph::has_unknown_edge(model::NodeIndex from, model::NodeIndex to) const noexcept {
  return std::binary_search(unknown_edges_.begin(), unknown_edges_.end(), std::make_pair(from, to));
}

}  // namespace fcfn::engine
