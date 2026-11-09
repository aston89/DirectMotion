#include "../directmotion/SeekFallbackGenerationPolicy.h"

#include <cstdint>
#include <iostream>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void Check(bool condition, const char* name) {
    ++checks;
    if (condition) std::cout << "PASS | " << name << '\n';
    else { ++failures; std::cout << "FAIL | " << name << '\n'; }
}
}

int main() {
    using DirectMotionSeekFallback::CanArmFallbackGeneration;
    using DirectMotionSeekFallback::OwnsFallbackGeneration;

    uint64_t owner = 41;
    Check(OwnsFallbackGeneration(owner, 41),
        "the processing pass that armed fallback owns its state");
    Check(!OwnsFallbackGeneration(owner, 40),
        "a retired processing generation cannot mutate fallback");
    Check(!OwnsFallbackGeneration(owner, 42),
        "a future processing generation cannot mutate fallback before it is armed");
    Check(!OwnsFallbackGeneration(0, 0),
        "generation zero never owns fallback state");
    Check(!OwnsFallbackGeneration(0, 42),
        "an unarmed fallback has no generation owner");

    // Reproduce the reported interleaving: generation 42 arms a fresh fallback,
    // then generation 41 returns late from GetFrame and attempts its handoff.
    owner = 42;
    bool fallbackActive = true;
    bool acceptingSource = true;
    const bool oldPassMayClassify = OwnsFallbackGeneration(owner, 41);
    if (oldPassMayClassify) {
        fallbackActive = false;
        acceptingSource = false;
    }
    Check(!oldPassMayClassify && fallbackActive && acceptingSource,
        "late old-generation handoff cannot seal the newly armed fallback");

    // A stale clear must not clear the replacement owner.
    const bool oldPassMayClear = OwnsFallbackGeneration(owner, 41);
    if (oldPassMayClear) owner = 0;
    Check(!oldPassMayClear && owner == 42,
        "late old-generation clear cannot retire the replacement fallback");

    Check(CanArmFallbackGeneration(42, 42),
        "fallback can be armed for the currently published generation");
    Check(!CanArmFallbackGeneration(41, 42),
        "a callback cannot arm fallback for an already retired generation");
    Check(!CanArmFallbackGeneration(0, 0),
        "generation zero cannot be armed even when it matches current state");
    Check(!CanArmFallbackGeneration(42, 0),
        "a callback cannot arm fallback after pipeline shutdown");

    // An inactive state remains owned until reset/clear; it cannot be mistaken for
    // a different generation when an old processing pass wakes up.
    fallbackActive = false;
    Check(OwnsFallbackGeneration(owner, 42) && !OwnsFallbackGeneration(owner, 41),
        "fallback handoff preserves generation ownership until explicit clear");

    // ResetEngineGenerationState must carry the callback's original generation
    // token. Reading CurrentGeneration after a delayed callback resumes could
    // accidentally adopt the replacement seek and reset its watermark.
    uint64_t resetOwner = 52;
    const uint64_t delayedResetToken = 51;
    bool resetProgress = false;
    if (OwnsFallbackGeneration(resetOwner, delayedResetToken)) resetProgress = true;
    Check(!resetProgress && resetOwner == 52,
        "delayed reset carrying an old token cannot reset a replacement fallback");
    const uint64_t currentResetToken = 52;
    if (OwnsFallbackGeneration(resetOwner, currentResetToken)) resetProgress = true;
    Check(resetProgress,
        "reset carrying the owning generation token may reset its fallback progress");

    std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << checks << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
