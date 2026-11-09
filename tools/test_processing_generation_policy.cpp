#include "../directmotion/ProcessingGenerationPolicy.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS | " << description << '\n';
    } else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}
}

int main() {
    using DirectMotionProcessingGeneration::MatchesEngineGeneration;

    Check(!MatchesEngineGeneration(0, 0),
        "zero generations never authorize an engine-processing pass");
    Check(!MatchesEngineGeneration(1, 0),
        "a processing generation cannot use an engine that is absent or being reset");
    Check(!MatchesEngineGeneration(0, 1),
        "an engine generation cannot authorize a pass without a pipeline generation");
    Check(MatchesEngineGeneration(1, 1),
        "processing is allowed when FIFO and engine generations match");
    Check(MatchesEngineGeneration(42, 42),
        "the current generation remains valid after a completed engine rebuild");

    // Reproduce the FIX-5.5 Run window: Pause has already advanced the FIFO,
    // but the old engine remains installed until ResetEngine quiesces the worker.
    constexpr std::uint64_t oldGeneration = 17;
    constexpr std::uint64_t newGeneration = 18;
    const std::uint64_t fifoGenerationAfterPause = newGeneration;
    const std::uint64_t engineGenerationBeforeReset = oldGeneration;
    Check(!MatchesEngineGeneration(fifoGenerationAfterPause, engineGenerationBeforeReset),
        "regression: Run cannot relabel work from the pre-Pause engine as the new FIFO generation");

    // Once ResetEngine has quiesced and removed the old graph, generation zero
    // remains rejected until EnsureEngine publishes the rebuilt graph identity.
    const std::uint64_t engineGenerationDuringReset = 0;
    Check(!MatchesEngineGeneration(newGeneration, engineGenerationDuringReset),
        "the worker cannot enter the processing path while ResetEngine has no active engine");

    const std::uint64_t rebuiltEngineGeneration = newGeneration;
    Check(MatchesEngineGeneration(fifoGenerationAfterPause, rebuiltEngineGeneration),
        "processing resumes only after the rebuilt engine adopts the current FIFO generation");

    Check(!MatchesEngineGeneration(oldGeneration, rebuiltEngineGeneration),
        "a delayed old-generation worker pass is rejected after the rebuilt engine is published");
    Check(!MatchesEngineGeneration(7, 8),
        "any mismatched nonzero generation is rejected");
    Check(MatchesEngineGeneration(UINT64_C(0xFFFFFFFFFFFFFFFE),
            UINT64_C(0xFFFFFFFFFFFFFFFE)),
        "large equal generation identifiers compare without arithmetic or overflow");

    // Deterministically release a pending worker after Pause has advanced the
    // FIFO but before ResetEngine has replaced the old graph. This is the exact
    // ordering that previously let the worker adopt a new FIFO ID for old work.
    std::atomic<std::uint64_t> fifoGeneration{oldGeneration};
    std::atomic<std::uint64_t> activeEngineGeneration{oldGeneration};
    std::atomic<bool> oldEngineTouched{false};
    std::mutex wakeMutex;
    std::condition_variable wakeChanged;
    bool runGateOpened = false;
    std::thread pendingWorker([&]() {
        {
            std::unique_lock<std::mutex> lock(wakeMutex);
            wakeChanged.wait(lock, [&]() { return runGateOpened; });
        }
        const auto workerGeneration = fifoGeneration.load(std::memory_order_acquire);
        const auto workerEngineGeneration = activeEngineGeneration.load(std::memory_order_acquire);
        if (MatchesEngineGeneration(workerGeneration, workerEngineGeneration)) {
            oldEngineTouched.store(true, std::memory_order_release);
        }
    });
    fifoGeneration.store(newGeneration, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        runGateOpened = true;
    }
    wakeChanged.notify_one();
    pendingWorker.join();
    Check(!oldEngineTouched.load(std::memory_order_acquire),
        "concurrent regression: a pending worker woken after FIFO rollover cannot touch the old engine");

    // Complete reset/rebuild and publish another work notification. Only now
    // should the same generation pair admit processing again.
    activeEngineGeneration.store(newGeneration, std::memory_order_release);
    Check(MatchesEngineGeneration(
            fifoGeneration.load(std::memory_order_acquire),
            activeEngineGeneration.load(std::memory_order_acquire)),
        "concurrent regression: the rebuilt graph resumes work under the new FIFO generation");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}