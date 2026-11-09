#pragma once

#include <cstdint>
#include <limits>

namespace DirectMotionSeekHandoffStatus {

enum class State : uint8_t {
    Idle = 0,
    SourceFallback = 1,
    ReservedLegacyWarmup = 2,
    PtsWatermarkDrain = 3,
    VisualBlend = 4
};

constexpr unsigned kStateBits = 3;
constexpr uint64_t kStateMask = (uint64_t{1} << kStateBits) - 1;
constexpr uint64_t kMaximumGeneration = (std::numeric_limits<uint64_t>::max)() >> kStateBits;

constexpr bool IsValidState(State state) noexcept {
    return state == State::Idle || state == State::SourceFallback ||
        state == State::PtsWatermarkDrain || state == State::VisualBlend;
}

constexpr uint64_t GenerationOf(uint64_t packed) noexcept {
    return packed >> kStateBits;
}

constexpr State StateOf(uint64_t packed) noexcept {
    return static_cast<State>(packed & kStateMask);
}

// Generation is part of the published value so a late completion from an old
// DeliveryWorker packet cannot overwrite the state of a newer seek.
constexpr uint64_t Update(uint64_t current, uint64_t generation, State state) noexcept {
    if (!IsValidState(state) || generation > kMaximumGeneration ||
        generation < GenerationOf(current)) return current;
    return (generation << kStateBits) | static_cast<uint64_t>(state);
}

} // namespace DirectMotionSeekHandoffStatus
