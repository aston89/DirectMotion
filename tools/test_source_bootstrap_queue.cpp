#include "../directmotion/SourceBootstrapQueue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
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

std::vector<uint8_t> MakeFrame(size_t bytes, uint8_t seed) {
    return std::vector<uint8_t>(bytes, seed);
}
}

int main() {
    using namespace DirectMotionSourceBootstrap;

    Queue queue(100);
    Check(queue.WaitForAdmission(101, [] { return false; }) == AdmissionResult::TooLarge,
        "oversized frame is rejected instead of waiting forever");
    queue.BeginGeneration(7);
    Check(queue.IsActiveForGeneration(7), "generation begins active");
    Check(queue.TryPush(7, 0, MakeFrame(70, 1)) == PushResult::Queued,
        "first frame admitted");
    Check(queue.BytesOutstanding() == 70 && queue.QueuedFrames() == 1,
        "queued bytes accounted");
    Check(queue.ShouldBackpressure(40), "byte budget applies backpressure");

    Frame first;
    Check(queue.WaitPop(first, [] { return false; }), "worker pops staged frame");
    Check(first.sourceFrameIndex == 0 && queue.QueuedFrames() == 0,
        "source index preserved after pop");
    Check(queue.BytesOutstanding() == 70,
        "in-flight frame remains charged to byte budget");
    Check(queue.TryCompleteGeneration(7) == false,
        "generation cannot complete while frame is in flight");

    std::atomic<bool> admissionFinished{ false };
    std::atomic<int> admissionResult{ -1 };
    std::thread admissionWaiter([&] {
        const AdmissionResult result = queue.WaitForAdmission(40, [] { return false; });
        admissionResult.store(result == AdmissionResult::Ready ? 1 : 0, std::memory_order_release);
        admissionFinished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(!admissionFinished.load(std::memory_order_acquire),
        "admission waits while in-flight payload consumes budget");
    queue.CompleteFrame(first);
    admissionWaiter.join();
    Check(admissionFinished.load(std::memory_order_acquire) &&
        admissionResult.load(std::memory_order_acquire) == 1,
        "admission resumes after frame publication completes");
    Check(queue.BytesOutstanding() == 0, "completed frame releases budget");
    queue.CompleteFrame(first);
    Check(queue.BytesOutstanding() == 0 && first.chargedBytes == 0 && !first.nv12,
        "frame completion is idempotent for an RAII cleanup path");
    Check(queue.TryCompleteGeneration(7), "empty generation completes atomically");
    Check(!queue.IsActiveForGeneration(7), "completed generation becomes direct-ingest ready");

    queue.BeginGeneration(8);
    Check(queue.TryPush(7, 1, MakeFrame(10, 2)) == PushResult::StaleGeneration,
        "prior generation rejected");
    auto oversized = MakeFrame(101, 3);
    Check(queue.TryPush(8, 0, std::move(oversized)) == PushResult::Full,
        "frame larger than budget rejected");
    Check(oversized.size() == 101,
        "rejected rvalue payload remains owned by Receive caller");
    Check(queue.TryPush(8, 0, MakeFrame(60, 4)) == PushResult::Queued,
        "new generation accepts frame");
    Frame staleInFlight;
    Check(queue.WaitPop(staleInFlight, [] { return false; }), "new-generation frame popped");

    queue.BeginGeneration(9);
    Check(!queue.IsCurrent(staleInFlight), "new generation invalidates in-flight old frame");
    Check(queue.BytesOutstanding() == 60,
        "stale in-flight old generation remains charged against the global byte budget");
    Check(queue.TryPush(9, 0, MakeFrame(60, 5)) == PushResult::Full,
        "new generation cannot exceed byte budget while stale in-flight pixels remain allocated");
    queue.CompleteFrame(staleInFlight);
    Check(queue.BytesOutstanding() == 0,
        "completion of stale in-flight frame releases its own global budget charge");

    Check(queue.TryPush(9, 0, MakeFrame(30, 6)) == PushResult::Queued,
        "current generation accepts first frame");
    Check(queue.TryPush(9, 1, MakeFrame(30, 7)) == PushResult::Queued,
        "current generation accepts second frame");
    Frame ordered0, ordered1;
    Check(queue.WaitPop(ordered0, [] { return false; }) &&
        queue.WaitPop(ordered1, [] { return false; }), "two staged frames pop");
    Check(ordered0.sourceFrameIndex == 0 && ordered1.sourceFrameIndex == 1,
        "FIFO preserves source frame ordering");
    queue.CompleteFrame(ordered0);
    queue.CompleteFrame(ordered0);
    Check(ordered0.completed && ordered0.chargedBytes == 0,
        "repeated completion of one staged frame is idempotent");
    Check(!queue.TryCompleteGeneration(9),
        "duplicate completion cannot retire a generation while a sibling frame is still in flight");
    queue.CompleteFrame(ordered1);
    Check(queue.TryCompleteGeneration(9), "drained generation completes");

    queue.BeginGeneration(10);
    auto generationFrame = MakeFrame(90, 7);
    auto generationPayload = queue.CreatePayload(generationFrame);
    Check(generationPayload && queue.TryPush(10, 0, generationPayload) == PushResult::Queued,
        "cancellation test frame admitted");
    std::atomic<bool> admissionWaitPredicateEntered{ false };
    std::atomic<int> cancelledResult{ -1 };
    std::thread cancelledWaiter([&] {
        const AdmissionResult result = queue.WaitForAdmission(20, [&] {
            admissionWaitPredicateEntered.store(true, std::memory_order_release);
            return false;
        });
        cancelledResult.store(result == AdmissionResult::Cancelled ? 1 : 0,
            std::memory_order_release);
    });
    const auto admissionWaitDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!admissionWaitPredicateEntered.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < admissionWaitDeadline) {
        std::this_thread::yield();
    }
    Check(admissionWaitPredicateEntered.load(std::memory_order_acquire),
        "admission cancellation test observes waiter entering its condition predicate");
    queue.BeginGeneration(11);
    cancelledWaiter.join();
    Check(cancelledResult.load(std::memory_order_acquire) == 1,
        "generation change cancels old admission waiter");
    Check(queue.BytesOutstanding() == 90,
        "retired staging reference is cleared while an independently owned old payload stays charged");
    generationPayload.reset();
    Check(queue.BytesOutstanding() == 0, "final old-generation payload owner releases pending memory");

    queue.BeginGeneration(12);
    std::atomic<bool> stopped{ true };
    Frame unused;
    Check(!queue.WaitPop(unused, [&] { return stopped.load(std::memory_order_acquire); }),
        "worker wait exits on stop predicate");
    queue.Cancel();
    Check(queue.BytesOutstanding() == 0 && queue.QueuedFrames() == 0,
        "cancel releases all queued staging memory");

    // Initial decoder preroll can precede NewSegment. The queue must retain its
    // FIFO payloads while dormant, then atomically rebind them to the announced
    // generation and release the consumer only after downstream segment delivery.
    Queue initialStage(128);
    initialStage.BeginGeneration(30, false);
    initialStage.SetFallbackAdmissionActive(false);
    Check(initialStage.IsActiveForGeneration(30) &&
        !initialStage.IsWorkerReadyForGeneration(30),
        "initial-segment staging begins active with its worker gated");
    Check(initialStage.TryPush(30, 0, MakeFrame(40, 11)) == PushResult::Queued &&
        initialStage.TryPush(30, 1, MakeFrame(40, 12)) == PushResult::Queued,
        "pre-segment preroll frames are retained in source-index order");
    std::atomic<bool> initialStagePopped{ false };
    std::atomic<uint64_t> initialStageGeneration{ 0 };
    std::atomic<int64_t> initialStageIndex{ -1 };
    std::thread initialStageWorker([&] {
        Frame staged;
        if (initialStage.WaitPop(staged, [] { return false; })) {
            initialStageGeneration.store(staged.generation, std::memory_order_release);
            initialStageIndex.store(staged.sourceFrameIndex, std::memory_order_release);
            initialStagePopped.store(true, std::memory_order_release);
            initialStage.CompleteFrame(staged);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(!initialStagePopped.load(std::memory_order_acquire),
        "source-bootstrap consumer cannot publish initial preroll before NewSegment");
    Check(initialStage.RebindDormantGeneration(30, 31),
        "first NewSegment rebinds the complete dormant queue without dropping frames");
    Check(!initialStage.IsCurrent(Frame{30, 1, 0, 40, false, {}}),
        "old pre-segment generation is stale after queue rebinding");
    Check(initialStage.SetWorkerReadyForGeneration(31, true),
        "downstream-accepted NewSegment opens the rebound source-bootstrap worker");
    initialStageWorker.join();
    Check(initialStagePopped.load(std::memory_order_acquire) &&
        initialStageGeneration.load(std::memory_order_acquire) == 31 &&
        initialStageIndex.load(std::memory_order_acquire) == 0,
        "worker receives staged preroll under the new generation and original source index");
    Frame initialStageSecond;
    Check(initialStage.WaitPop(initialStageSecond, [] { return false; }) &&
        initialStageSecond.generation == 31 && initialStageSecond.sourceFrameIndex == 1,
        "second staged preroll frame retains FIFO order after generation handoff");
    initialStage.CompleteFrame(initialStageSecond);
    initialStage.SetFallbackAdmissionActive(false);
    Check(initialStage.TryCompleteGeneration(31),
        "rebound initial stage drains and releases its bounded admission budget");

    // If NewSegment arrives before the first Receive, the same dormant queue is
    // rebound empty. The first post-segment Receive still enters this generation-
    // owned staging lane, avoiding a separate startup publication path.
    Queue emptyInitialStage(64);
    emptyInitialStage.BeginGeneration(32, false);
    emptyInitialStage.SetFallbackAdmissionActive(false);
    Check(emptyInitialStage.RebindDormantGeneration(32, 33),
        "NewSegment-before-Receive rebinds an empty dormant initial queue");
    Check(emptyInitialStage.SetWorkerReadyForGeneration(33, true),
        "empty initial queue opens only after downstream NewSegment succeeds");
    Check(emptyInitialStage.TryPush(33, 0, MakeFrame(32, 13)) == PushResult::Queued,
        "first post-segment Receive enters the rebound initial staging generation");
    Frame firstPostSegment;
    Check(emptyInitialStage.WaitPop(firstPostSegment, [] { return false; }) &&
        firstPostSegment.generation == 33 && firstPostSegment.sourceFrameIndex == 0,
        "bootstrap worker consumes the first post-segment sample in order");
    emptyInitialStage.CompleteFrame(firstPostSegment);
    emptyInitialStage.SetFallbackAdmissionActive(false);
    Check(emptyInitialStage.TryCompleteGeneration(33),
        "empty-first initial stage completes after its first sample is published");

    // The output fallback lane and source bootstrap hold the same immutable
    // payload. Staging completion must not release the byte charge while the
    // renderer still owns the fallback packet.
    queue.BeginGeneration(13);
    auto sharedSource = MakeFrame(60, 8);
    auto sharedPayload = queue.CreatePayload(sharedSource);
    Check(sharedPayload && sharedSource.empty() && queue.BytesOutstanding() == 60,
        "shared payload transfers Receive storage and charges its bytes once");
    auto fallbackReference = sharedPayload;
    Check(queue.TryPush(13, 0, sharedPayload, false) == PushResult::Queued,
        "bootstrap staging accepts the same shared payload without an early worker wake");
    Frame sharedStaged;
    Check(queue.WaitPop(sharedStaged, [] { return false; }),
        "bootstrap worker obtains its shared payload reference");
    sharedPayload.reset();
    queue.CompleteFrame(sharedStaged);
    Check(queue.TryCompleteGeneration(13),
        "bootstrap generation can finish after InputClip publication");
    Check(queue.BytesOutstanding() == 60 && queue.ShouldBackpressure(50),
        "fallback delivery keeps the original frame charged after staging has drained");
    std::atomic<bool> fallbackAdmissionWaitPredicateEntered{ false };
    std::atomic<bool> fallbackAdmissionFinished{ false };
    std::atomic<int> fallbackAdmissionResult{ -1 };
    std::thread fallbackAdmissionWaiter([&] {
        const AdmissionResult result = queue.WaitForAdmission(50, [&] {
            fallbackAdmissionWaitPredicateEntered.store(true, std::memory_order_release);
            return false;
        });
        fallbackAdmissionResult.store(result == AdmissionResult::Ready ? 1 : 0,
            std::memory_order_release);
        fallbackAdmissionFinished.store(true, std::memory_order_release);
    });
    const auto fallbackWaitDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!fallbackAdmissionWaitPredicateEntered.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < fallbackWaitDeadline) {
        std::this_thread::yield();
    }
    Check(fallbackAdmissionWaitPredicateEntered.load(std::memory_order_acquire),
        "fallback admission test observes waiter entering its condition predicate");
    Check(!fallbackAdmissionFinished.load(std::memory_order_acquire),
        "fallback-only payloads continue to participate in source admission backpressure");
    fallbackReference.reset();
    fallbackAdmissionWaiter.join();
    Check(fallbackAdmissionFinished.load(std::memory_order_acquire) &&
        fallbackAdmissionResult.load(std::memory_order_acquire) == 1 &&
        queue.BytesOutstanding() == 0,
        "final delivery reference releases the shared byte charge and wakes Receive");
    queue.SetFallbackAdmissionActive(false);
    Check(!queue.ShouldBackpressure(90),
        "completed PTS handoff releases fallback admission backpressure");

    queue.BeginGeneration(14);
    auto pendingStage = MakeFrame(90, 9);
    Check(queue.TryPush(14, 0, pendingStage) == PushResult::Queued,
        "new staged generation reactivates the shared source byte budget");
    queue.SetFallbackAdmissionActive(false);
    Check(queue.ShouldBackpressure(20),
        "sealing fallback cannot disable admission while source bootstrap still has staged work");
    Frame pendingStageFrame;
    Check(queue.WaitPop(pendingStageFrame, [] { return false; }),
        "worker obtains the final staged frame after fallback has been sealed");
    queue.CompleteFrame(pendingStageFrame);
    Check(queue.TryCompleteGeneration(14) && !queue.ShouldBackpressure(20),
        "the byte budget closes after both fallback handoff and staging completion");

    std::cout << "SOURCE_BOOTSTRAP_QUEUE_TESTS " << (failures == 0 ? "PASS" : "FAIL")
        << " checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
