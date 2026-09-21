// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/model/authority.hpp"

#include <cstdio>

#include "fcfn/core/canonical.hpp"
#include "fcfn/model/explanation.hpp"

namespace fcfn::model {
namespace {

struct AuthorityToken {
  AuthorityState value;
  const char* token;
};

constexpr AuthorityToken kAuthorityTokens[] = {
    {AuthorityState::Valid, "valid"},
    {AuthorityState::StaleEpoch, "stale_epoch"},
    {AuthorityState::StaleBoot, "stale_boot"},
    {AuthorityState::StalePolicy, "stale_policy"},
    {AuthorityState::StaleTopology, "stale_topology"},
    {AuthorityState::StaleEvidence, "stale_evidence"},
    {AuthorityState::Fenced, "fenced"},
};

}  // namespace

const char* to_string(AuthorityState value) noexcept {
  for (const AuthorityToken& entry : kAuthorityTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised_authority_state";
}

Digest AuthorityVector::digest() const {
  CanonicalWriter writer;
  writer.strong(epoch);
  writer.u64(boot.id.value());
  writer.u32(boot.process_id);
  writer.strong(policy);
  writer.strong(topology);
  writer.strong(evidence_revision);
  writer.digest(evidence_digest);
  writer.strong(issued_sequence);
  return writer.digest();
}

std::string AuthorityVector::render() const {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "epoch=%llu boot=%016llx/%lu policy=%llu topology=%llu evidence_rev=%llu seq=%llu",
                static_cast<unsigned long long>(epoch.value()),
                static_cast<unsigned long long>(boot.id.value()),
                static_cast<unsigned long>(boot.process_id),
                static_cast<unsigned long long>(policy.value()),
                static_cast<unsigned long long>(topology.value()),
                static_cast<unsigned long long>(evidence_revision.value()),
                static_cast<unsigned long long>(issued_sequence.value()));
  return std::string{buffer};
}

std::vector<std::byte> AuthorityVector::encode() const {
  CanonicalWriter writer;
  writer.strong(epoch);
  writer.u64(boot.id.value());
  writer.u32(boot.process_id);
  writer.strong(policy);
  writer.strong(topology);
  writer.strong(evidence_revision);
  writer.digest(evidence_digest);
  writer.strong(issued_sequence);
  return writer.data();
}

Result<AuthorityVector> AuthorityVector::decode(std::span<const std::byte> bytes) {
  CanonicalReader reader(bytes);
  AuthorityVector vector;
  vector.epoch = reader.strong<CoordinatorEpochTag>();
  vector.boot.id = BootId{reader.u64()};
  vector.boot.process_id = reader.u32();
  vector.policy = reader.strong<PolicyGenerationTag>();
  vector.topology = reader.strong<TopologyGenerationTag>();
  vector.evidence_revision = reader.strong<EvidenceRevisionTag>();
  vector.evidence_digest = reader.digest();
  vector.issued_sequence = reader.strong<SequenceTag>();
  reader.require_end();
  if (!reader.ok()) {
    return reader.status();
  }
  return vector;
}

StatusCode AuthorityCheck::status_code() const noexcept {
  switch (state) {
    case AuthorityState::Valid:
      return StatusCode::Ok;
    case AuthorityState::StaleEpoch:
    case AuthorityState::StaleBoot:
    case AuthorityState::StalePolicy:
    case AuthorityState::StaleTopology:
    case AuthorityState::StaleEvidence:
      return StatusCode::StaleGeneration;
    case AuthorityState::Fenced:
      return StatusCode::Fenced;
  }
  return StatusCode::Internal;
}

const char* AuthorityCheck::reason() const noexcept {
  switch (state) {
    case AuthorityState::Valid:
      return "authority_validated";
    case AuthorityState::StaleEpoch:
      return "authority_stale_epoch";
    case AuthorityState::StaleBoot:
      return "authority_stale_boot";
    case AuthorityState::StalePolicy:
      return "authority_stale_policy";
    case AuthorityState::StaleTopology:
      return "authority_stale_topology";
    case AuthorityState::StaleEvidence:
      return "authority_stale_evidence";
    case AuthorityState::Fenced:
      return "authority_fenced";
  }
  return "authority_unknown_state";
}

AuthorityCheck validate_authority(const AuthorityVector& presented, const AuthorityVector& current) noexcept {
  AuthorityCheck check;
  if (presented.boot != current.boot) {
    // A different process incarnation means the presented authority belongs to a
    // previous boot and has been fenced.
    check.state = AuthorityState::Fenced;
    return check;
  }
  if (presented.epoch != current.epoch) {
    check.state = AuthorityState::StaleEpoch;
    return check;
  }
  if (presented.policy != current.policy) {
    check.state = AuthorityState::StalePolicy;
    return check;
  }
  if (presented.topology != current.topology) {
    check.state = AuthorityState::StaleTopology;
    return check;
  }
  if (presented.evidence_revision != current.evidence_revision ||
      presented.evidence_digest != current.evidence_digest) {
    check.state = AuthorityState::StaleEvidence;
    return check;
  }
  return check;
}

}  // namespace fcfn::model
