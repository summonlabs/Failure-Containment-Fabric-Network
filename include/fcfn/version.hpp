// Failure Containment Fabric Network - version identity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_VERSION_HPP
#define FCFN_VERSION_HPP

#include <cstdint>

namespace fcfn {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Numeric version identity used by durable formats and wire frames.
inline constexpr std::uint32_t kAbiVersion = (kVersionMajor * 10000u) + (kVersionMinor * 100u) + kVersionPatch;

inline constexpr const char* kVersionString = "1.0.0";

/// Identity of the semantic contract implemented by this build. Durable stores and
/// wire peers must agree on both kAbiVersion and kSemanticVersion or refuse to proceed.
inline constexpr std::uint32_t kSemanticVersion = 1;

}  // namespace fcfn

#endif  // FCFN_VERSION_HPP
