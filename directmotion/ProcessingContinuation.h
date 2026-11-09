#pragma once

namespace DirectMotionProcessingContinuation {

// A capped pass is the only non-terminal path that requires an internal
// continuation. All other outcomes either wait for new source publication,
// have completed, or have been interrupted by lifecycle state.
inline bool ShouldRearmAfterPass(
    bool passLimitReached,
    bool terminalDrain,
    bool workerStop,
    bool workerSuspend,
    bool generationCurrent,
    bool graphTransitioning,
    bool pauseTransitionPending,
    bool runtimeFailed,
    bool processingSucceeded) noexcept {
    return passLimitReached && !terminalDrain && !workerStop && !workerSuspend &&
        generationCurrent && !graphTransitioning && !pauseTransitionPending &&
        !runtimeFailed && processingSucceeded;
}

} // namespace DirectMotionProcessingContinuation
