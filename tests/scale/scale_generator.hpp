// FCFN scale suite: purpose-built synthetic generator.
//
// The generator produces a funnel: a source that can only leave through a narrow
// waist of "waist" nodes, followed by a wide sparse bulk, ending in a protected
// obligation. Every source-to-obligation path therefore crosses the waist, so
// the minimum containment scope is exactly the waist - a known answer that does
// not grow with the graph - while the graph itself grows to tens of thousands of
// nodes. This is synthetic structure only: no physical fabric is observed.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TESTS_SCALE_GENERATOR_HPP
#define FCFN_TESTS_SCALE_GENERATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fcfn/model/topology.hpp"
#include "solver/case_builder.hpp"

namespace fcfn::test::scale_cases {

struct FunnelOptions {
  /// Total node count, including source, waist, bulk, and obligation.
  std::size_t nodes{2000};
  /// Nodes in the waist: the minimum containment scope.
  std::size_t waist{4};
  /// Nodes per bulk layer.
  std::size_t layer_width{64};
  /// Out-degree of every bulk node.
  std::size_t fan_out{2};
  /// Weight of every bulk node (the waist stays at weight 1).
  std::uint64_t bulk_weight{7};
};

struct FunnelShape {
  std::size_t nodes{0};
  std::size_t edges{0};
  std::size_t waist{0};
  std::size_t layers{0};
  std::vector<std::string> waist_ids{};
};

inline std::string index_name(const char* prefix, std::size_t index) {
  return std::string(prefix) + std::to_string(index);
}

/// Build the funnel specification together with its shape description.
inline model::TopologySpec funnel_topology(const FunnelOptions& options, FunnelShape& shape) {
  model::TopologySpec spec;
  spec.generation = model::TopologyGeneration{1};
  const std::size_t waist = options.waist;
  const std::size_t width = options.layer_width;
  // Keep whole layers only: the requested size is an upper bound.
  const std::size_t layers = (options.nodes - waist - 2) / width;
  const std::size_t bulk = layers * width;

  model::NodeSpec source;
  source.id = model::ResourceId::unchecked("s0");
  source.containable = false;
  source.weight = 1;
  spec.nodes.push_back(std::move(source));

  shape.waist_ids.clear();
  for (std::size_t i = 0; i < waist; ++i) {
    model::NodeSpec node;
    node.id = model::ResourceId::unchecked(index_name("w", i));
    node.containable = true;
    node.weight = 1;
    spec.nodes.push_back(std::move(node));
    shape.waist_ids.push_back(index_name("w", i));
  }

  for (std::size_t i = 0; i < bulk; ++i) {
    model::NodeSpec node;
    node.id = model::ResourceId::unchecked(index_name("b", i));
    node.containable = true;
    node.weight = options.bulk_weight;
    spec.nodes.push_back(std::move(node));
  }

  model::NodeSpec obligation;
  obligation.id = model::ResourceId::unchecked("p0");
  obligation.containable = false;
  obligation.weight = 1;
  obligation.protected_obligation = true;
  spec.nodes.push_back(std::move(obligation));

  auto add = [&spec](const std::string& from, const std::string& to) {
    model::EdgeSpec edge;
    edge.from = model::ResourceId::unchecked(from);
    edge.to = model::ResourceId::unchecked(to);
    edge.evidence = model::EdgeEvidence::Proven;
    edge.generation = model::EvidenceGeneration{1};
    spec.edges.push_back(std::move(edge));
  };

  // Source -> waist, and waist -> the whole first bulk layer.
  for (std::size_t i = 0; i < waist; ++i) {
    add("s0", index_name("w", i));
  }
  for (std::size_t i = 0; i < waist; ++i) {
    for (std::size_t q = 0; q < width; ++q) {
      add(index_name("w", i), index_name("b", q));
    }
  }
  // Bulk layer L -> layer L + 1 (or the obligation after the last layer).
  for (std::size_t layer = 0; layer < layers; ++layer) {
    for (std::size_t q = 0; q < width; ++q) {
      const std::size_t from = layer * width + q;
      if (layer + 1 == layers) {
        add(index_name("b", from), "p0");
        continue;
      }
      for (std::size_t step = 0; step < options.fan_out; ++step) {
        const std::size_t next = (layer + 1) * width + ((q + step) % width);
        add(index_name("b", from), index_name("b", next));
      }
    }
  }

  shape.nodes = spec.nodes.size();
  shape.edges = spec.edges.size();
  shape.waist = waist;
  shape.layers = layers;
  return spec;
}

}  // namespace fcfn::test::scale_cases

#endif  // FCFN_TESTS_SCALE_GENERATOR_HPP
