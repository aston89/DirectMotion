#pragma once

namespace DirectMotionPauseRetention {

// Once strict Running -> Paused hold is committed, stop producing additional
// interpolated packets from the already-populated AviSynth cache. A terminal
// EOS drain keeps its existing ordered completion policy.
inline bool ShouldSuspendProcessing(bool strictPauseHold,
                                    bool terminalDrain) noexcept {
    return strictPauseHold && !terminalDrain;
}

// Pause publishes the strict hold bit just before advancing the monotonic epoch.
// Also recognize a still-active hold so a GetFrame completing in that narrow
// publication window is not lost; the epoch covers a Run that already cleared it.
inline bool WasGetFrameInterruptedByStrictPause(bool pauseEpochChanged,
                                                bool strictPauseHoldActive) noexcept {
    return pauseEpochChanged || strictPauseHoldActive;
}

// A strict Running -> Paused hold is not a graph discontinuity. Keep the already
// generated output and the AviSynth source history until Run can resume delivery.
inline bool ShouldHoldDelivery(bool strictPauseHold, bool terminalDrain,
                               bool lifecycleInvalidated) noexcept {
    return strictPauseHold && !terminalDrain && !lifecycleInvalidated;
}

// A packet removed from the FIFO but interrupted by the local Pause flush is still
// the next ordered frame. A monotonic pause epoch identifies this even if the worker is
// rescheduled after Run has already cleared the transient hold flags.
inline bool ShouldRetryInterruptedDelivery(bool pauseInterruptedPacket,
                                            bool terminalDrain,
                                            bool generationCurrent,
                                            bool lifecycleInvalidated,
                                            bool stopping,
                                            bool expectedPauseCancellation) noexcept {
    return pauseInterruptedPacket && !terminalDrain && generationCurrent &&
        !lifecycleInvalidated && !stopping && expectedPauseCancellation;
}

// If an AviSynth GetFrame completed across a strict Pause, its output index has already
// advanced. Preserve the frame in the same FIFO generation rather than skipping it; a
// matching pause epoch also covers a Run that raced with the GetFrame return.
inline bool ShouldQueueCompletedFrameAfterPause(bool pauseInterruptedGetFrame,
                                                 bool generationCurrent,
                                                 bool lifecycleInvalidated,
                                                 bool runtimeFailed,
                                                 bool frameAvailable,
                                                 bool finished) noexcept {
    return pauseInterruptedGetFrame && generationCurrent && !lifecycleInvalidated &&
        !runtimeFailed && frameAvailable && !finished;
}

} // namespace DirectMotionPauseRetention
