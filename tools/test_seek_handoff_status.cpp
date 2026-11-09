#include "../directmotion/SeekHandoffStatusPolicy.h"

#include <cstdint>
#include <iostream>
#include <limits>

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
    using namespace DirectMotionSeekHandoffStatus;

    const uint64_t initial = Update(0, 7, State::SourceFallback);
    Check(GenerationOf(initial) == 7 && StateOf(initial) == State::SourceFallback,
          "source fallback state is published with its owning generation");

    const uint64_t draining = Update(initial, 7, State::PtsWatermarkDrain);
    Check(StateOf(draining) == State::PtsWatermarkDrain,
          "first visible MVTools output advances diagnostics to PTS-watermark drain");

    const uint64_t blending = Update(draining, 7, State::VisualBlend);
    Check(StateOf(blending) == State::VisualBlend,
          "the PTS handoff exposes the visual-blend phase");

    const uint64_t completed = Update(blending, 7, State::Idle);
    Check(StateOf(completed) == State::Idle && GenerationOf(completed) == 7,
          "successful final blend delivery returns the same generation to idle");

    const uint64_t newer = Update(completed, 8, State::SourceFallback);
    Check(GenerationOf(newer) == 8 && StateOf(newer) == State::SourceFallback,
          "a newer seek takes ownership of the published phase");

    Check(Update(newer, 7, State::Idle) == newer,
          "late delivery completion from an older seek cannot clear a newer seek");
    Check(Update(newer, 7, State::PtsWatermarkDrain) == newer,
          "late PTS-drain completion from an older seek cannot advance a newer seek");
    Check(Update(newer, 8, State::PtsWatermarkDrain) != newer,
          "the current seek can advance its own phase");

    Check(Update(newer, kMaximumGeneration + 1, State::Idle) == newer,
          "generation packing rejects values that would overflow the state word");
    Check(Update(newer, 8, State::ReservedLegacyWarmup) == newer,
          "the retired timer phase remains reserved and cannot be published");
    Check(Update(newer, 8, static_cast<State>(7)) == newer,
          "invalid phase values are rejected without corrupting the published state");
    Check(GenerationOf(0) == 0 && StateOf(0) == State::Idle,
          "zero-initialized diagnostics represent idle generation zero");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
