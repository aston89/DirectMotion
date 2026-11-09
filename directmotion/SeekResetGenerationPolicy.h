#pragma once

#include <cstdint>

namespace DirectMotionSeekReset {

enum class Phase : uint8_t {
    Idle = 0,
    AwaitingSegment,
    AwaitingBootstrap
};

struct State final {
    uint64_t generation = 0;
    Phase phase = Phase::Idle;
    bool deferredResetPending = false;
};

inline void Reset(State& state) noexcept {
    state.generation = 0;
    state.phase = Phase::Idle;
    state.deferredResetPending = false;
}

inline bool BeginDeferredReset(State& state, uint64_t generation,
                               bool awaitNewSegment) noexcept {
    if (generation == 0) return false;
    state.generation = generation;
    state.phase = awaitNewSegment ? Phase::AwaitingSegment : Phase::AwaitingBootstrap;
    state.deferredResetPending = true;
    return true;
}

inline bool BeginSynchronousResetAwaitingSegment(State& state,
                                                  uint64_t generation) noexcept {
    if (generation == 0) return false;
    state.generation = generation;
    state.phase = Phase::AwaitingSegment;
    state.deferredResetPending = false;
    return true;
}

// A NewSegment supersedes any sample generation admitted after EndFlush. If a
// deferred reset is pending, transfer its ownership to the final segment generation.
// If the flush already completed synchronously, consume the segment marker without
// manufacturing a deferred bootstrap.
inline bool RebindForNewSegment(State& state, uint64_t newGeneration) noexcept {
    if (newGeneration == 0 || state.phase == Phase::Idle) return false;
    state.generation = newGeneration;
    if (state.deferredResetPending) {
        state.phase = Phase::AwaitingBootstrap;
    } else {
        Reset(state);
    }
    return true;
}

inline bool IsAwaitingSegment(const State& state) noexcept {
    return state.phase == Phase::AwaitingSegment;
}

inline bool OwnsBootstrap(const State& state, uint64_t generation) noexcept {
    return generation != 0 && generation == state.generation &&
        state.phase == Phase::AwaitingBootstrap && state.deferredResetPending;
}

// A decoded one-shot frame may arrive while the graph is paused immediately
// after EndFlush, before NewSegment, or after NewSegment has rebound the reset.
// In either window, the paused-frame presentation must not run the generic
// synchronous ResetEngine path: that path cancels SourceBootstrapQueue and can
// invalidate the same generation's shared-NV12 admission while fallback still
// owns it. Preserve the deferred reset only when both state and queue still
// identify this exact generation.
inline bool ShouldPreserveDeferredBootstrapForPausedFrame(
    const State& state, uint64_t generation, bool sourceBootstrapQueueActive) noexcept {
    return sourceBootstrapQueueActive && generation != 0 &&
        generation == state.generation && state.deferredResetPending &&
        (state.phase == Phase::AwaitingSegment ||
         state.phase == Phase::AwaitingBootstrap);
}

inline bool CanComplete(const State& state, uint64_t generation) noexcept {
    return OwnsBootstrap(state, generation);
}

inline bool Complete(State& state, uint64_t generation) noexcept {
    if (!CanComplete(state, generation)) return false;
    Reset(state);
    return true;
}

} // namespace DirectMotionSeekReset
