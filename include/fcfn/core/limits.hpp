// FCFN - bounded resource envelope.
//
// Every table, history, queue, document, explanation, and retained attempt in
// FCFN is bounded by a constant from this header or by a tighter per-runtime
// configured limit. Exceeding a bound is a deterministic refusal, never an
// unbounded allocation and never process instability.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_CORE_LIMITS_HPP
#define FCFN_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace fcfn {

/// Topology materialisation bounds.
inline constexpr std::size_t kMaxTopologyNodes = 65536;
inline constexpr std::size_t kMaxTopologyEdges = 262144;
inline constexpr std::size_t kMaxProtectedObligations = 4096;
inline constexpr std::size_t kMaxFailureSources = 4096;

/// Solver bounds. Reaching any of these yields an explicit bounded outcome.
inline constexpr std::uint64_t kMaxExactSearchNodesDefault = 200000;
inline constexpr std::uint64_t kMaxHeuristicIterationsDefault = 200000;
inline constexpr std::size_t kMaxBoundaryMembers = 8192;

/// Eligibility / weight bounds.
inline constexpr std::uint64_t kMaxNodeWeight = 1000000;
inline constexpr std::uint64_t kMaxPlanWeightSum = 1000000000000ULL;

/// Explanation bounds.
inline constexpr std::size_t kMaxExplanationItems = 64;
inline constexpr std::size_t kMaxExplanationTextLength = 256;
inline constexpr std::size_t kMaxCutCertificatePaths = 256;
inline constexpr std::size_t kMaxCutCertificatePathNodes = 4096;

/// Durable format bounds.
inline constexpr std::uint32_t kMaxRecordPayloadBytes = 16u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxSnapshotPayloadBytes = 256u * 1024u * 1024u;
inline constexpr std::size_t kMaxWalRecordsPerSegment = 1000000;
inline constexpr std::size_t kMaxRetainedSnapshots = 4;
inline constexpr std::size_t kMaxHistoryEntries = 4096;

/// Wire protocol bounds.
inline constexpr std::uint32_t kMaxFramePayloadBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxFramesPerSession = 1000000;
inline constexpr std::size_t kMaxSessions = 256;
inline constexpr std::size_t kMaxQueuedRequestsPerSession = 1024;
inline constexpr std::size_t kMaxSessionTokenLength = 64;
inline constexpr std::size_t kMaxSessionLabelLength = 64;

/// Runtime bounds.
inline constexpr std::size_t kMaxRetainedPlans = 1024;
inline constexpr std::size_t kMaxRetainedAttempts = 4096;
inline constexpr std::size_t kMaxRetainedTransitions = 4096;
inline constexpr std::size_t kMaxRetainedDetections = 4096;
inline constexpr std::size_t kMaxFencedBoots = 1024;

/// Textual bounds for canonical documents.
inline constexpr std::size_t kMaxCanonicalTextLength = 4096;
inline constexpr std::size_t kMaxDiagnosticTextLength = 512;

}  // namespace fcfn

#endif  // FCFN_CORE_LIMITS_HPP
