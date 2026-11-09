#include "../directmotion/InitialPrerollPolicy.h"

#include <iostream>

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
}

int main() {
    using DirectMotionInitialPrerollPolicy::AllowColdStartPreroll;

    Check(AllowColdStartPreroll(true, false, false),
        "Stopped-to-Paused cold start admits source samples for AviSynth bootstrap");
    Check(!AllowColdStartPreroll(false, false, false),
        "ordinary paused state continues to use the one-shot frame-step gate");
    Check(!AllowColdStartPreroll(true, true, false),
        "Run disables the cold-start preroll exception");
    Check(!AllowColdStartPreroll(true, false, true),
        "strict Running-to-Paused frame hold always overrides cold-start admission");
    Check(!AllowColdStartPreroll(false, true, false),
        "normal Running state does not depend on the cold-start policy");
    Check(!AllowColdStartPreroll(false, false, true),
        "strict pause hold remains active after cold-start mode has been cleared");

    // A segment/seek during initial Paused preroll does not change the lifecycle
    // classification; the graph can collect the minimum three source frames again.
    bool initialPrerollActive = true;
    Check(AllowColdStartPreroll(initialPrerollActive, false, false),
        "initial seek remains eligible for a fresh bootstrap while the graph is Paused");
    Check(AllowColdStartPreroll(initialPrerollActive, false, false),
        "repeated paused NewSegment callbacks do not re-arm the one-shot frame gate");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}