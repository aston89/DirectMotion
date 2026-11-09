#include "../directmotion/PauseReceiveBackpressure.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct ReceiveHarness {
    PauseReceiveBackpressure backpressure;
    std::mutex receiveLock;
    std::atomic<bool> runActive{false};
    std::atomic<bool> pauseHoldActive{false};
    std::atomic<bool> pausedFramePresented{false};
    std::atomic<int> acceptedSample{-1};
    std::atomic<unsigned> acceptedCount{0};

    bool MustWait() const noexcept {
        return pauseHoldActive.load(std::memory_order_acquire) ||
            (!runActive.load(std::memory_order_acquire) &&
             pausedFramePresented.load(std::memory_order_acquire));
    }

    PauseReceiveBackpressure::WaitResult WaitForAdmission() noexcept {
        const auto token = backpressure.Capture();
        if (!MustWait()) return PauseReceiveBackpressure::WaitResult::Resumed;
        return backpressure.WaitForChange(token);
    }

    void Receive(int sampleId) {
        for (;;) {
            const auto waitResult = WaitForAdmission();
            if (waitResult != PauseReceiveBackpressure::WaitResult::Resumed) return;

            std::unique_lock<std::mutex> lock(receiveLock);
            // Mirrors CTransformInputPin's second admission check after sample
            // properties have been refreshed under m_csReceive.
            if (MustWait()) {
                lock.unlock();
                continue;
            }
            acceptedSample.store(sampleId, std::memory_order_release);
            acceptedCount.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
    }
};

int checks = 0;
int failures = 0;

void Check(const std::string& name, bool condition) {
    ++checks;
    if (condition) std::cout << "PASS: " << name << '\n';
    else { ++failures; std::cout << "FAIL: " << name << '\n'; }
}

