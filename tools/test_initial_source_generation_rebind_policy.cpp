#include "../directmotion/InitialSourceGenerationRebindPolicy.h"

#include <iostream>
#include <limits>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS | " << description << '\n';
    } else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}

DirectMotionInitialSourceGenerationRebindPolicy::Snapshot Eligible() {
    DirectMotionInitialSourceGenerationRebindPolicy::Snapshot state;
    state.graphInitialized = true;
    state.inputClipPresent = true;
    state.pipelineGenerationMatches = true;
    state.noFilterInputPublished = true;
    state.noFilterOutputEmitted = true;
    state.noEngineInputAccepted = true;
    state.noEngineOutputEmitted = true;
    state.cacheEmpty = true;
    state.noActiveGetFrames = true;
    state.noSourceWaiters = true;
    state.notEndOfStream = true;
    state.notLocallyInterrupted = true;
    state.inputClipEpoch = 4;
    state.expectedPreviousEpoch = 4;
    state.newSourceInterruptEpoch = 5;
    state.currentGateEpoch = 5;
    state.currentCacheGeneration = 27;
    state.newCacheGeneration = 28;
    return state;
}
}

int main() {
    using DirectMotionInitialSourceGenerationRebindPolicy::CanRebindPristineInitialSourceGeneration;

    Check(CanRebindPristineInitialSourceGeneration(Eligible()),
        "reported cold-start sequence rebinds InputClip epoch 4 to the exact NewSegment epoch 5");
    Check(CanRebindPristineInitialSourceGeneration(Eligible()),
        "decoder preroll staged outside InputClip does not count as a published source-cache frame");

    auto state = Eligible(); state.graphInitialized = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "uninitialized graph cannot be rebound");
    state = Eligible(); state.inputClipPresent = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "missing InputClip cannot be rebound");
    state = Eligible(); state.pipelineGenerationMatches = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "stale pipeline generation cannot be rebound");
    state = Eligible(); state.noFilterInputPublished = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "source publication into InputClip rejects rebinding");
    state = Eligible(); state.noFilterOutputEmitted = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "filter-level output rejects rebinding");
    state = Eligible(); state.noEngineInputAccepted = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "engine source input rejects rebinding");
    state = Eligible(); state.noEngineOutputEmitted = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "engine output state rejects rebinding");
    state = Eligible(); state.cacheEmpty = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "nonempty source cache rejects rebinding");
    state = Eligible(); state.noActiveGetFrames = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "active GetFrame calls reject rebinding");
    state = Eligible(); state.noSourceWaiters = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "registered source waiters reject rebinding");
    state = Eligible(); state.notEndOfStream = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "finalized source graph rejects rebinding");
    state = Eligible(); state.notLocallyInterrupted = false;
    Check(!CanRebindPristineInitialSourceGeneration(state), "locally interrupted InputClip rejects rebinding");
    state = Eligible(); state.inputClipEpoch = 3;
    Check(!CanRebindPristineInitialSourceGeneration(state), "InputClip must still own the exact pre-NewSegment epoch");
    state = Eligible(); state.newSourceInterruptEpoch = 6; state.currentGateEpoch = 6;
    Check(!CanRebindPristineInitialSourceGeneration(state), "an intervening lifecycle cancellation rejects rebinding");
    state = Eligible(); state.currentGateEpoch = 6;
    Check(!CanRebindPristineInitialSourceGeneration(state), "epoch change after the intended NewSegment increment rejects rebinding");
    state = Eligible(); state.newSourceInterruptEpoch = 4; state.currentGateEpoch = 4;
    Check(!CanRebindPristineInitialSourceGeneration(state), "rebinding requires exactly one forward epoch transition");
    state = Eligible(); state.newCacheGeneration = 0;
    Check(!CanRebindPristineInitialSourceGeneration(state), "zero is never a valid new cache generation");
    state = Eligible(); state.newCacheGeneration = state.currentCacheGeneration;
    Check(!CanRebindPristineInitialSourceGeneration(state), "cache generation must be refreshed, not reused");
    state = Eligible(); state.expectedPreviousEpoch = (std::numeric_limits<uint64_t>::max)();
    state.inputClipEpoch = state.expectedPreviousEpoch;
    state.newSourceInterruptEpoch = 0;
    state.currentGateEpoch = 0;
    Check(!CanRebindPristineInitialSourceGeneration(state), "epoch wraparound is rejected");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}