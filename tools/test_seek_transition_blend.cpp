#include "../directmotion/SeekTransitionBlend.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void Check(bool condition, const char* description) {
    ++checks;
    if (condition) std::cout << "PASS | " << description << '\n';
    else { ++failures; std::cout << "FAIL | " << description << '\n'; }
}
}

int main() {
    using namespace DirectMotionSeekTransition;

    State state{};
    Check(Begin(state, 41), "handoff begins only for a nonzero generation");
    uint32_t index = 99;
    Check(TryAcquire(state, 41, index) && index == 0,
        "first interpolated packet owns blend step zero");
    Check(!TryAcquire(state, 40, index),
        "a stale generation cannot consume the active blend");
    Check(!Commit(state, 41, 1) && state.nextFrameIndex == 0,
        "out-of-order blend completion cannot advance the transition");
    Check(Commit(state, 41, 0) && TryAcquire(state, 41, index) && index == 1,
        "transition advances only after the matching output packet is queued");

    Check(Begin(state, 42), "a newer seek replaces the prior blend owner");
    Check(!Commit(state, 41, 1) && state.generation == 42 && state.nextFrameIndex == 0,
        "late completion from an old seek cannot mutate the new transition");
    Check(TryAcquire(state, 42, index) && index == 0,
        "new seek restarts the fade from its first frame");
    for (uint32_t i = 0; i < kBlendFrameCount; ++i) {
        Check(TryAcquire(state, 42, index) && index == i,
            "blend exposes exactly the next ordered output slot");
        Check(Commit(state, 42, i),
            "matching queued output commits exactly one blend slot");
    }
    Check(state.generation == 0 && !TryAcquire(state, 42, index),
        "completed transition releases its generation ownership");
    Check(!Begin(state, 0), "generation zero cannot start a visual transition");
    Check(!Begin(state, 43, 0), "zero-length transitions are rejected");

    const std::array<uint8_t, 6> source{{0, 60, 120, 180, 240, 255}};
    const std::array<uint8_t, 6> target{{60, 120, 180, 240, 0, 15}};
    auto blended = target;
    Check(BlendNv12InPlace(source.data(), blended.data(), blended.size(), 0, 6),
        "first NV12 blend step is accepted");
    Check(blended == std::array<uint8_t, 6>{{10, 70, 130, 190, 200, 215}},
        "first step keeps five-sixths original and adds one-sixth interpolation");

    blended = target;
    Check(BlendNv12InPlace(source.data(), blended.data(), blended.size(), 2, 6),
        "middle NV12 blend step is accepted");
    Check(blended == std::array<uint8_t, 6>{{30, 90, 150, 210, 120, 135}},
        "middle step advances the linear blend to one-half interpolation");

    blended = target;
    Check(BlendNv12InPlace(source.data(), blended.data(), blended.size(), 5, 6),
        "last NV12 blend step is accepted");
    Check(blended == target,
        "last blend step is exactly the unmodified interpolated frame");

    blended = target;
    Check(!BlendNv12InPlace(nullptr, blended.data(), blended.size(), 0, 6) && blended == target,
        "null source is rejected without mutating the destination");
    Check(!BlendNv12InPlace(source.data(), blended.data(), 0, 0, 6) && blended == target,
        "empty payload is rejected without mutating the destination");
    Check(!BlendNv12InPlace(source.data(), blended.data(), blended.size(), 6, 6) && blended == target,
        "out-of-range blend index is rejected without mutating the destination");
    Check(!BlendNv12InPlace(source.data(), blended.data(), blended.size(), 0, 0) && blended == target,
        "zero frame count is rejected without mutating the destination");
    Check(!BlendNv12InPlace(source.data(), blended.data(), blended.size(), 0, 5) && blended == target,
        "non-six transition length is rejected so production blending stays bounded and constant-divisor");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
