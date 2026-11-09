#pragma once

namespace DirectMotionInitialSegmentGraphPolicy {

// The graph prepared by StartStreaming may be adopted by the first NewSegment
// only when it is still the pristine graph for the current output generation.
// Every condition is explicit so future lifecycle changes cannot accidentally
// preserve an engine that has already consumed samples or belongs to an older
// stream/seek generation.
inline bool CanAdoptPreparedInitialGraph(
    bool awaitingInitialSegment,
    bool graphInitialized,
    bool graphGenerationMatches,
    bool noSourceInputAccepted,
    bool noOutputEmitted,
    bool seekResetIdle,
    bool bootstrapQueueInactive,
    bool noFlushTransition,
    bool notStopping) noexcept {
    return awaitingInitialSegment && graphInitialized && graphGenerationMatches &&
        noSourceInputAccepted && noOutputEmitted && seekResetIdle &&
        bootstrapQueueInactive && noFlushTransition && notStopping;
}

} // namespace DirectMotionInitialSegmentGraphPolicy
