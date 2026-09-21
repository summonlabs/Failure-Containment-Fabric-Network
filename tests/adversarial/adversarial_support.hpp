// FCFN adversarial suite support: hostile-input fixtures.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_ADVERSARIAL_SUPPORT_HPP
#define FCFN_TEST_ADVERSARIAL_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/model/topology.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/store/store.hpp"
#include "fixtures.hpp"
#include "temp_dir.hpp"
#include "test_harness.hpp"

namespace fcfn::test::adversarial {

using fcfn::model::ContainmentBoundary;
using fcfn::model::ContainmentPlan;
using fcfn::model::ContainmentPolicy;
using fcfn::model::EvidenceState;
using fcfn::model::FailureObservation;
using fcfn::model::ResourceId;
using fcfn::model::TopologySpec;
using fcfn::runtime::ContainmentRuntime;
using fcfn::runtime::RuntimeConfig;
using fcfn::StatusCode;

using fcfn::kMaxBoundaryMembers;
using fcfn::kMaxNodeWeight;
using fcfn::kMaxPlanWeightSum;
using fcfn::kMaxTopologyNodes;
using fcfn::Result;
using fcfn::VoidResult;

inline void fail_here(const char* what, const std::string& detail) {
  ::fcfn::test::fail(__FILE__, __LINE__, std::string(what) + ": " + detail);
}

inline void expect_code(const char* what, StatusCode actual, StatusCode expected) {
  if (actual != expected) {
    fail_here(what, std::string("expected ") + to_string(expected) + " but got " +
                        to_string(actual));
  }
}

inline void expect_void(const char* what, const VoidResult& result, StatusCode expected) {
  expect_code(what, result.status().code(), expected);
}

template <class T>
void expect_result(const char* what, const Result<T>& result, StatusCode expected) {
  expect_code(what, result.status().code(), expected);
}

/// Layered fixture whose failure source is containable, so containment over it
/// can be proven rather than being degraded by an uncontainable source.
inline TopologySpec contained_source_topology(std::uint64_t generation) {
  TopologySpec spec = layered_topology(4, 2, 3, generation);
  spec.nodes[0].containable = true;  // "l0c0", the failure source
  return spec;
}

/// Layered fixture whose failure source cannot be contained: containment must
/// cut a whole intermediate layer.
inline TopologySpec uncontainable_source_topology(std::uint64_t generation) {
  return layered_topology(4, 3, 7, generation);
}

inline RuntimeConfig non_durable_config() {
  RuntimeConfig config;
  config.persist = false;
  config.policy = fcfn::model::default_policy();
  config.topology = contained_source_topology(1);
  config.require_evidence_confirmation_in_current_boot = true;
  return config;
}

inline RuntimeConfig durable_config(const std::filesystem::path& root) {
  RuntimeConfig config = non_durable_config();
  config.store_root = root;
  config.persist = true;
  return config;
}

inline std::unique_ptr<ContainmentRuntime> open_or_fail(const RuntimeConfig& config,
                                                        fcfn::Clock* clock) {
  Result<std::unique_ptr<ContainmentRuntime>> opened = ContainmentRuntime::open(config, clock);
  if (!opened.ok()) {
    fail_here("runtime open", opened.status().to_string());
  }
  return std::move(opened.value());
}

inline FailureObservation observation(const char* resource, EvidenceState state,
                                      std::uint64_t generation, std::uint64_t digest_hi,
                                      std::uint64_t digest_lo) {
  FailureObservation value;
  value.resource = ResourceId::unchecked(resource);
  value.state = state;
  value.generation = fcfn::model::EvidenceGeneration{generation};
  value.source_digest = fcfn::model::Digest{digest_hi, digest_lo};
  return value;
}

inline FailureObservation source_observation(std::uint64_t generation, std::uint64_t digest_hi,
                                             std::uint64_t digest_lo) {
  return observation("l0c0", EvidenceState::Present, generation, digest_hi, digest_lo);
}

inline void require_ok(const char* what, const VoidResult& result) {
  if (!result.ok()) {
    fail_here(what, result.status().to_string());
  }
}

inline std::uint64_t source_detection(ContainmentRuntime& runtime, std::uint64_t generation) {
  const Result<fcfn::model::DetectionRecord> recorded =
      runtime.record_detection(source_observation(generation, 0xa1a1, 0xb2b2));
  if (!recorded.ok()) {
    fail_here("record_detection", recorded.status().to_string());
  }
  return recorded.value().sequence.value();
}

/// Every identifier form the canonical domain must refuse.
inline const std::vector<std::string>& hostile_ids() {
  static const std::vector<std::string> ids{
      std::string(),
      " ",
      "a b",
      "a/b",
      "a\\b",
      "..",
      ".",
      "a..b",
      "..a",
      "a..",
      "-leading-dash",
      "trailing-dash-",
      ".leading-dot",
      "trailing-dot.",
      "a\tb",
      "a\nb",
      "a\rb",
      "semi;colon",
      "pipe|char",
      "star*",
      "question?",
      "quote\"char",
      "angle<bracket>",
      "percent%20",
      std::string(64, 'x'),
      std::string(4096, 'y'),
      "caf\xc3\xa9",
      std::string("embedded\0nul", 12),
  };
  return ids;
}

/// The same hostile forms, restricted to those a container can carry.
inline const std::vector<std::string>& non_empty_hostile_ids() {
  static const std::vector<std::string> ids = [] {
    std::vector<std::string> out;
    for (const std::string& id : hostile_ids()) {
      if (!id.empty()) {
        out.push_back(id);
      }
    }
    return out;
  }();
  return ids;
}

}  // namespace fcfn::test::adversarial

#endif  // FCFN_TEST_ADVERSARIAL_SUPPORT_HPP
