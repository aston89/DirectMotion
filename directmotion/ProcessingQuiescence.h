#pragma once

namespace DirectMotionProcessingQuiescence {

// A stop request is not proof that the ProcessingWorker has left AviSynth/MVTools.
// Engine memory may be reclaimed only after the active pass is gone and either the
// worker is no longer started or its wait predicate is held closed by Stop/Suspend.
struct Snapshot {
    bool workerStarted = false;
    bool stopRequested = false;
    bool suspendRequested = false;
    bool activePass = false;
};

inline bool IsQuiescentForEngineReset(const Snapshot& state) noexcept {
    if (state.activePass) return false;
    return !state.workerStarted || state.stopRequested || state.suspendRequested;
}

} // namespace DirectMotionProcessingQuiescence
