#include "../directmotion/ProcessingContinuation.h"
#include "../directmotion/AsyncPipelineState.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using DirectMotionProcessingContinuation::ShouldRearmAfterPass;
constexpr unsigned kPassLimit = 128;

unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* name) {
    ++checks;
    if (condition) std::cout << "PASS: " << name << '\n';
    else {
        ++failures;
        std::cout << "FAIL: " << name << '\n';
    }
}

struct WorkerSignalModel {
    std::mutex mutex;
    bool pending = true;
    bool active = false;
    bool stopped = false;
    bool suspended = false;

    bool BeginPass() {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped || suspended || !pending) return false;
        pending = false;
        active = true;
        return true;
    }

    void NotifySourcePublished() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopped && !suspended) pending = true;
    }

    bool FinishPass(bool capped, bool generationCurrent = true,
                    bool transitioning = false, bool pausePending = false,
                    bool runtimeFailed = false, bool succeeded = true,
                    bool terminalDrain = false) {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
        if (ShouldRearmAfterPass(capped, terminalDrain, stopped, suspended,
                generationCurrent, transitioning, pausePending, runtimeFailed, succeeded)) {
            // Deliberately OR the internal continuation with any source signal
            // already published during this pass; never clear an external wake.
            pending = true;
        }
        return pending;
    }

    bool HasPending() {
        std::lock_guard<std::mutex> lock(mutex);
        return pending;
    }
};

struct BurstResult {
    uint64_t produced = 0;
    uint64_t consumed = 0;
    uint64_t passes = 0;
    bool queueOk = true;
    AsyncPipelineSnapshot snapshot{};
};

