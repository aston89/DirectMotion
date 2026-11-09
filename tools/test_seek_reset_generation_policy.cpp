#include "../directmotion/SeekResetGenerationPolicy.h"
#include "../directmotion/SourceBootstrapQueue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {
int failures = 0;
int checks = 0;
void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using DirectMotionSeekReset::Phase;
    using DirectMotionSeekReset::State;

    State state;
    Check(DirectMotionSeekReset::BeginDeferredReset(state, 100, true),
        "flush opens a deferred reset owned by the pre-segment generation");
    Check(state.generation == 100 && state.phase == Phase::AwaitingSegment &&
        state.deferredResetPending, "flush state explicitly waits for NewSegment");
    Check(!DirectMotionSeekReset::CanComplete(state, 100),
        "pre-segment generation cannot complete or bootstrap the replacement engine");
    Check(!DirectMotionSeekReset::CanComplete(state, 101),
        "a later generation cannot bootstrap before NewSegment rebinds ownership");

    Check(DirectMotionSeekReset::RebindForNewSegment(state, 101),
        "NewSegment rebinds deferred reset to the final segment generation");
    Check(state.generation == 101 && state.phase == Phase::AwaitingBootstrap &&
        state.deferredResetPending, "rebound reset enters the bootstrap-ready phase");
    Check(DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 101, true),
        "paused one-shot preserves an active bootstrap owned by the final NewSegment generation");
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 101, false),
        "paused one-shot does not preserve a bootstrap whose source queue is inactive");
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 100, true),
        "paused one-shot cannot preserve a bootstrap owned by a retired generation");
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 0, true),
        "paused one-shot cannot preserve a zero-generation bootstrap");
    Check(!DirectMotionSeekReset::CanComplete(state, 100),
        "old pre-segment bootstrap completion is rejected");
    Check(DirectMotionSeekReset::CanComplete(state, 101),
        "only the final NewSegment generation may complete reset");
    Check(DirectMotionSeekReset::Complete(state, 101) && state.phase == Phase::Idle,
        "completion clears only the owner generation");
    Check(!DirectMotionSeekReset::Complete(state, 100),
        "late old completion cannot clear a later reset");

    Check(DirectMotionSeekReset::BeginDeferredReset(state, 200, true),
        "a subsequent seek opens a fresh awaited-segment reset");
    Check(DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 200, true),
        "paused one-shot in the EndFlush-to-NewSegment window preserves deferred reset ownership");
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 200, false),
        "pre-segment paused one-shot requires the matching source queue to remain active");
    Check(DirectMotionSeekReset::BeginDeferredReset(state, 201, true),
        "overlapping flush transfers ownership to the newest flush generation");
    Check(state.generation == 201 && !DirectMotionSeekReset::CanComplete(state, 201),
        "overlapping flush remains gated until NewSegment");
    Check(DirectMotionSeekReset::RebindForNewSegment(state, 202) &&
        DirectMotionSeekReset::CanComplete(state, 202),
        "NewSegment after overlapping flush arms only the final generation");

    DirectMotionSeekReset::Reset(state);
    Check(DirectMotionSeekReset::BeginSynchronousResetAwaitingSegment(state, 300),
        "synchronous non-seek flush records segment ownership without deferred work");
    Check(!state.deferredResetPending && !DirectMotionSeekReset::CanComplete(state, 300),
        "synchronous reset does not create a bootstrap obligation");
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 300, true),
        "synchronous segment marker does not masquerade as a deferred bootstrap owner");
    Check(DirectMotionSeekReset::RebindForNewSegment(state, 301) &&
        state.phase == Phase::Idle && !state.deferredResetPending,
        "NewSegment consumes a synchronous reset marker without creating a worker reset");

    DirectMotionSeekReset::Reset(state);
    Check(!DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            state, 301, true),
        "ordinary paused frame-step without a deferred seek retains the synchronous reset path");

    // Reproduce the reported failure: a paused seek frame is delivered directly,
    // but must not call generic ResetEngine/Queue::Cancel while the seek still owns
    // the active shared-payload budget. The next running Receive must be able to
    // create the shared NV12 payload and stage it into the same generation.
    State pausedSeekState;
    Check(BeginDeferredReset(pausedSeekState, 500, true),
        "paused-seek scenario opens a deferred reset at EndFlush");
    DirectMotionSourceBootstrap::Queue pausedSeekQueue(4096);
    pausedSeekQueue.BeginGeneration(500, false);
    const bool preservePreSegmentOneShot =
        DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            pausedSeekState, 500, pausedSeekQueue.IsActiveForGeneration(500));
    Check(preservePreSegmentOneShot,
        "EndFlush-to-NewSegment paused one-shot keeps its queue generation alive");
    Check(pausedSeekQueue.IsActiveForGeneration(500),
        "pre-segment one-shot does not cancel source-payload admission");

    Check(DirectMotionSeekReset::RebindForNewSegment(pausedSeekState, 501),
        "NewSegment transfers paused-seek reset ownership to its final generation");
    pausedSeekQueue.BeginGeneration(501, true);
    const bool preserveFinalSegmentOneShot =
        DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
            pausedSeekState, 501, pausedSeekQueue.IsActiveForGeneration(501));
    Check(preserveFinalSegmentOneShot,
        "final-segment paused one-shot preserves its active bootstrap generation");

    std::vector<uint8_t> pausedSeekSource(512, 0x5a);
    auto pausedSeekPayload = pausedSeekQueue.CreatePayload(pausedSeekSource);
    Check(static_cast<bool>(pausedSeekPayload) && pausedSeekSource.empty(),
        "next source Receive can allocate a shared NV12 payload after the paused one-shot");
    Check(pausedSeekQueue.TryPush(501, 0, pausedSeekPayload, false) ==
            DirectMotionSourceBootstrap::PushResult::Queued,
        "next source Receive stages into the preserved final seek generation");
    pausedSeekPayload.reset();
    DirectMotionSourceBootstrap::Frame pausedSeekFrame;
    Check(pausedSeekQueue.WaitPop(pausedSeekFrame, [] { return false; }),
        "bootstrap worker consumes the first post-Run source frame");
    Check(pausedSeekFrame.generation == 501 && pausedSeekFrame.sourceFrameIndex == 0,
        "first post-Run source frame remains source index zero in the seek generation");
    pausedSeekQueue.CompleteFrame(pausedSeekFrame);
    Check(pausedSeekQueue.BytesOutstanding() == 0,
        "preserved paused-seek payload charge is released after bootstrap consumption");
    pausedSeekQueue.Cancel();

    // Reproduce EndFlush -> Receive -> NewSegment. Receive may enqueue pre-segment
    // samples, but the worker must not pop them and build an engine for generation 400.
    DirectMotionSourceBootstrap::Queue queue(4096);
    queue.BeginGeneration(400, false);
    std::vector<uint8_t> preSegmentPayload(512, 0x11);
    Check(queue.IsActiveForGeneration(400) && !queue.IsWorkerReadyForGeneration(400),
        "pre-NewSegment generation admits Receive but keeps bootstrap worker gated");
    Check(queue.TryPush(400, 0, preSegmentPayload) ==
        DirectMotionSourceBootstrap::PushResult::Queued,
        "Receive can stage the intermediate pre-segment sample without blocking");
    std::atomic<bool> popped{false};
    std::thread worker([&] {
        DirectMotionSourceBootstrap::Frame frame;
        const bool got = queue.WaitPop(frame, [] { return false; });
        if (got) {
            popped.store(true, std::memory_order_release);
            queue.CompleteFrame(frame);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Check(!popped.load(std::memory_order_acquire),
        "bootstrap worker cannot consume pre-segment frame before NewSegment");
    queue.BeginGeneration(401, true);
    Check(queue.IsWorkerReadyForGeneration(401),
        "NewSegment opens the worker only for the final segment generation");
    std::vector<uint8_t> finalPayload(512, 0x22);
    Check(queue.TryPush(401, 0, finalPayload) ==
        DirectMotionSourceBootstrap::PushResult::Queued,
        "final segment accepts its first decoded frame");
    worker.join();
    Check(popped.load(std::memory_order_acquire),
        "worker wakes and consumes the final segment frame after NewSegment");
    Check(queue.BytesOutstanding() == 0 && queue.IsActiveForGeneration(401),
        "pre-segment queue invalidation and final frame completion preserve bounded accounting");
    queue.Cancel();

    std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << checks << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