bool WaitForWaiter(const PauseReceiveBackpressure& gate) {
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (gate.WaitingCount() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return gate.WaitingCount() != 0;
}

void TestRunToPauseKeepsLastImageAndHoldsExactNextSample() {
    ReceiveHarness harness;
    // Strict Running -> Paused hold does not depend on a frame being presented
    // by DirectMotion during the pause. The renderer already owns the last image.
    harness.pauseHoldActive.store(true, std::memory_order_release);
    std::thread receiver([&] { harness.Receive(47); });

    Check("Run-to-Pause blocks the next Receive without presenting a paused step frame",
          WaitForWaiter(harness.backpressure) && !harness.pausedFramePresented.load());
    Check("held source sample is neither acknowledged nor processed while paused",
          harness.acceptedSample.load(std::memory_order_acquire) == -1 && harness.acceptedCount.load() == 0);

    harness.runActive.store(true, std::memory_order_release);
    harness.pauseHoldActive.store(false, std::memory_order_release);
    harness.backpressure.NotifyRun();
    receiver.join();

    Check("Run releases the exact source sample held at the pause boundary",
          harness.acceptedSample.load(std::memory_order_acquire) == 47);
    Check("held source sample is admitted exactly once", harness.acceptedCount.load() == 1);
    Check("Run notification advances the resume epoch", harness.backpressure.RunEpoch() == 1);
}

void TestReceiveLockIsNotHeldDuringStrictPauseWait() {
    ReceiveHarness harness;
    harness.pauseHoldActive.store(true, std::memory_order_release);
    std::thread receiver([&] { harness.Receive(11); });
    Check("Receive waits before attempting to own the receive lock",
          WaitForWaiter(harness.backpressure));

    const bool receiveLockAvailable = harness.receiveLock.try_lock();
    Check("lifecycle callbacks can acquire the receive lock while source Receive waits", receiveLockAvailable);
    if (receiveLockAvailable) harness.receiveLock.unlock();

    harness.runActive.store(true, std::memory_order_release);
    harness.pauseHoldActive.store(false, std::memory_order_release);
    harness.backpressure.NotifyRun();
    receiver.join();
    Check("waiting Receive exits cleanly after the lock-availability probe", harness.acceptedSample.load() == 11);
}

void TestConcurrentReceiveRechecksAfterLock() {
    ReceiveHarness harness;
    harness.runActive.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> held(harness.receiveLock);
    std::atomic<bool> started{false};
    std::thread receiver([&] {
        started.store(true, std::memory_order_release);
        harness.Receive(82);
    });
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    Check("concurrent Receive reaches its first admission check", started.load());
    Check("first admission check may pass before Pause commits", !harness.backpressure.WaitingCount());

    // Pause wins while this callback is waiting for m_csReceive. The post-lock
    // check must release the lock and retry admission rather than process a frame.
    harness.pauseHoldActive.store(true, std::memory_order_release);
    harness.runActive.store(false, std::memory_order_release);
    held.unlock();
    Check("post-lock admission detects strict pause hold and waits again",
          WaitForWaiter(harness.backpressure));
    Check("racing sample does not change the displayed-frame state or enter processing",
          harness.acceptedSample.load(std::memory_order_acquire) == -1 && harness.acceptedCount.load() == 0);

    harness.runActive.store(true, std::memory_order_release);
    harness.pauseHoldActive.store(false, std::memory_order_release);
    harness.backpressure.NotifyRun();
    receiver.join();
    Check("racing sample is processed once after Run", harness.acceptedSample.load() == 82 && harness.acceptedCount.load() == 1);
}

void TestInitialPausedStartupStillAllowsFirstFrame() {
    ReceiveHarness harness;
    // Stopped -> Paused and a paused seek intentionally retain the legacy
    // one-shot first-frame path. Strict hold applies only to Running -> Paused.
    harness.Receive(93);
    Check("initial Paused preroll/frame path is not blocked by the new Run-to-Pause hold",
          harness.acceptedSample.load() == 93 && harness.acceptedCount.load() == 1);
}

void TestLifecycleCancellationReleasesWaiter() {
    for (const char* lifecycle : {"Stop", "BeginFlush", "NewSegment/seek", "runtime failure"}) {
        ReceiveHarness harness;
        harness.pauseHoldActive.store(true, std::memory_order_release);
        PauseReceiveBackpressure::WaitResult result = PauseReceiveBackpressure::WaitResult::Resumed;
        const auto token = harness.backpressure.Capture();
        std::thread waiter([&] { result = harness.backpressure.WaitForChange(token); });
        Check(std::string(lifecycle) + " test registers a held source Receive",
              WaitForWaiter(harness.backpressure));
        harness.pauseHoldActive.store(false, std::memory_order_release);
        harness.backpressure.Cancel();
        waiter.join();
        Check(std::string(lifecycle) + " wakes the pending Receive as cancellation",
              result == PauseReceiveBackpressure::WaitResult::Cancelled);
        Check(std::string(lifecycle) + " does not masquerade as Run", harness.backpressure.RunEpoch() == 0);
    }
}

void TestImmediatePauseAfterRunReholdsThePendingSample() {
    ReceiveHarness harness;
    harness.pauseHoldActive.store(true, std::memory_order_release);
    std::thread receiver([&] { harness.Receive(101); });
    Check("first Pause registers the exact pending source sample", WaitForWaiter(harness.backpressure));

    // Hold the receive lock while Run wakes the callback, then immediately enter a
    // new Pause. The post-lock check must notice the new hold and wait for that Run.
    std::unique_lock<std::mutex> held(harness.receiveLock);
    harness.runActive.store(true, std::memory_order_release);
    harness.pauseHoldActive.store(false, std::memory_order_release);
    harness.backpressure.NotifyRun();
    harness.pauseHoldActive.store(true, std::memory_order_release);
    harness.runActive.store(false, std::memory_order_release);
    held.unlock();

    Check("immediate second Pause re-blocks the same sample", WaitForWaiter(harness.backpressure));
    Check("sample is not admitted between adjacent Run/Pause transitions",
          harness.acceptedSample.load() == -1 && harness.acceptedCount.load() == 0);

    harness.runActive.store(true, std::memory_order_release);
    harness.pauseHoldActive.store(false, std::memory_order_release);
    harness.backpressure.NotifyRun();
    receiver.join();
    Check("the same pending sample is admitted after the next Run", harness.acceptedSample.load() == 101 && harness.acceptedCount.load() == 1);
}

void TestCancellationIsNotLostAcrossRestartStateReset() {
    PauseReceiveBackpressure gate;
    const auto token = gate.Capture();
    gate.Cancel();
    // Simulate Stop followed by StartStreaming resetting ordinary lifecycle flags.
    // The monotonic cancel epoch must still make the old callback exit.
    const auto result = gate.WaitForChange(token);
    Check("Stop cancellation remains observable after later lifecycle flag reset",
          result == PauseReceiveBackpressure::WaitResult::Cancelled);
}

} // namespace

int main() {
    TestRunToPauseKeepsLastImageAndHoldsExactNextSample();
    TestReceiveLockIsNotHeldDuringStrictPauseWait();
    TestConcurrentReceiveRechecksAfterLock();
    TestInitialPausedStartupStillAllowsFirstFrame();
    TestLifecycleCancellationReleasesWaiter();
    TestImmediatePauseAfterRunReholdsThePendingSample();
    TestCancellationIsNotLostAcrossRestartStateReset();
    std::cout << "RESULT: " << (checks - failures) << '/' << checks
              << " checks passed; " << failures << " failed.\n";
    return failures == 0 ? 0 : 1;
}
