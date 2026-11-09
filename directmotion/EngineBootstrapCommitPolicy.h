#pragma once

namespace DirectMotionEngineBootstrapCommit {

// A replacement AviSynth graph is prepared on a worker without holding the
// DirectShow Receive critical section. It may be published only if every
// lifecycle identity still matches and the engine slot is still empty.
inline bool CanCommitCandidate(
    bool pipelineGenerationCurrent,
    bool sourceBootstrapGenerationCurrent,
    bool receiveCommitCurrent,
    bool deferredResetCompleted,
    bool engineSlotEmpty,
    bool candidateInitialized) noexcept {
    return pipelineGenerationCurrent && sourceBootstrapGenerationCurrent &&
        receiveCommitCurrent && deferredResetCompleted && engineSlotEmpty &&
        candidateInitialized;
}

// After the retired engine is detached and destroyed outside m_csReceive, the
// worker may finish the deferred reset only if the same seek still owns the
// staging generation and no lifecycle transition superseded it in the meantime.
inline bool CanFinalizeRetiredEngine(
    bool pipelineGenerationCurrent,
    bool sourceBootstrapGenerationCurrent,
    bool receiveCommitCurrent,
    bool deferredResetStillPending,
    bool bootstrapWorkerStillRunning,
    bool engineSlotEmpty) noexcept {
    return pipelineGenerationCurrent && sourceBootstrapGenerationCurrent &&
        receiveCommitCurrent && deferredResetStillPending &&
        bootstrapWorkerStillRunning && engineSlotEmpty;
}

} // namespace DirectMotionEngineBootstrapCommit
