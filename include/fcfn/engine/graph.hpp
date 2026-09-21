// FCFN - indexed propagation graph.
//
// Two adjacency views are maintained: the conservative view (PROVEN + UNKNOWN
// edges) used for every safety claim, and the proven view (PROVEN edges only)
// used to classify witnesses. REFUTED edges appear in neither view.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_ENGINE_GRAPH_HPP
#define FCFN_ENGINE_GRAPH_HPP

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "fcfn/model/topology.hpp"

namespace fcfn::engine {

/// Immutable indexed view of a topology's propagation edges.
class PropagationGraph {
 public:
  PropagationGraph() = default;

  [[nodiscard]] static PropagationGraph build(const model::Topology& topology);

  [[nodiscard]] const model::Topology& topology() const noexcept { return *topology_; }
  [[nodiscard]] bool valid() const noexcept { return topology_ != nullptr; }
  [[nodiscard]] std::size_t node_count() const noexcept { return conservative_.size(); }
  [[nodiscard]] std::size_t conservative_edge_count() const noexcept { return conservative_edge_count_; }
  [[nodiscard]] std::size_t proven_edge_count() const noexcept { return proven_edge_count_; }
  [[nodiscard]] std::size_t unknown_edge_count() const noexcept { return unknown_edge_count_; }

  [[nodiscard]] std::span<const model::NodeIndex> conservative_successors(model::NodeIndex node) const {
    return {conservative_[node].data(), conservative_[node].size()};
  }

  [[nodiscard]] std::span<const model::NodeIndex> proven_successors(model::NodeIndex node) const {
    return {proven_[node].data(), proven_[node].size()};
  }

  [[nodiscard]] std::span<const model::NodeIndex> conservative_predecessors(model::NodeIndex node) const {
    return {conservative_in_[node].data(), conservative_in_[node].size()};
  }

  /// True when at least one UNKNOWN edge enters or leaves the node.
  [[nodiscard]] bool touches_unknown_edge(model::NodeIndex node) const noexcept {
    return touches_unknown_[node] != 0;
  }

  /// True when a specific UNKNOWN edge exists (used by witness classification).
  [[nodiscard]] bool has_unknown_edge(model::NodeIndex from, model::NodeIndex to) const noexcept;

 private:
  const model::Topology* topology_{nullptr};
  std::vector<std::vector<model::NodeIndex>> conservative_{};
  std::vector<std::vector<model::NodeIndex>> conservative_in_{};
  std::vector<std::vector<model::NodeIndex>> proven_{};
  std::vector<std::uint8_t> touches_unknown_{};
  std::vector<std::pair<model::NodeIndex, model::NodeIndex>> unknown_edges_{};
  std::size_t conservative_edge_count_{0};
  std::size_t proven_edge_count_{0};
  std::size_t unknown_edge_count_{0};
};

}  // namespace fcfn::engine

#endif  // FCFN_ENGINE_GRAPH_HPP
