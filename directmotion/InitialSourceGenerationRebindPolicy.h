#pragma once

#include <cstdint>
#include <limits>

namespace DirectMotionInitialSourceGenerationRebindPolicy {

// Rebinding is legal only for the one pristine graph carried across the first
// NewSegment. The shared source-wait mutex makes the epoch fields one coherent
// snapshot with respect to concurrent lifecycle cancellation.
struct Snapshot {
    bool graphInitialized = false;
    bool inputClipPresent = false;
    bool pipelineGenerationMatches = false;
    // This counts frames published into InputClip, not decoded preroll held in
    // the separate dormant SourceBootstrapQueue; that FIFO may already be nonempty.
    bool noFilterInputPublished = false;
    bool noFilterOutputEmitted = false;
    bool noEngineInputAccepted = false;
    bool noEngineOutputEmitted = false;
    bool cacheEmpty = false;
    bool noActiveGetFrames = false;
    bool noSourceWaiters = false;
    bool notEndOfStream = false;
    bool notLocallyInterrupted = false;
    uint64_t inputClipEpoch = 0;
    uint64_t expectedPreviousEpoch = 0;
    uint64_t newSourceInterruptEpoch = 0;
    uint64_t currentGateEpoch = 0;
    uint64_t currentCacheGeneration = 0;
    uint64_t newCacheGeneration = 0;
};

inline bool CanRebindPristineInitialSourceGeneration(const Snapshot& state) noexcept {
    if (state.expectedPreviousEpoch == (std::numeric_limits<uint64_t>::max)())
        return false;

    return state.graphInitialized && state.inputClipPresent &&
        state.pipelineGenerationMatches && state.noFilterInputPublished &&
        state.noFilterOutputEmitted && state.noEngineInputAccepted &&
        state.noEngineOutputEmitted && state.cacheEmpty &&
        state.noActiveGetFrames && state.noSourceWaiters &&
        state.notEndOfStream && state.notLocallyInterrupted &&
        state.inputClipEpoch == state.expectedPreviousEpoch &&
        state.newSourceInterruptEpoch == state.expectedPreviousEpoch + 1 &&
        state.currentGateEpoch == state.newSourceInterruptEpoch &&
        state.newCacheGeneration != 0 &&
        state.newCacheGeneration != state.currentCacheGeneration;
}

} // namespace DirectMotionInitialSourceGenerationRebindPolicy