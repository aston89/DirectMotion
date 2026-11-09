#pragma once

namespace DirectMotionReceiveCommitPolicy {

// A Receive sample may commit work only while its captured pipeline generation
// is still current and no Stop/Flush/NewSegment transition is being published.
// Callers re-evaluate this gate at each expensive or externally visible boundary.
inline bool CanCommit(bool generationIsCurrent, bool graphTransitioning) noexcept {
    return generationIsCurrent && !graphTransitioning;
}

} // namespace DirectMotionReceiveCommitPolicy