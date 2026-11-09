#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

enum class DeliveryResult { Delivered, AbortedByFlush, TimedOut };

struct MockDownstream {
    std::mutex mutex;
    std::condition_variable changed;
    bool flushing = false;
    bool failBeginFlush = false;
    int beginCount = 0;
    int endCount = 0;

    bool BeginFlush() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failBeginFlush) return false;
            flushing = true;
            ++beginCount;
        }
        changed.notify_all();
        return true;
    }

    void EndFlush() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            flushing = false;
            ++endCount;
        }
        changed.notify_all();
    }

    DeliveryResult GetDeliveryBuffer(std::atomic<bool>& entered) {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true, std::memory_order_release);
        const bool released = changed.wait_for(lock, 2s, [this] { return flushing; });
        return released ? DeliveryResult::AbortedByFlush : DeliveryResult::TimedOut;
    }

    DeliveryResult DeliverSample(std::atomic<bool>& entered) {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true, std::memory_order_release);
        const bool released = changed.wait_for(lock, 2s, [this] { return flushing; });
        return released ? DeliveryResult::AbortedByFlush : DeliveryResult::TimedOut;
    }
};

struct PauseModel {
    std::recursive_timed_mutex deliveryFence;
    MockDownstream downstream;
    std::atomic<bool> pausePending{false};
    std::atomic<bool> sourceCancelled{false};
    std::atomic<bool> runActive{true};
    std::atomic<bool> pauseCall{false};
    std::atomic<unsigned> generation{7};
    std::atomic<unsigned> delivered{0};
    std::atomic<bool> terminalPacketDiscardedAsPreroll{false};
    std::atomic<unsigned> orderingSequence{0};
    std::atomic<unsigned> beginFlushOrder{0};
    std::atomic<unsigned> sourceCancelOrder{0};
    std::atomic<unsigned> fenceAcquireOrder{0};

    bool Pause(bool runningToPaused, bool externalFlushActive,
               bool stopAlreadyFlushing, bool terminalEosActive) {
        pauseCall.store(true, std::memory_order_release);
        if (!runningToPaused) {
            pauseCall.store(false, std::memory_order_release);
            return true;
        }

        runActive.store(false, std::memory_order_release);
        pausePending.store(true, std::memory_order_release);

        const bool localFlush = !externalFlushActive && !stopAlreadyFlushing;
        if (localFlush && !downstream.BeginFlush()) {
            pausePending.store(false, std::memory_order_release);
            runActive.store(true, std::memory_order_release);
            pauseCall.store(false, std::memory_order_release);
            return false;
        }

        // Record the downstream abort owner before source cancellation. In a real
        // graph an external Flush/Stop has already sent its BeginFlush at this point.
        beginFlushOrder.store(orderingSequence.fetch_add(1, std::memory_order_acq_rel) + 1,
                              std::memory_order_release);

        // Only interrupt Prefetch source waits after our downstream BeginFlush
        // succeeded (or an external Flush/Stop already owns the transition).
        // Otherwise a failed BeginFlush rollback could leave a live Run interrupted.
        if (!terminalEosActive) {
            sourceCancelled.store(true, std::memory_order_release);
            sourceCancelOrder.store(orderingSequence.fetch_add(1, std::memory_order_acq_rel) + 1,
                                    std::memory_order_release);
        }

        // The key ordering under test: downstream flush releases a worker blocked
        // while holding deliveryFence; only then may Pause acquire the fence.
        std::unique_lock<std::recursive_timed_mutex> fence(deliveryFence, std::defer_lock);
        if (!fence.try_lock_for(1500ms)) {
            pauseCall.store(false, std::memory_order_release);
            return false;
        }
        fenceAcquireOrder.store(orderingSequence.fetch_add(1, std::memory_order_acq_rel) + 1,
                                std::memory_order_release);
        if (!terminalEosActive) ++generation;
        if (localFlush) downstream.EndFlush();
        if (terminalEosActive) {
            // DeliveryWorker treats this expected terminal-packet cancellation as
            // an accepted non-visible preroll so the ordered EOS barrier can proceed.
            terminalPacketDiscardedAsPreroll.store(true, std::memory_order_release);
        }
        fence.unlock();
        pauseCall.store(false, std::memory_order_release);
        return true;
    }
};

bool WaitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return flag.load(std::memory_order_acquire);
}

int checks = 0;
int failures = 0;

void Check(const std::string& name, bool condition) {
    ++checks;
    if (condition) std::cout << "PASS: " << name << '\n';
    else { ++failures; std::cout << "FAIL: " << name << '\n'; }
}

void TestAllocatorWaitIsReleasedBeforeFence() {
    PauseModel model;
    std::atomic<bool> inAllocator{false};
    DeliveryResult deliveryResult = DeliveryResult::TimedOut;

    std::thread worker([&] {
        std::lock_guard<std::recursive_timed_mutex> fence(model.deliveryFence);
        inAllocator.store(true, std::memory_order_release);
        deliveryResult = model.downstream.GetDeliveryBuffer(inAllocator);
        if (deliveryResult == DeliveryResult::Delivered &&
            !model.pausePending.load(std::memory_order_acquire))
            ++model.delivered;
    });

    Check("delivery worker entered allocator wait while owning the delivery fence", WaitFor(inAllocator));
    const bool paused = model.Pause(true, false, false, false);
    worker.join();

    Check("Pause completes after downstream BeginFlush releases GetDeliveryBuffer", paused);
    Check("allocator wait exits as an expected flush cancellation", deliveryResult == DeliveryResult::AbortedByFlush);
    Check("Pause retires the old output generation", model.generation.load() == 8);
    Check("local downstream flush is balanced exactly once", model.downstream.beginCount == 1 && model.downstream.endCount == 1);
    Check("no stale frame is delivered after the Pause transition", model.delivered.load() == 0);
    Check("source cancellation is published before Pause returns", model.sourceCancelled.load());
    Check("downstream BeginFlush precedes source cancellation and both precede the delivery fence",
          model.beginFlushOrder.load() < model.sourceCancelOrder.load() &&
          model.sourceCancelOrder.load() < model.fenceAcquireOrder.load());
}

