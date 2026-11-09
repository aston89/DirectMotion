#pragma once

#include <cstddef>
#include <cstdint>

namespace DirectMotionSeekTransition {

// A short, bounded handoff smooths the visual change from duplicated decoded
// source frames to MVTools output. At 59.86 Hz, six output slots are about 100 ms.
inline constexpr uint32_t kBlendFrameCount = 6;

struct State {
    uint64_t generation = 0;
    uint32_t nextFrameIndex = 0;
    uint32_t frameCount = 0;
};

inline void Reset(State& state) noexcept {
    state = {};
}

inline bool Begin(State& state, uint64_t generation,
                  uint32_t frameCount = kBlendFrameCount) noexcept {
    Reset(state);
    if (generation == 0 || frameCount == 0) return false;
    state.generation = generation;
    state.frameCount = frameCount;
    return true;
}

inline bool TryAcquire(const State& state, uint64_t generation,
                       uint32_t& frameIndex) noexcept {
    frameIndex = 0;
    if (generation == 0 || state.generation != generation ||
        state.frameCount == 0 || state.nextFrameIndex >= state.frameCount)
        return false;
    frameIndex = state.nextFrameIndex;
    return true;
}

// Commit only after the blended output packet has entered the bounded FIFO.
// A cancelled/stale enqueue therefore cannot consume a transition step.
inline bool Commit(State& state, uint64_t generation,
                   uint32_t frameIndex) noexcept {
    if (generation == 0 || state.generation != generation ||
        state.frameCount == 0 || frameIndex != state.nextFrameIndex ||
        frameIndex >= state.frameCount)
        return false;
    ++state.nextFrameIndex;
    if (state.nextFrameIndex == state.frameCount) Reset(state);
    return true;
}

// Blend source NV12 into an interpolated NV12 destination in place. The byte
// layout is identical for both normalized, tightly packed visible NV12 images:
// Y followed by interleaved UV. Only the first `frameCount` packets are marked
// for blending; the final step has source weight zero and needs no pixel loop.
inline bool BlendNv12InPlace(const uint8_t* source, uint8_t* interpolated,
                             size_t bytes, uint32_t frameIndex,
                             uint32_t frameCount) noexcept {
    if (!source || !interpolated || bytes == 0 ||
        frameCount != kBlendFrameCount || frameIndex >= kBlendFrameCount)
        return false;

    const uint32_t interpolatedWeight = frameIndex + 1;
    const uint32_t sourceWeight = kBlendFrameCount - interpolatedWeight;
    if (sourceWeight == 0) return true;

    // Keep the divisor compile-time constant so MSVC can lower division by six
    // to reciprocal multiply/shift instead of a per-byte integer divide.
    constexpr uint32_t rounding = kBlendFrameCount / 2;
    for (size_t i = 0; i < bytes; ++i) {
        const uint32_t mixed =
            static_cast<uint32_t>(source[i]) * sourceWeight +
            static_cast<uint32_t>(interpolated[i]) * interpolatedWeight +
            rounding;
        interpolated[i] = static_cast<uint8_t>(mixed / kBlendFrameCount);
    }
    return true;
}

} // namespace DirectMotionSeekTransition
