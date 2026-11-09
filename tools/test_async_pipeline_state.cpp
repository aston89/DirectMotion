#include "../directmotion/AsyncPipelineState.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    std::cout << (condition ? "PASS" : "FAIL") << " | " << name << '\n';
    if (!condition) ++failures;
}

AsyncSourceFrame MakeInput(uint64_t generation, int64_t sequence) {
    AsyncSourceFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.sourcePts = sequence;
    frame.width = 4;
    frame.height = 2;
    frame.stride = 4;
    frame.nv12 = { static_cast<uint8_t>(sequence), 1, 2, 3 };
    return frame;
}

AsyncOutputFrame MakeOutput(uint64_t generation, int64_t sequence) {
    AsyncOutputFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.pts = sequence * 100;
    frame.outputLineSizeY = 4;
    frame.outputLineSizeUV = 4;
    frame.streamTimeOrigin100ns = 1234567;
    frame.targetFrameDuration100ns = 166833;
    frame.outputTimeBaseNumerator = 1001;
    frame.outputTimeBaseDenominator = 60000;
    frame.targetFpsNumerator = 60000;
    frame.targetFpsDenominator = 1001;
    frame.prerollBoundaryTime100ns = 2345678;
    frame.prerollBoundaryTimeValid = true;
    frame.nv12 = { static_cast<uint8_t>(sequence), 4, 5, 6 };
    return frame;
}
}