void TestTerminalEosKeepsGeneration() {
    PauseModel model;
    std::atomic<bool> inAllocator{false};
    DeliveryResult deliveryResult = DeliveryResult::TimedOut;
    std::thread worker([&] {
        std::lock_guard<std::recursive_timed_mutex> fence(model.deliveryFence);
        deliveryResult = model.downstream.GetDeliveryBuffer(inAllocator);
    });
    Check("terminal test worker enters allocator wait", WaitFor(inAllocator));
    const bool paused = model.Pause(true, false, false, true);
    worker.join();

    Check("Pause completes while terminal EOS delivery is waiting", paused);
    Check("terminal EOS generation is retained", model.generation.load() == 7);
    Check("active terminal EOS source requests are not interrupted by Pause", !model.sourceCancelled.load());
    Check("interrupted terminal packet is classified as non-visible preroll", model.terminalPacketDiscardedAsPreroll.load());
    Check("terminal local flush is balanced", model.downstream.beginCount == 1 && model.downstream.endCount == 1);
}

void TestDownstreamDeliverIsReleasedBeforeFence() {
    PauseModel model;
    std::atomic<bool> inDeliver{false};
    DeliveryResult deliveryResult = DeliveryResult::TimedOut;
    std::thread worker([&] {
        std::lock_guard<std::recursive_timed_mutex> fence(model.deliveryFence);
        deliveryResult = model.downstream.DeliverSample(inDeliver);
        if (deliveryResult == DeliveryResult::Delivered &&
            !model.pausePending.load(std::memory_order_acquire))
            ++model.delivered;
    });
    Check("delivery worker entered a downstream Deliver wait while owning the fence", WaitFor(inDeliver));
    const bool paused = model.Pause(true, false, false, false);
    worker.join();

    Check("Pause completes after downstream BeginFlush releases Deliver", paused);
    Check("in-flight downstream Deliver exits as an expected flush cancellation", deliveryResult == DeliveryResult::AbortedByFlush);
    Check("generation is retired before delivery reopens", model.generation.load() == 8);
    Check("Deliver flush is balanced", model.downstream.beginCount == 1 && model.downstream.endCount == 1);
    Check("in-flight sample is not counted as delivered", model.delivered.load() == 0);
}

void TestExternalFlushOwnsPairing() {
    PauseModel model;
    Check("external flush starts", model.downstream.BeginFlush());
    const bool paused = model.Pause(true, true, false, false);
    Check("Pause acquires the fence when an external flush already owns downstream", paused);
    Check("Pause does not issue a nested BeginFlush", model.downstream.beginCount == 1);
    Check("Pause does not prematurely pair the external EndFlush", model.downstream.endCount == 0);
    Check("source cancellation follows the already-owned external flush", model.sourceCancelled.load() &&
          model.beginFlushOrder.load() < model.sourceCancelOrder.load());
    model.downstream.EndFlush();
    Check("external owner can pair its own EndFlush", model.downstream.beginCount == model.downstream.endCount);
}

void TestBeginFlushFailureRollsBack() {
    PauseModel model;
    model.downstream.failBeginFlush = true;
    const bool paused = model.Pause(true, false, false, false);
    Check("Pause is rejected when downstream BeginFlush fails", !paused);
    Check("failed BeginFlush rolls back the running state", model.runActive.load() && !model.pausePending.load());
    Check("failed BeginFlush does not retire the output generation", model.generation.load() == 7);
    Check("failed BeginFlush does not interrupt source-cache requests", !model.sourceCancelled.load());
    Check("failed BeginFlush does not call EndFlush", model.downstream.endCount == 0);
}

void TestNoDeliveryInFlightAndIdempotentPause() {
    PauseModel model;
    const bool firstPause = model.Pause(true, false, false, false);
    Check("Pause works with no delivery operation in flight", firstPause);
    Check("no-inflight Pause still balances downstream flush", model.downstream.beginCount == 1 && model.downstream.endCount == 1);
    Check("no-inflight Pause advances generation once", model.generation.load() == 8);

    const unsigned before = model.generation.load();
    const int beginBefore = model.downstream.beginCount;
    const bool secondPause = model.Pause(false, false, false, false);
    Check("idempotent Pause on an already paused filter succeeds", secondPause);
    Check("idempotent Pause does not flush or advance generation", model.generation.load() == before && model.downstream.beginCount == beginBefore);
}

} // namespace

int main() {
    TestAllocatorWaitIsReleasedBeforeFence();
    TestDownstreamDeliverIsReleasedBeforeFence();
    TestTerminalEosKeepsGeneration();
    TestExternalFlushOwnsPairing();
    TestBeginFlushFailureRollsBack();
    TestNoDeliveryInFlightAndIdempotentPause();
    std::cout << "RESULT: " << (checks - failures) << '/' << checks
              << " checks passed; " << failures << " failed.\n";
    return failures == 0 ? 0 : 1;
}
