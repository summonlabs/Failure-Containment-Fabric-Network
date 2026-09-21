// FCFN - authority vectors and generation binding.
//
// An authority vector binds everything that made a decision legal: the
// coordinator epoch, the process incarnation, the policy generation, the
// topology generation, the exact evidence revision, and the issuing sequence.
// Any change to an authority-bearing dependency invalidates the vector.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_AUTHORITY_HPP
#define FCFN_MODEL_AUTHORITY_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fcfn/core/ids.hpp"
#include "fcfn/model/ids.hpp"
#include "fcfn/core/result.hpp"

namespace fcfn::model {

/// Monotone evidence revision counter: incremented on every accepted detection.
using EvidenceRevision = StrongId<struct EvidenceRevisionTag>;

/// Identity of an issued authorization. Authorization is a lease bound to an
/// authority vector: it never survives a restart and is never inferred.
struct AuthorizationIdTag;
using AuthorizationId = StrongId<AuthorizationIdTag>;

/// Complete binding of the authority that made a decision legal.
struct AuthorityVector {
  CoordinatorEpoch epoch{};
  BootIdentity boot{};
  PolicyGeneration policy{};
  TopologyGeneration topology{};
  EvidenceRevision evidence_revision{};
  Digest evidence_digest{};
  Sequence issued_sequence{};

  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string render() const;
  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<AuthorityVector> decode(std::span<const std::byte> bytes);
};

/// Result of validating a presented vector against current authority.
enum class AuthorityState : std::uint8_t {
  Valid = 0,
  StaleEpoch = 1,
  StaleBoot = 2,
  StalePolicy = 3,
  StaleTopology = 4,
  StaleEvidence = 5,
  Fenced = 6,
};

[[nodiscard]] const char* to_string(AuthorityState value) noexcept;

struct AuthorityCheck {
  AuthorityState state{AuthorityState::Valid};
  bool ok() const noexcept { return state == AuthorityState::Valid; }
  [[nodiscard]] StatusCode status_code() const noexcept;
  [[nodiscard]] const char* reason() const noexcept;
};

/// Validate a presented vector against the runtime's current authority vector.
/// A mismatched boot is reported as Fenced: pre-restart authority is not current.
[[nodiscard]] AuthorityCheck validate_authority(const AuthorityVector& presented, const AuthorityVector& current) noexcept;

}  // namespace fcfn::model

#endif  // FCFN_MODEL_AUTHORITY_HPP
