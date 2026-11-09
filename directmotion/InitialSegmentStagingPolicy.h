#pragma once

namespace DirectMotionInitialSegmentStagingPolicy {

// Before the first NewSegment, decoder preroll belongs to a dormant initial
// generation. It may be accepted only while the graph marker, queue gate and
// engine generation all agree; after NewSegment the worker gate is opened.
inline bool ShouldStageBeforeFirstSegment(
    bool awaitingInitialSegment,
    bool queueActiveForGeneration,
    bool workerStillGated,
    bool graphGenerationMatches) noexcept {
    return awaitingInitialSegment && queueActiveForGeneration &&
        workerStillGated && graphGenerationMatches;
}

// A Receive that already passed its initial graph-transition admission check may
// finish committing its frame into the dormant queue after NewSegment has begun
// retiring the old pipeline generation. NewSegment cannot rebind the queue until
// it acquires the same receive lock, so the old queue remains the exclusive owner
// until this callback returns. This exception never applies during flush/Stop/error.
inline bool CanCommitInFlightInitialStage(
    bool initialSegmentStaging,
    bool segmentTransitionActive,
    bool flushTransitionActive,
    bool stopRequested,
    bool runtimeFailed,
    bool initialMarkerStillArmed,
    bool queueActiveForGeneration,
    bool workerStillGated,
    bool graphGenerationMatches) noexcept {
    return initialSegmentStaging && segmentTransitionActive &&
        !flushTransitionActive && !stopRequested && !runtimeFailed &&
        initialMarkerStillArmed && queueActiveForGeneration &&
        workerStillGated && graphGenerationMatches;
}

// A pre-segment staged queue can be rebound to the first announced segment only
// when no consumer has taken a frame and the graph is still pristine. Queue-level
// RebindDormantGeneration performs the final in-flight/epoch check atomically.
inline bool CanAdoptStagedInitialGraph(
    bool awaitingInitialSegment,
    bool graphInitialized,
    bool graphGenerationMatches,
    bool stagingQueueActive,
    bool stagingWorkerGated,
    bool noSourceFramesPublished,
    bool noOutputEmitted,
    bool seekResetIdle,
    bool noFlushTransition,
    bool notStopping,
    bool noPreviousSegment) noexcept {
    return awaitingInitialSegment && graphInitialized && graphGenerationMatches &&
        stagingQueueActive && stagingWorkerGated &&
        noSourceFramesPublished && noOutputEmitted && seekResetIdle &&
        noFlushTransition && notStopping && noPreviousSegment;
}

} // namespace DirectMotionInitialSegmentStagingPolicy
