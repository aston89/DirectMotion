#include "../directmotion/InitialSegmentGraphPolicy.h"

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

bool Eligible(bool awaitingInitialSegment = true,
              bool graphInitialized = true,
              bool graphGenerationMatches = true,
              bool noSourceInputAccepted = true,
              bool noOutputEmitted = true,
              bool seekResetIdle = true,
              bool bootstrapQueueInactive = true,
              bool noFlushTransition = true,
              bool notStopping = true) {
    return DirectMotionInitialSegmentGraphPolicy::CanAdoptPreparedInitialGraph(
        awaitingInitialSegment, graphInitialized, graphGenerationMatches,
        noSourceInputAccepted, noOutputEmitted, seekResetIdle,
        bootstrapQueueInactive, noFlushTransition, notStopping);
}
}

int main() {
    Check(Eligible(),
        "fresh StartStreaming graph is adopted by the first NewSegment");
    Check(!Eligible(false),
        "a later NewSegment cannot adopt a graph after the initial segment window");
    Check(!Eligible(true, false),
        "an absent or failed graph is never adopted");
    Check(!Eligible(true, true, false),
        "a graph from an older delivery generation is never adopted");
    Check(!Eligible(true, true, true, false),
        "a graph that already accepted source input is rebuilt");
    Check(!Eligible(true, true, true, true, false),
        "a graph that already emitted output is rebuilt");
    Check(!Eligible(true, true, true, true, true, false),
        "an active or pending seek reset takes precedence over initial-graph adoption");
    Check(!Eligible(true, true, true, true, true, true, false),
        "an active source-bootstrap queue prevents initial-graph adoption");
    Check(!Eligible(true, true, true, true, true, true, true, false),
        "an overlapping flush prevents initial-graph adoption");
    Check(!Eligible(true, true, true, true, true, true, true, true, false),
        "a stopping filter never adopts an initial graph");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
