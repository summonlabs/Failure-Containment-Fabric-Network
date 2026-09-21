// FCFN test support: deterministic SYNTHETIC fixtures.
//
// Every graph produced here is synthetic and generated from an explicit seed.
// Nothing in this file observes or claims physical switch, NIC, RDMA, or
// multi-node behaviour.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_FIXTURES_HPP
#define FCFN_TEST_FIXTURES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fcfn/model/topology.hpp"

namespace fcfn::test {

/// Deterministic xorshift64 generator: identical across platforms and runs.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  /// Uniform value in [0, bound).
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

struct SyntheticGraphOptions {
  std::size_t nodes{10};
  std::size_t edges{16};
  std::uint32_t containable_percent{80};
  std::uint32_t unknown_percent{30};
  std::uint32_t partial_adjacency_percent{0};
  std::uint64_t max_weight{5};
  std::size_t protected_count{1};
  std::uint64_t seed{1};
  std::uint64_t topology_generation{1};
  /// Insert a cycle through the source when true.
  bool add_cycle{false};
};

/// Random layered DAG-ish structure with cycles, shared-risk nodes, and a mix of
/// PROVEN and UNKNOWN edges, derived entirely from the seed.
[[nodiscard]] model::TopologySpec synthetic_topology(const SyntheticGraphOptions& options);

/// Layered graph with a known minimum vertex cut: every intermediate layer is
/// fully connected to the next, so the optimum is either the source itself or
/// the cheapest complete layer.
[[nodiscard]] model::TopologySpec layered_topology(std::size_t layers, std::size_t width,
                                                   std::uint64_t seed, std::uint64_t generation);

/// Node name helper: "n0", "n1", ...
[[nodiscard]] std::string node_name(std::size_t index);

/// Build a topology from a specification, failing the test when invalid.
[[nodiscard]] model::Topology require_topology(model::TopologySpec spec);

}  // namespace fcfn::test

#endif  // FCFN_TEST_FIXTURES_HPP
