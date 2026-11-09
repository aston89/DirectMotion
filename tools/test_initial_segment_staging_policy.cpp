#include "../directmotion/InitialSegmentStagingPolicy.h"

#include <iostream>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) std::cout << "PASS | " << description << '\n';
    else { ++failures; std::cout << "FAIL | " << description << '\n'; }
}

bool ShouldStage(bool awaiting = true, bool queueActive = true,
                 bool workerGated = true, bool generationMatches = true) {
    return DirectMotionInitialSegmentStagingPolicy::ShouldStageBeforeFirstSegment(
        awaiting, queueActive, workerGated, generationMatches);
}


bool CanFinishInFlightStage(bool initialStage = true,
                            bool segmentTransition = true,
                            bool flushTransition = false,
                            bool stopping = false,
                            bool runtimeFailed = false,
                            bool markerArmed = true,
                            bool queueActive = true,
                            bool workerGated = true,
                            bool graphGenerationMatches = true) {
    return DirectMotionInitialSegmentStagingPolicy::CanCommitInFlightInitialStage(
        initialStage, segmentTransition, flushTransition, stopping,
        runtimeFailed, markerArmed, queueActive, workerGated,
        graphGenerationMatches);
}

bool CanAdopt(bool awaiting = true, bool graphInitialized = true,
              bool graphGenerationMatches = true, bool queueActive = true,
              bool workerGated = true, bool noPublished = true, bool noOutput = true,
              bool seekIdle = true, bool noFlush = true,
              bool notStopping = true, bool noPreviousSegment = true) {
    return DirectMotionInitialSegmentStagingPolicy::CanAdoptStagedInitialGraph(
        awaiting, graphInitialized, graphGenerationMatches, queueActive,
        workerGated, noPublished, noOutput, seekIdle,
        noFlush, notStopping, noPreviousSegment);
}
}

int main() {
    Check(ShouldStage(), "Receive-before-NewSegment samples enter dormant initial staging");
    Check(!ShouldStage(false), "staging is disabled after initial segment ownership ends");
    Check(!ShouldStage(true, false), "inactive staging generation cannot accept preroll");
    Check(!ShouldStage(true, true, false), "a ready worker is not an initial dormant stage");
    Check(!ShouldStage(true, true, true, false), "stale graph generation cannot stage initial preroll");

    Check(CanFinishInFlightStage(),
        "Receive already inside callback may finish into dormant queue after NewSegment retires generation");
    Check(!CanFinishInFlightStage(false),
        "ordinary Receive cannot use the in-flight initial-stage exception");
    Check(!CanFinishInFlightStage(true, false),
        "in-flight initial-stage exception requires an active segment transition");
    Check(!CanFinishInFlightStage(true, true, true),
        "flush transition always rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, true),
        "Stop always rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, false, true),
        "runtime failure rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, false, false, false),
        "cleared initial marker rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, false, false, true, false),
        "cancelled queue rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, false, false, true, true, false),
        "ready worker rejects in-flight initial-stage commit");
    Check(!CanFinishInFlightStage(true, true, false, false, false, true, true, true, false),
        "stale graph generation rejects in-flight initial-stage commit");

    Check(CanAdopt(), "queued initial preroll can be rebound to first NewSegment");
    Check(!CanAdopt(false), "expired initial-segment marker rejects adoption");
    Check(!CanAdopt(true, false), "missing graph rejects staged adoption");
    Check(!CanAdopt(true, true, false), "stale graph generation rejects staged adoption");
    Check(!CanAdopt(true, true, true, false), "inactive staging queue rejects adoption");
    Check(!CanAdopt(true, true, true, true, false), "ready worker rejects adoption of a dormant stage");
    Check(CanAdopt(true, true, true, true, true, true, true, true, true, true),
        "empty dormant queue can still rebind so first post-segment Receive is staged");
    Check(!CanAdopt(true, true, true, true, false), "published source frames reject staged adoption");
    Check(!CanAdopt(true, true, true, true, true, false), "emitted output rejects staged adoption");
    Check(!CanAdopt(true, true, true, true, true, true, false), "active seek reset rejects staged adoption");
    Check(!CanAdopt(true, true, true, true, true, true, true, false), "flush transition rejects staged adoption");
    Check(!CanAdopt(true, true, true, true, true, true, true, true, false), "stopping filter rejects staged adoption");
    Check(!CanAdopt(true, true, true, true, true, true, true, true, true, false), "later segment cannot adopt initial staged graph");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