BurstResult RunEligibleBurst(uint64_t eligibleFrames) {
    AsyncPipelineState pipeline(8, AsyncPipelineState::kDefaultOutputCapacity);
    const auto generation = pipeline.BeginGeneration();
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> consumerOk{true};

    std::thread consumer([&]() {
        AsyncOutputFrame packet;
        for (;;) {
            const auto status = pipeline.WaitDequeueOutput(generation, packet);
            if (status == AsyncQueueStatus::Closed) break;
            if (status != AsyncQueueStatus::Item) {
                consumerOk.store(false, std::memory_order_release);
                break;
            }
            consumed.fetch_add(1, std::memory_order_acq_rel);
            if ((packet.sequence % 7) == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    });

    uint64_t produced = 0;
    uint64_t passes = 0;
    bool workPending = true;
    bool producerOk = true;
    while (workPending) {
        workPending = false;
        ++passes;
        unsigned emittedThisPass = 0;
        while (produced < eligibleFrames && emittedThisPass < kPassLimit) {
            AsyncOutputFrame packet{};
            packet.generation = generation;
            packet.sequence = static_cast<int64_t>(produced);
            if (pipeline.WaitEnqueueOutput(std::move(packet)) != AsyncQueueStatus::Queued) {
                producerOk = false;
                break;
            }
            ++produced;
            ++emittedThisPass;
        }

        const bool passLimitReached = emittedThisPass >= kPassLimit;
        workPending = ShouldRearmAfterPass(passLimitReached, false, false, false,
            pipeline.IsCurrentGeneration(generation), false, false, false, producerOk);
        if (!producerOk) break;
    }

    pipeline.MarkOutputComplete(generation);
    consumer.join();

    BurstResult result;
    result.produced = produced;
    result.consumed = consumed.load(std::memory_order_acquire);
    result.passes = passes;
    result.queueOk = producerOk && consumerOk.load(std::memory_order_acquire);
    result.snapshot = pipeline.Snapshot();
    return result;
}

} // namespace

int main() {
    // The cap itself must request a continuation only for ordinary successful work.
    Check(ShouldRearmAfterPass(true, false, false, false, true, false, false, false, true),
        "a successful non-terminal capped pass requests an internal continuation");
    Check(!ShouldRearmAfterPass(false, false, false, false, true, false, false, false, true),
        "a pass that found no further available output does not re-arm");
    Check(!ShouldRearmAfterPass(true, true, false, false, true, false, false, false, true),
        "terminal EOS drain keeps its existing uncapped drain path");
    Check(!ShouldRearmAfterPass(true, false, true, false, true, false, false, false, true),
        "Stop suppresses a capped-pass continuation");
    Check(!ShouldRearmAfterPass(true, false, false, true, true, false, false, false, true),
        "engine reset suspension suppresses a capped-pass continuation");
    Check(!ShouldRearmAfterPass(true, false, false, false, false, false, false, false, true),
        "a stale pipeline generation cannot schedule old work");
    Check(!ShouldRearmAfterPass(true, false, false, false, true, true, false, false, true),
        "Flush, Stop or segment transition suppresses a capped-pass continuation");
    Check(!ShouldRearmAfterPass(true, false, false, false, true, false, true, false, true),
        "Pause transition suppresses a capped-pass continuation");
    Check(!ShouldRearmAfterPass(true, false, false, false, true, false, false, true, true),
        "runtime failure suppresses a capped-pass continuation");
    Check(!ShouldRearmAfterPass(true, false, false, false, true, false, false, false, false),
        "a failed processing pass never schedules a continuation");

    // A source publication during a pass and the internal cap signal share one
    // mutex-protected pending bit; one must not overwrite the other.
    {
        WorkerSignalModel worker;
        Check(worker.BeginPass(), "worker consumes the initial pending signal and enters a pass");
        worker.NotifySourcePublished();
        Check(worker.FinishPass(true),
            "a source notification arriving during a capped pass remains pending");
        Check(worker.BeginPass(), "the pending source/internal signal starts the next pass");
        Check(!worker.FinishPass(false),
            "a non-capped follow-up pass returns to idle without a synthetic re-arm");
    }

    // Race an upstream notification against completion/re-arm repeatedly. The
    // pending signal must survive regardless of which side acquires the mutex first.
    {
        unsigned lostWakeups = 0;
        for (unsigned i = 0; i < 1000; ++i) {
            WorkerSignalModel worker;
            if (!worker.BeginPass()) { ++lostWakeups; continue; }
            std::thread notifier([&worker]() { worker.NotifySourcePublished(); });
            worker.FinishPass(true);
            notifier.join();
            if (!worker.HasPending()) ++lostWakeups;
        }
        Check(lostWakeups == 0,
            "1000 notification-versus-rearm races preserve the pending work signal");
    }

    const struct { uint64_t frames; uint64_t expectedPasses; const char* label; } bursts[] = {
        {128, 2, "128-output burst performs one bounded pass and one empty availability check"},
        {129, 2, "129-output burst continues without a later source publication"},
        {256, 3, "256-output burst continues across two capped passes"},
        {512, 5, "512-output burst continues through repeated bounded passes"},
        {1024, 9, "1024-output burst drains without a source publication after the first pass"}
    };

    for (const auto& burst : bursts) {
        const BurstResult result = RunEligibleBurst(burst.frames);
        Check(result.queueOk && result.produced == burst.frames && result.consumed == burst.frames,
            burst.label);
        Check(result.passes == burst.expectedPasses,
            "burst uses the expected number of bounded 128-output passes");
        Check(result.snapshot.output.highWater <= AsyncPipelineState::kDefaultOutputCapacity &&
              result.snapshot.output.queued == 0,
            "continuation preserves bounded output FIFO capacity and drains it fully");
        if (burst.frames > AsyncPipelineState::kDefaultOutputCapacity) {
            Check(result.snapshot.output.producerWaits > 0,
                "large burst remains backpressured by the existing bounded output FIFO");
        }
    }

    // A capped pass must not keep running after a lifecycle transition invalidates
    // the generation. This also guards against a re-arm busy loop after cancellation.
    {
        WorkerSignalModel worker;
        worker.BeginPass();
        worker.stopped = true;
        Check(!worker.FinishPass(true, true, false, false, false, true),
            "Stop racing with a capped pass leaves no continuation pending");
        Check(!worker.BeginPass(), "stopped worker cannot consume a stale continuation");
    }

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
