// FCFN - model-layer identity vocabulary.
//
// The domain model re-exports the core identity types so that model and engine
// code can name them uniformly. They are the same types, not parallel ones:
// nothing here weakens the type distinctions established in fcfn/core/ids.hpp.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_MODEL_IDS_HPP
#define FCFN_MODEL_IDS_HPP

#include <cstdint>

#include "fcfn/core/ids.hpp"

namespace fcfn::model {

using fcfn::ApplierEpoch;
using fcfn::AttemptSequence;
using fcfn::BootId;
using fcfn::BootIdentity;
using fcfn::BoundaryGeneration;
using fcfn::CoordinatorEpoch;
using fcfn::Digest;
using fcfn::EvidenceGeneration;
using fcfn::PlanGeneration;
using fcfn::PolicyGeneration;
using fcfn::ResourceId;
using fcfn::ResourceIdHash;
using fcfn::Sequence;
using fcfn::SessionId;
using fcfn::SnapshotSequence;
using fcfn::StrongId;
using fcfn::TopologyGeneration;
using fcfn::TransitionGeneration;

// Tag aliases: the model layer may name either the identity type or its tag.
using fcfn::ApplierEpochTag;
using fcfn::AttemptSequenceTag;
using fcfn::BootIdTag;
using fcfn::BoundaryGenerationTag;
using fcfn::CoordinatorEpochTag;
using fcfn::EvidenceGenerationTag;
using fcfn::PlanGenerationTag;
using fcfn::PolicyGenerationTag;
using fcfn::ResourceIdTag;
using fcfn::SequenceTag;
using fcfn::SessionIdTag;
using fcfn::SnapshotSequenceTag;
using fcfn::TopologyGenerationTag;
using fcfn::TransitionGenerationTag;

}  // namespace fcfn::model

#endif  // FCFN_MODEL_IDS_HPP
