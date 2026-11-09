#include "../directmotion/ReceiveCommitPolicy.h"

#include <atomic>
#include <cstdint>
#include <iostream>

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
    using DirectMotionReceiveCommitPolicy::CanCommit;

    Check(CanCommit(true, false),
        "a sample may commit while its captured pipeline generation is current");
    Check(!CanCommit(false, false),
        "a retired pipeline generation cannot commit even when no lifecycle flag remains set");
    Check(!CanCommit(true, true),
        "a current generation cannot commit while Stop/Flush/NewSegment is being announced");
    Check(!CanCommit(false, true),
        "a retired generation remains rejected during an active lifecycle transition");

    // A second seek may invalidate the generation while deferred teardown is in
    // progress. The post-reset gate must prevent the old sample from constructing
    // a replacement graph.
    constexpr std::uint64_t capturedGeneration = 71;
    std::atomic<std::uint64_t> pipelineGeneration{capturedGeneration};
    std::atomic<bool> graphTransitioning{false};
    const auto gate = [&]() noexcept {
        const auto before = graphTransitioning.load(std::memory_order_acquire);
        const bool sameGeneration = pipelineGeneration.load(std::memory_order_acquire) == capturedGeneration;
        const auto after = graphTransitioning.load(std::memory_order_acquire);
        return CanCommit(sameGeneration, before || after);
    };

    Check(gate(), "the captured generation passes before a second seek begins");
    pipelineGeneration.store(capturedGeneration + 1, std::memory_order_release);
    Check(!gate(), "a second seek between fallback admission and EnsureEngine rejects the stale sample");

    // Model a generation change during EnsureEngine. The sample may have entered
    // initialization while current, but its post-initialization gate must prevent
    // it from publishing into the retired source cache.
    pipelineGeneration.store(capturedGeneration, std::memory_order_release);
    Check(gate(), "the sample passes the gate immediately before engine initialization");
    pipelineGeneration.store(capturedGeneration + 2, std::memory_order_release);
    Check(!gate(), "a generation retired during EnsureEngine cannot reach PushSourceNv12");

    pipelineGeneration.store(capturedGeneration, std::memory_order_release);
    graphTransitioning.store(true, std::memory_order_release);
    Check(!gate(), "the lifecycle announcement rejects a sample before generation rollover completes");
    graphTransitioning.store(false, std::memory_order_release);
    Check(gate(), "the new current generation can commit after the transition fully clears");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
