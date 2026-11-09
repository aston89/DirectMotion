#include "../directmotion/ProcessingQuiescence.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
int checks = 0;
int failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    } else {
        std::cout << "PASS | " << description << '\n';
    }
}
}

int main() {
    using DirectMotionProcessingQuiescence::IsQuiescentForEngineReset;
    using DirectMotionProcessingQuiescence::Snapshot;

    Check(IsQuiescentForEngineReset({false, false, false, false}),
        "engine reset is safe when the processing thread is absent and no pass is active");
    Check(IsQuiescentForEngineReset({true, false, true, false}),
        "engine reset is safe when a started worker is idle and suspension closes its wake predicate");
    Check(IsQuiescentForEngineReset({true, true, false, false}),
        "engine reset is safe when a stopped worker is idle");
    Check(!IsQuiescentForEngineReset({true, false, false, false}),
        "an idle but unsuspended worker is not quiescent because it may start a pass");
    Check(!IsQuiescentForEngineReset({true, true, false, true}),
        "a stop request does not authorize engine reset while a pass is still active");
    Check(!IsQuiescentForEngineReset({true, false, true, true}),
        "a suspension request does not authorize engine reset while a pass is still active");
    Check(!IsQuiescentForEngineReset({false, true, true, true}),
        "an active pass remains protected even if thread-lifetime flags are inconsistent");
    Check(IsQuiescentForEngineReset({true, true, true, false}),
        "concurrent Stop and Suspend are quiescent only after the active pass exits");

    // Model the lifecycle fence held by ResetEngine across both the quiescence
    // check and engine destruction. Start/Stop must use the same mutex.
    std::mutex lifecycleMutex;
    std::mutex signalMutex;
    std::condition_variable signalChanged;
    bool resetHasLifecycleFence = false;
    bool releaseReset = false;
    bool startAttempted = false;
    bool startAcquired = false;
    bool engineReleased = false;
    std::thread resetThread([&]() {
        std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex);
        {
            std::lock_guard<std::mutex> signalLock(signalMutex);
            resetHasLifecycleFence = true;
        }
        signalChanged.notify_all();
        std::unique_lock<std::mutex> signalLock(signalMutex);
        signalChanged.wait(signalLock, [&]() { return releaseReset; });
        engineReleased = true;
    });
    {
        std::unique_lock<std::mutex> signalLock(signalMutex);
        signalChanged.wait(signalLock, [&]() { return resetHasLifecycleFence; });
    }
    std::thread startThread([&]() {
        {
            std::lock_guard<std::mutex> signalLock(signalMutex);
            startAttempted = true;
        }
        signalChanged.notify_all();
        std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex);
        {
            std::lock_guard<std::mutex> signalLock(signalMutex);
            startAcquired = true;
        }
        signalChanged.notify_all();
    });
    {
        std::unique_lock<std::mutex> signalLock(signalMutex);
        signalChanged.wait(signalLock, [&]() { return startAttempted; });
    }
    const bool resetFenceBlocksLifecycle = !lifecycleMutex.try_lock();
    if (!resetFenceBlocksLifecycle) lifecycleMutex.unlock();
    Check(resetFenceBlocksLifecycle,
        "ResetEngine lifecycle fence blocks concurrent ProcessingWorker start or join");
    {
        std::lock_guard<std::mutex> signalLock(signalMutex);
        releaseReset = true;
    }
    signalChanged.notify_all();
    resetThread.join();
    startThread.join();
    Check(startAcquired,
        "ProcessingWorker lifecycle operation resumes after engine destruction leaves the fence");
    Check(engineReleased,
        "engine release completes inside the same serialized lifecycle window");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
