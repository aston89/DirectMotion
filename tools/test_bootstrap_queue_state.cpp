#include "../directmotion/AsyncPipelineState.h"
#include "../directmotion/BootstrapQueueState.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
int checks = 0;
int failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS | " << description << '\n';
    } else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}

AsyncOutputFrame MakeOutput(uint64_t generation, int64_t sequence, bool preroll) {
    AsyncOutputFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.pts = sequence * 100;
    frame.explicitPreroll = preroll;
    frame.nv12 = { 1, 2, 3, 4, 5, 6 };
    return frame;
}
}

int main() {
    BootstrapQueueState bootstrap;
    AsyncPipelineState pipeline(2, 2);
    const auto generation = pipeline.BeginGeneration();

    Check(!bootstrap.HasBeenEnqueued(),
        "a fresh engine generation requires one bootstrap packet");

    // Paused startup/seek preroll is allowed to enter the FIFO before Run. Its
    // explicitPreroll bit prevents display; queued-state must not depend on Run.
    const bool paused = true;
    const bool runActive = false;
    const auto enqueueBootstrap = pipeline.WaitEnqueueOutput(
        MakeOutput(generation, 100, true));
    Check(paused && !runActive && enqueueBootstrap == AsyncQueueStatus::Queued,
        "paused preroll is accepted by the output FIFO before Run");
    if (enqueueBootstrap == AsyncQueueStatus::Queued) bootstrap.MarkEnqueued();
    Check(bootstrap.HasBeenEnqueued(),
        "bootstrap state commits immediately after successful FIFO enqueue");

    std::mutex deliveryMutex;
    std::condition_variable deliveryChanged;
    bool deliveryBlocked = false;
    bool releaseDelivery = false;
    AsyncOutputFrame deliveredBootstrap;
    AsyncOutputFrame deliveredNext;
    AsyncQueueStatus firstPop = AsyncQueueStatus::Empty;
    AsyncQueueStatus secondPop = AsyncQueueStatus::Empty;

    // Model DeliveryWorker after it has removed the bootstrap packet from the FIFO
    // but is blocked in allocator/Deliver. No acknowledgement is returned to the
    // processing worker for normal asynchronous frames.
    std::thread slowDelivery([&]() {
        firstPop = pipeline.WaitDequeueOutput(generation, deliveredBootstrap);
        {
            std::unique_lock<std::mutex> lock(deliveryMutex);
            deliveryBlocked = true;
            deliveryChanged.notify_all();
            deliveryChanged.wait(lock, [&]() { return releaseDelivery; });
        }
        secondPop = pipeline.WaitDequeueOutput(generation, deliveredNext);
    });

    {
        std::unique_lock<std::mutex> lock(deliveryMutex);
        deliveryChanged.wait(lock, [&]() { return deliveryBlocked; });
    }
    Check(firstPop == AsyncQueueStatus::Item && deliveredBootstrap.sequence == 100 &&
        deliveredBootstrap.explicitPreroll,
        "delivery worker receives the explicit bootstrap preroll as the first FIFO item");
    Check(bootstrap.HasBeenEnqueued(),
        "bootstrap remains queued-state while downstream delivery is stalled");
    const bool bootstrapRequestNeededWhileDeliveryStalled = !bootstrap.HasBeenEnqueued();
    Check(!bootstrapRequestNeededWhileDeliveryStalled,
        "slow downstream delivery cannot trigger a duplicate bootstrap request");

    Check(pipeline.WaitEnqueueOutput(MakeOutput(generation, 101, false)) == AsyncQueueStatus::Queued,
        "normal output can follow the bootstrap while downstream is blocked");
    const auto blockedSnapshot = pipeline.Snapshot();
    Check(blockedSnapshot.output.queued == 1 && blockedSnapshot.output.highWater <= 2,
        "the output FIFO stays bounded while downstream owns the in-flight bootstrap");

    {
        std::lock_guard<std::mutex> lock(deliveryMutex);
        releaseDelivery = true;
    }
    deliveryChanged.notify_all();
    slowDelivery.join();

    Check(secondPop == AsyncQueueStatus::Item && deliveredNext.sequence == 101 &&
        !deliveredNext.explicitPreroll,
        "the first ordinary output follows bootstrap in FIFO order");
    Check(bootstrap.HasBeenEnqueued(),
        "successful renderer delivery does not redefine the FIFO-enqueued state");

    const auto nextGeneration = pipeline.BeginGeneration();
    bootstrap.Reset();
    Check(nextGeneration == generation + 1 && !bootstrap.HasBeenEnqueued(),
        "engine generation reset clears the bootstrap-enqueued state");
    Check(pipeline.WaitEnqueueOutput(MakeOutput(generation, 200, true)) ==
        AsyncQueueStatus::StaleGeneration && !bootstrap.HasBeenEnqueued(),
        "stale-generation enqueue cannot commit the new bootstrap state");

    Check(pipeline.CancelGeneration(nextGeneration, "bootstrap enqueue cancellation"),
        "test cancels the replacement generation before bootstrap publication");
    const auto cancelledEnqueue = pipeline.WaitEnqueueOutput(
        MakeOutput(nextGeneration, 201, true));
    if (cancelledEnqueue == AsyncQueueStatus::Queued) bootstrap.MarkEnqueued();
    Check(cancelledEnqueue == AsyncQueueStatus::Cancelled && !bootstrap.HasBeenEnqueued(),
        "cancelled enqueue leaves bootstrap state false");

    const auto recoveredGeneration = pipeline.BeginGeneration();
    bootstrap.Reset();
    Check(recoveredGeneration == nextGeneration + 1 && !bootstrap.HasBeenEnqueued(),
        "a recovered generation starts with exactly one new bootstrap opportunity");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