int main() {
    AsyncPipelineState state(2, 1);
    Check(state.CurrentGeneration() == 0, "new state starts dormant without an active generation");
    const auto generation = state.BeginGeneration();
    Check(generation == 1 && state.IsCurrentGeneration(generation), "BeginGeneration opens generation 1");

    Check(state.TryEnqueueInput(MakeInput(generation, 10)) == AsyncQueueStatus::Queued,
        "input queue accepts first source frame");
    Check(state.TryEnqueueInput(MakeInput(generation, 11)) == AsyncQueueStatus::Queued,
        "input queue accepts up to its configured capacity");
    Check(state.TryEnqueueInput(MakeInput(generation, 12)) == AsyncQueueStatus::Full,
        "input publication reports Full without blocking");
    const auto inputPressure = state.Snapshot();
    Check(inputPressure.input.enqueued == 2 && inputPressure.input.fullRejects == 1 &&
        inputPressure.input.highWater == 2,
        "input metrics count accepted frames, non-blocking full rejects and high-water depth");

    AsyncSourceFrame input;
    Check(state.WaitDequeueInput(generation, input) == AsyncQueueStatus::Item && input.sequence == 10,
        "input queue preserves FIFO ordering");
    Check(state.TryEnqueueInput(MakeInput(generation, 12)) == AsyncQueueStatus::Queued,
        "input queue reopens one slot after dequeue");

    Check(state.MarkInputComplete(generation), "input completion closes the producer side");
    Check(state.TryEnqueueInput(MakeInput(generation, 13)) == AsyncQueueStatus::Closed,
        "input metrics distinguish a closed producer from a full FIFO");
    Check(state.WaitDequeueInput(generation, input) == AsyncQueueStatus::Item && input.sequence == 11,
        "input completion preserves already queued frames");
    Check(state.WaitDequeueInput(generation, input) == AsyncQueueStatus::Item && input.sequence == 12,
        "input queue drains all accepted frames");
    Check(state.WaitDequeueInput(generation, input) == AsyncQueueStatus::Closed,
        "input consumer sees Closed after normal drain");

    Check(state.SetWorkerState(generation, AsyncWorkerKind::Processing, AsyncWorkerState::Running),
        "processing worker state is tracked per generation");
    Check(state.SetWorkerBusy(generation, AsyncWorkerKind::Processing, true) &&
        state.SetWorkerBusy(generation, AsyncWorkerKind::Processing, false),
        "processing worker busy/idle state is tracked independently from thread lifetime");
    Check(state.WaitEnqueueOutput(MakeOutput(generation, 20)) == AsyncQueueStatus::Queued,
        "output queue accepts first generated frame");

    std::atomic<bool> producerStarted{false};
    AsyncQueueStatus blockedPushResult = AsyncQueueStatus::Empty;
    std::thread blockedProducer([&]() {
        producerStarted.store(true, std::memory_order_release);
        blockedPushResult = state.WaitEnqueueOutput(MakeOutput(generation, 21));
    });
    while (!producerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    AsyncOutputFrame output;
    Check(state.WaitDequeueOutput(generation, output) == AsyncQueueStatus::Item && output.sequence == 20,
        "output queue preserves FIFO ordering");
    Check(output.streamTimeOrigin100ns == 1234567 && output.targetFrameDuration100ns == 166833 &&
        output.outputTimeBaseNumerator == 1001 && output.outputTimeBaseDenominator == 60000 &&
        output.prerollBoundaryTimeValid && output.prerollBoundaryTime100ns == 2345678,
        "output FIFO preserves immutable per-frame timestamp/preroll snapshots");
    blockedProducer.join();
    Check(blockedPushResult == AsyncQueueStatus::Queued,
        "processing producer resumes when delivery frees output capacity");
    const auto blockedMetrics = state.Snapshot();
    Check(blockedMetrics.output.producerWaits >= 1 && blockedMetrics.output.producerWaitMaxUs > 0 &&
        blockedMetrics.output.highWater == 1 && blockedMetrics.output.enqueued == 2 &&
        blockedMetrics.output.dequeued == 1,
        "output metrics record producer backpressure, wait duration, high-water and FIFO counts");
    Check(blockedMetrics.processingWorkItems == 1 && !blockedMetrics.processingBusy,
        "worker snapshot counts processing work transitions and exposes idle state");
    Check(state.WaitDequeueOutput(generation, output) == AsyncQueueStatus::Item && output.sequence == 21,
        "output queue returns the next generated frame");

    Check(state.MarkOutputComplete(generation), "output completion closes the delivery side");
    Check(state.WaitDequeueOutput(generation, output) == AsyncQueueStatus::Closed,
        "delivery consumer sees Closed after normal output drain");

    const auto nextGeneration = state.BeginGeneration();
    Check(nextGeneration == generation + 1, "new generation increments monotonically");
    Check(state.TryEnqueueInput(MakeInput(generation, 99)) == AsyncQueueStatus::StaleGeneration,
        "old-generation source frame is rejected");
    Check(state.Snapshot().input.staleRejects >= 1,
        "input metrics count stale-generation publication rejects");
    Check(state.IsCurrentGeneration(nextGeneration), "new generation becomes current");

    Check(state.WaitEnqueueOutput(MakeOutput(nextGeneration, 30)) == AsyncQueueStatus::Queued,
        "new generation accepts current output");

    std::atomic<bool> cancellationProducerStarted{false};
    AsyncQueueStatus cancelledPushResult = AsyncQueueStatus::Empty;
    std::thread cancellationProducer([&]() {
        cancellationProducerStarted.store(true, std::memory_order_release);
        cancelledPushResult = state.WaitEnqueueOutput(MakeOutput(nextGeneration, 31));
    });
    while (!cancellationProducerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Check(state.CancelGeneration(nextGeneration, "unit test cancellation"),
        "cancellation closes both queues and records its reason");
    cancellationProducer.join();
    Check(cancelledPushResult == AsyncQueueStatus::Cancelled,
        "cancellation wakes a processing producer blocked on full output capacity");
    Check(state.WaitDequeueOutput(nextGeneration, output) == AsyncQueueStatus::Cancelled,
        "cancellation wakes output consumers and discards queued stale output");
    const auto snapshot = state.Snapshot();
    Check(snapshot.phase == AsyncPipelinePhase::Cancelled && snapshot.cancelled &&
        snapshot.reason == "unit test cancellation", "snapshot exposes cancelled generation state");
    Check(snapshot.output.queued == 0 && snapshot.input.queued == 0 &&
        snapshot.output.discarded >= 1 && snapshot.output.cancelledRejects >= 1 &&
        snapshot.input.closedRejects >= 1,
        "queue metrics distinguish discarded packets and closed/cancelled/stale rejects");

    const auto ackGeneration = state.BeginGeneration();
    auto discardedCompletion = std::make_shared<AsyncOutputCompletion>();
    AsyncOutputFrame pendingAck = MakeOutput(ackGeneration, 35);
    pendingAck.drainBarrier = true;
    pendingAck.completion = discardedCompletion;
    Check(state.WaitEnqueueOutput(std::move(pendingAck)) == AsyncQueueStatus::Queued,
        "terminal barrier with completion acknowledgement enters the queue");
    const auto afterAckReset = state.BeginGeneration();
    Check(afterAckReset == ackGeneration + 1,
        "generation reset advances while an output frame is still queued");
    Check(static_cast<uint32_t>(discardedCompletion->Wait()) == 0x80004004u,
        "generation reset completes discarded terminal-barrier acknowledgement with E_ABORT");

    const auto barrierGeneration = state.BeginGeneration();
    Check(state.WaitEnqueueOutput(MakeOutput(barrierGeneration, 50)) == AsyncQueueStatus::Queued,
        "normal output packet enters the terminal-drain generation first");
    auto terminalCompletion = std::make_shared<AsyncOutputCompletion>();
    AsyncOutputFrame terminalBarrier;
    terminalBarrier.generation = barrierGeneration;
    terminalBarrier.sequence = 51;
    terminalBarrier.drainBarrier = true;
    terminalBarrier.completion = terminalCompletion;
    std::atomic<bool> barrierProducerStarted{false};
    AsyncQueueStatus terminalBarrierPush = AsyncQueueStatus::Empty;
    std::thread barrierProducer([&]() {
        barrierProducerStarted.store(true, std::memory_order_release);
        terminalBarrierPush = state.WaitEnqueueOutput(std::move(terminalBarrier));
    });
    while (!barrierProducerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(state.WaitDequeueOutput(barrierGeneration, output) == AsyncQueueStatus::Item &&
        output.sequence == 50 && !output.drainBarrier,
        "terminal drain preserves every ordinary packet ahead of the barrier");
    barrierProducer.join();
    Check(terminalBarrierPush == AsyncQueueStatus::Queued,
        "terminal barrier is enqueued once output capacity becomes available");
    Check(state.WaitDequeueOutput(barrierGeneration, output) == AsyncQueueStatus::Item &&
        output.drainBarrier && output.sequence == 51,
        "terminal barrier is observed only after the preceding output packet");
    const auto queueTotals = state.Snapshot();
    Check(queueTotals.output.enqueued >= queueTotals.output.dequeued &&
        queueTotals.output.highWater <= queueTotals.output.capacity,
        "queue metrics preserve bounded high-water and accepted/dequeued totals");
    terminalCompletion->Complete(0);
    Check(terminalCompletion->Wait() == 0,
        "terminal barrier acknowledgement completes after FIFO drain reaches the marker");

    const auto finalGeneration = state.BeginGeneration();
    Check(state.TryEnqueueInput(MakeInput(finalGeneration, 40)) == AsyncQueueStatus::Queued,
        "BeginGeneration recovers queues after cancellation");
    Check(state.FailGeneration(finalGeneration, "unit test failure"),
        "failure transitions the active generation to Failed");
    const auto failed = state.Snapshot();
    Check(failed.phase == AsyncPipelinePhase::Failed &&
        failed.processingWorker == AsyncWorkerState::Failed &&
        failed.deliveryWorker == AsyncWorkerState::Failed,
        "failure snapshot marks both worker paths failed");
    Check(state.WaitDequeueInput(finalGeneration, input) == AsyncQueueStatus::Cancelled,
        "failure interrupts source consumers");

    AsyncPipelineState idleState(1, 1);
    const auto idleGeneration = idleState.BeginGeneration();
    std::atomic<bool> consumerStarted{false};
    AsyncQueueStatus consumerWaitResult = AsyncQueueStatus::Empty;
    std::thread idleConsumer([&]() {
        consumerStarted.store(true, std::memory_order_release);
        AsyncOutputFrame waitedFrame;
        consumerWaitResult = idleState.WaitDequeueOutput(idleGeneration, waitedFrame);
        if (consumerWaitResult == AsyncQueueStatus::Item && waitedFrame.sequence != 77)
            consumerWaitResult = AsyncQueueStatus::Empty;
    });
    while (!consumerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(idleState.WaitEnqueueOutput(MakeOutput(idleGeneration, 77)) == AsyncQueueStatus::Queued,
        "output producer wakes a delivery worker blocked on an empty FIFO");
    idleConsumer.join();
    const auto idleMetrics = idleState.Snapshot();
    Check(consumerWaitResult == AsyncQueueStatus::Item && idleMetrics.output.consumerWaits == 1 &&
        idleMetrics.output.consumerWaitMaxUs > 0,
        "output metrics measure delivery-worker idle wait count and duration");
    Check(idleState.SetWorkerBusy(idleGeneration, AsyncWorkerKind::Delivery, true) &&
        idleState.SetWorkerBusy(idleGeneration, AsyncWorkerKind::Delivery, false) &&
        idleState.Snapshot().deliveryWorkItems == 1,
        "delivery worker busy transitions are counted independently from thread lifetime");

    AsyncPipelineState fallbackQueue(1, 1, 2);
    const auto fallbackGeneration = fallbackQueue.BeginGeneration();
    AsyncOutputFrame ordinaryWhileFallback = MakeOutput(fallbackGeneration, 88);
    ordinaryWhileFallback.pts = 8800;
    Check(fallbackQueue.TryEnqueueOutput(std::move(ordinaryWhileFallback)) == AsyncQueueStatus::Queued,
        "interpolated frame fills its own bounded output lane");
    AsyncOutputFrame fallbackPacket = MakeOutput(fallbackGeneration, 88);
    fallbackPacket.pts = 8800;
    fallbackPacket.seekFallback = true;
    fallbackPacket.seekFallbackFirstPacket = true;
    fallbackPacket.seekFallbackArmedTickNs = 123456789;
    fallbackPacket.repeatCount = 3;
    fallbackPacket.fallbackSourceFrameIndex = 7;
    auto sharedFallbackPayload = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>(96, 0x5a));
    const auto expectedFallbackPayload = sharedFallbackPayload;
    fallbackPacket.sharedNv12 = sharedFallbackPayload;
    sharedFallbackPayload.reset();
    Check(fallbackQueue.TryEnqueueOutput(std::move(fallbackPacket)) == AsyncQueueStatus::Queued,
        "progressive-seek fallback enters its independent priority lane while interpolation lane is full");
    AsyncOutputFrame secondFallback = MakeOutput(fallbackGeneration, 90);
    secondFallback.seekFallback = true;
    secondFallback.pts = 9000;
    Check(fallbackQueue.TryEnqueueOutput(std::move(secondFallback)) == AsyncQueueStatus::Queued,
        "fallback lane admits frames up to its own independent capacity");
    AsyncOutputFrame thirdFallback = MakeOutput(fallbackGeneration, 91);
    thirdFallback.seekFallback = true;
    Check(fallbackQueue.TryEnqueueOutput(std::move(thirdFallback)) == AsyncQueueStatus::Full,
        "fallback lane applies its own bounded non-blocking capacity");
    const auto fallbackMetrics = fallbackQueue.Snapshot();
    Check(fallbackMetrics.output.fullRejects == 0 && fallbackMetrics.output.queued == 1 &&
        fallbackMetrics.output.fallbackQueued == 2 && fallbackMetrics.output.fallbackFullRejects == 1,
        "fallback pressure is isolated from interpolation queue metrics");
    AsyncOutputFrame queuedFallback;
    Check(fallbackQueue.WaitDequeueOutput(fallbackGeneration, queuedFallback) == AsyncQueueStatus::Item &&
        queuedFallback.seekFallback && queuedFallback.repeatCount == 3 &&
        queuedFallback.fallbackSourceFrameIndex == 7 && queuedFallback.seekFallbackFirstPacket &&
        queuedFallback.seekFallbackArmedTickNs == 123456789 && queuedFallback.pts == 8800 &&
        queuedFallback.sharedNv12 == expectedFallbackPayload && queuedFallback.sharedNv12->size() == 96,
        "PTS merge gives source fallback priority on a tie and retains the shared NV12 payload/metadata");
    Check(fallbackQueue.WaitDequeueOutput(fallbackGeneration, queuedFallback) == AsyncQueueStatus::Item &&
        !queuedFallback.seekFallback && queuedFallback.sequence == 88,
        "single delivery consumer merges lanes by PTS after source wins an equal-PTS tie");
    Check(fallbackQueue.WaitDequeueOutput(fallbackGeneration, queuedFallback) == AsyncQueueStatus::Item &&
        queuedFallback.seekFallback && queuedFallback.fallbackSourceFrameIndex == -1,
        "single delivery consumer continues with the next source fallback packet in timestamp order");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
        << " (" << failures << " failed checks)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
