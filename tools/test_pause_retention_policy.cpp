#include "../directmotion/PauseRetentionPolicy.h"

#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using namespace DirectMotionPauseRetention;

    Check(ShouldSuspendProcessing(true, false),
          "strict Pause suspends further interpolated frame production");
    Check(!ShouldSuspendProcessing(false, false),
          "normal Running continues interpolated frame production");
    Check(!ShouldSuspendProcessing(true, true),
          "terminal EOS drain retains its existing processing policy");
    Check(WasGetFrameInterruptedByStrictPause(true, false),
          "pause epoch detects a GetFrame that completed after Run already released hold");
    Check(WasGetFrameInterruptedByStrictPause(false, true),
          "active hold detects GetFrame completion before strict pause epoch publication");
    Check(!WasGetFrameInterruptedByStrictPause(false, false),
          "ordinary GetFrame completion is not classified as a pause interruption");

    Check(ShouldHoldDelivery(true, false, false), "strict pause retains delivery packet");
    Check(!ShouldHoldDelivery(false, false, false), "normal running does not hold delivery");
    Check(!ShouldHoldDelivery(true, true, false), "terminal drain keeps existing preroll policy");
    Check(!ShouldHoldDelivery(true, false, true), "seek/flush does not retain stale delivery");

    Check(ShouldRetryInterruptedDelivery(true, false, true, false, false, true),
          "pause-cancelled active packet retries after Run");
    Check(!ShouldRetryInterruptedDelivery(false, false, true, false, false, true),
          "non-pause abort is not retried");
    Check(!ShouldRetryInterruptedDelivery(true, true, true, false, false, true),
          "terminal-drain packet keeps its existing paused-preroll completion policy");
    Check(!ShouldRetryInterruptedDelivery(true, false, false, false, false, true),
          "stale generation is not retried");
    Check(!ShouldRetryInterruptedDelivery(true, false, true, true, false, true),
          "flush/segment invalidation is not retried");
    Check(!ShouldRetryInterruptedDelivery(true, false, true, false, true, true),
          "stop cancellation is not retried");
    Check(!ShouldRetryInterruptedDelivery(true, false, true, false, false, false),
          "real delivery error is not retried");

    Check(ShouldQueueCompletedFrameAfterPause(true, true, false, false, true, false),
          "completed GetFrame is retained during strict pause");
    Check(!ShouldQueueCompletedFrameAfterPause(true, false, false, false, true, false),
          "completed frame from stale generation is rejected");
    Check(!ShouldQueueCompletedFrameAfterPause(true, true, true, false, true, false),
          "completed frame during seek/flush is rejected");
    Check(!ShouldQueueCompletedFrameAfterPause(true, true, false, true, true, false),
          "completed frame after runtime failure is rejected");
    Check(!ShouldQueueCompletedFrameAfterPause(true, true, false, false, false, false),
          "unavailable output is not queued");
    Check(!ShouldQueueCompletedFrameAfterPause(true, true, false, false, true, true),
          "finished stream marker is not queued as a frame");

    if (failures != 0) {
        std::cerr << failures << " pause-retention policy checks failed\n";
        return 1;
    }
    std::cout << "23/23 pause-retention policy checks passed\n";
    return 0;
}
