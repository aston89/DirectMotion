#include "../directmotion/PauseFrameGate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

int checks = 0;
int failures = 0;

void Check(const std::string& name, bool condition) {
    ++checks;
    if (condition) std::cout << "PASS: " << name << '\n';
    else { ++failures; std::cout << "FAIL: " << name << '\n'; }
}

void TestOneShotPerPauseEpoch() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    Check("first decoded frame claims the current paused epoch", gate.TryClaim(epoch));
    Check("second decoded frame cannot claim the same paused epoch", !gate.TryClaim(epoch));
    gate.MarkPresented(epoch);
    Check("successful delivery marks exactly this epoch as presented", gate.WasPresented(epoch));
    Check("presented marker is not visible for another epoch", !gate.WasPresented(epoch + 1));

    const auto nextEpoch = gate.Invalidate();
    Check("Pause/seek invalidation advances the epoch", nextEpoch == epoch + 1);
    Check("old allocator request is no longer current", !gate.IsCurrent(epoch));
    Check("old presented marker cannot suppress the new epoch", !gate.WasPresented(nextEpoch));
    Check("new pause epoch admits exactly one decoded frame", gate.TryClaim(nextEpoch));
    Check("new epoch rejects a second decoded frame", !gate.TryClaim(nextEpoch));
}

void TestRunInvalidatesBlockedAllocatorRequest() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    std::mutex mutex;
    std::condition_variable changed;
    bool allocatorEntered = false;
    bool releaseAllocator = false;
    std::atomic<unsigned> downstreamDeliveries{0};
    std::atomic<bool> staleDiscarded{false};

    std::thread receive([&] {
        if (!gate.TryClaim(epoch)) return;
        {
            std::unique_lock<std::mutex> lock(mutex);
            allocatorEntered = true;
            changed.notify_all();
            changed.wait_for(lock, 1500ms, [&] { return releaseAllocator; });
        }
        // Mirrors DeliverOriginalSample's post-GetDeliveryBuffer epoch check.
        if (!gate.IsCurrent(epoch)) {
            staleDiscarded.store(true, std::memory_order_release);
            gate.ReleaseClaim(epoch);
            return;
        }
        downstreamDeliveries.fetch_add(1, std::memory_order_acq_rel);
        gate.MarkPresented(epoch);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, 500ms, [&] { return allocatorEntered; });
    }
    Check("paused frame enters a blocked renderer allocator request", allocatorEntered);
    bool pendingFrameRequest = false;
    const bool resumedAfterPresentedFrame = gate.AdvanceForRun(&pendingFrameRequest);
    // Model Run's local downstream BeginFlush: it actively releases the pending
    // allocator wait before Run attempts to acquire the delivery fence.
    if (pendingFrameRequest) {
        std::lock_guard<std::mutex> lock(mutex);
        releaseAllocator = true;
    }
    changed.notify_all();
    receive.join();

    Check("Run invalidates a paused-frame request that has not yet been presented", !resumedAfterPresentedFrame);
    Check("Run detects the claimed but not-yet-presented frame request", pendingFrameRequest);
    Check("Run-owned flush releases the blocked paused allocator wait", releaseAllocator);
    Check("allocator completion after Run is discarded", staleDiscarded.load(std::memory_order_acquire));
    Check("no stale paused frame reaches downstream after Run", downstreamDeliveries.load() == 0);
    Check("late allocator completion cannot mark the new lifecycle as presented", !gate.WasPresented(gate.CurrentEpoch()));
}

void TestRunCanRetryFailedLocalFlush() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    Check("Run retry test begins with an unpresented paused-frame claim", gate.TryClaim(epoch));
    bool firstRunPending = false;
    gate.AdvanceForRun(&firstRunPending);
    Check("first Run detects the outstanding claim before its local flush", firstRunPending);

    // Simulate DeliverBeginFlush failing: the request remains blocked and its
    // claim must survive so another Run cannot resume without retrying the flush.
    bool retryPending = false;
    gate.AdvanceForRun(&retryPending);
    Check("later Run retries cancellation while the old claim is still outstanding", retryPending);

    gate.ReleaseClaim(epoch);
    bool afterCompletionPending = true;
    gate.AdvanceForRun(&afterCompletionPending);
    Check("Run stops requesting a flush after the stale Receive releases its claim", !afterCompletionPending);
}

void TestOtherLifecycleInvalidations() {
    const char* transitions[] = {"Stop", "BeginFlush", "NewSegment/seek"};
    for (const char* transition : transitions) {
        PauseFrameGate gate;
        const auto oldEpoch = gate.CurrentEpoch();
        Check(std::string(transition) + " begins with one valid paused-frame request", gate.TryClaim(oldEpoch));
        gate.Invalidate();
        Check(std::string(transition) + " invalidates a delayed allocator completion", !gate.IsCurrent(oldEpoch));
        Check(std::string(transition) + " allows a fresh lifecycle epoch", gate.TryClaim(gate.CurrentEpoch()));
    }
}

void TestConcurrentClaimsAreOneShot() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    std::atomic<unsigned> successfulClaims{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 32; ++i) {
        threads.emplace_back([&] {
            if (gate.TryClaim(epoch)) successfulClaims.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    for (auto& thread : threads) thread.join();
    Check("32 concurrent Receive claims produce exactly one paused frame", successfulClaims.load() == 1);
}

void TestAllocatorFailureReleasesClaim() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    Check("first allocator attempt owns the paused-frame claim", gate.TryClaim(epoch));
    gate.ReleaseClaim(epoch); // maps to VFW_E_NOT_COMMITTED before any sample was presented
    Check("decommitted allocator releases the unpresented one-shot claim", gate.TryClaim(epoch));
    Check("retry still cannot create a second concurrent claim", !gate.TryClaim(epoch));
}

void TestRunConsumesPresentedMarker() {
    PauseFrameGate gate;
    const auto epoch = gate.CurrentEpoch();
    Check("frame is claimed before delivery", gate.TryClaim(epoch));
    gate.MarkPresented(epoch);
    bool pendingFrameRequest = true;
    Check("Run recognizes a frame already presented in the previous epoch", gate.AdvanceForRun(&pendingFrameRequest));
    Check("Run does not flush an already-presented paused frame", !pendingFrameRequest);
    Check("Run clears the old visible-frame marker", !gate.WasPresented(gate.CurrentEpoch()));
    Check("new paused epoch after Run can claim a new frame", gate.TryClaim(gate.CurrentEpoch()));
}
}

int main() {
    TestOneShotPerPauseEpoch();
    TestRunInvalidatesBlockedAllocatorRequest();
    TestRunCanRetryFailedLocalFlush();
    TestOtherLifecycleInvalidations();
    TestConcurrentClaimsAreOneShot();
    TestAllocatorFailureReleasesClaim();
    TestRunConsumesPresentedMarker();
    std::cout << "RESULT: " << checks << "/" << checks - failures << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
