// FCFN codec suite: systematic single-byte corruption.
//
// Product proposition proved here: a canonical document has no slack. Changing
// any one byte either fails a structural/integrity check or yields a different
// object that re-encodes consistently. No corruption is silently absorbed into
// the original object, and no damaged document is normalised back into a valid
// decision.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "codec_support.hpp"

namespace {

using namespace fcfn::test::codec;

constexpr std::size_t kStatusSlots = 32;

struct SweepCounts {
  std::size_t bytes{0};
  std::size_t corruptions{0};
  std::size_t rejected{0};
  std::size_t changed{0};
};

/// Replace every byte with every other byte value and classify the outcome.
template <class T>
SweepCounts sweep_single_byte_corruptions(const Codec<T>& codec, const T& value) {
  const std::vector<std::byte> original = codec.encode(value);
  std::array<std::size_t, kStatusSlots> refusals{};
  SweepCounts counts;
  counts.bytes = original.size();
  counts.corruptions = original.size() * 255;

  std::vector<std::byte> corrupted = original;
  for (std::size_t index = 0; index < original.size(); ++index) {
    const auto original_value = static_cast<std::uint8_t>(original[index]);
    for (int candidate = 0; candidate < 256; ++candidate) {
      const auto replacement = static_cast<std::uint8_t>(candidate);
      if (replacement == original_value) {
        continue;
      }
      corrupted[index] = static_cast<std::byte>(replacement);
      const Result<T> decoded = codec.decode(corrupted);
      if (!decoded.ok()) {
        ++counts.rejected;
        const auto slot = static_cast<std::size_t>(decoded.status().code());
        if (slot < refusals.size()) {
          ++refusals[slot];
        }
        continue;
      }
      ++counts.changed;
      const std::vector<std::byte> reencoded = codec.encode(decoded.value());
      if (same_bytes(reencoded, original)) {
        ::fcfn::test::fail(
            __FILE__, __LINE__,
            std::string(codec.name) + " silently absorbed a corruption at byte index " +
                std::to_string(index) + " (" + std::to_string(original_value) + " -> " +
                std::to_string(replacement) + "): the decoded object re-encodes to the original " +
                hex_of(original));
      }
      const Result<T> again = codec.decode(reencoded);
      if (!again.ok() || !same_bytes(codec.encode(again.value()), reencoded)) {
        ::fcfn::test::fail(__FILE__, __LINE__,
                           std::string(codec.name) + " decoded a corrupted document at index " +
                               std::to_string(index) + " into an object that does not re-encode " +
                               "consistently: " + hex_of(reencoded));
      }
    }
    corrupted[index] = original[index];
  }

  std::printf("  %s: %zu bytes, %zu corruptions, refused=%zu, different-object=%zu\n", codec.name,
              counts.bytes, counts.corruptions, counts.rejected, counts.changed);
  for (std::size_t slot = 0; slot < refusals.size(); ++slot) {
    if (refusals[slot] != 0) {
      std::printf("    refused as %s=%zu\n", to_string(static_cast<StatusCode>(slot)),
                  refusals[slot]);
    }
  }
  if (counts.rejected + counts.changed != counts.corruptions) {
    ::fcfn::test::fail(__FILE__, __LINE__,
                       std::string(codec.name) + " corruption accounting is incomplete");
  }
  return counts;
}

}  // namespace

FCFN_TEST(codec, every_single_byte_corruption_is_refused_or_changes_the_object) {
  (void)sweep_single_byte_corruptions(topology_codec(), sample_topology());
  (void)sweep_single_byte_corruptions(evidence_codec(), sample_evidence());
  (void)sweep_single_byte_corruptions(policy_codec(), sample_policy());
  (void)sweep_single_byte_corruptions(authority_codec(), sample_authority());
  (void)sweep_single_byte_corruptions(boundary_codec(), sample_boundary());
}

FCFN_TEST(codec, a_corrupted_plan_never_decodes) {
  // A plan carries the digest of every decision-bearing field, so a codec-level
  // corruption can never survive: it must be refused, not repaired.
  const SweepCounts counts = sweep_single_byte_corruptions(plan_codec(), sample_plan());
  FCFN_CHECK_EQ(counts.changed, static_cast<std::size_t>(0));
  FCFN_CHECK_EQ(counts.rejected, counts.corruptions);
}

FCFN_TEST(codec, a_corrupted_topology_never_keeps_the_original_definition_digest) {
  const Codec<fcfn::model::Topology>& codec = topology_codec();
  const fcfn::model::Topology value = sample_topology();
  const std::vector<std::byte> original = codec.encode(value);
  std::size_t decoded_count = 0;
  for (std::size_t index = 0; index < original.size(); ++index) {
    // A single fixed mask keeps this case cheap; the exhaustive sweep above
    // covers every replacement value.
    std::vector<std::byte> corrupted = original;
    corrupted[index] = static_cast<std::byte>(static_cast<std::uint8_t>(original[index]) ^ 0x5au);
    const auto decoded = codec.decode(corrupted);
    if (!decoded.ok()) {
      continue;
    }
    ++decoded_count;
    if (decoded.value().definition_digest() == value.definition_digest()) {
      ::fcfn::test::fail(__FILE__, __LINE__,
                         "topology corruption at byte index " + std::to_string(index) +
                             " preserved the definition digest of a different definition");
    }
  }
  FCFN_CHECK(decoded_count > 0);
}
