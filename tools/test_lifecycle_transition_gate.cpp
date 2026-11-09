#include "../directmotion/LifecycleTransitionGate.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using DirectMotionLifecycle::TransitionGate;
using DirectMotionLifecycle::TransitionLease;

namespace {
int checks = 0;
int failures = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (condition) std::cout << "PASS | " << message << "\n";
    else { ++failures; std::cout << "FAIL | " << message << "\n"; }
}
}

int main() {
    TransitionGate gate;
    Check(!gate.Active() && gate.Count() == 0, "gate starts inactive");

    gate.Acquire();
    gate.Acquire();
    Check(gate.Active() && gate.Count() == 2, "overlapping lifecycle callbacks are independently counted");
    gate.Release();
    Check(gate.Active() && gate.Count() == 1, "completion of an older callback cannot clear the newer callback");
    gate.Release();
    Check(!gate.Active(), "gate becomes inactive only after every callback releases its lease");

    gate.Release();
    Check(!gate.Active() && gate.Count() == 0, "duplicate release is harmless and cannot underflow");

    {
        TransitionLease outer(gate);
        Check(gate.Count() == 1, "scope lease acquires one transition");
        {
            TransitionLease inner(gate);
            Check(gate.Count() == 2, "nested scope lease increments independently");
            inner.Release();
            inner.Release();
            Check(gate.Count() == 1, "scope lease explicit release is idempotent");
        }
        Check(gate.Count() == 1, "released inner lease destructor cannot clear outer lease");
    }
    Check(!gate.Active(), "outer scope destructor releases its own transition");

    std::atomic<bool> firstEntered{false};
    std::atomic<bool> secondEntered{false};
    std::atomic<bool> releaseFirst{false};
    std::atomic<bool> releaseSecond{false};
    std::thread first([&] {
        TransitionLease lease(gate);
        firstEntered.store(true, std::memory_order_release);
        while (!releaseFirst.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!firstEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread second([&] {
        TransitionLease lease(gate);
        secondEntered.store(true, std::memory_order_release);
        while (!releaseSecond.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!secondEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    Check(gate.Count() == 2, "concurrent Flush/NewSegment-style callbacks hold separate leases");
    releaseFirst.store(true, std::memory_order_release);
    first.join();
    Check(gate.Active() && gate.Count() == 1, "first concurrent callback exit leaves second transition active");
    releaseSecond.store(true, std::memory_order_release);
    second.join();
    Check(!gate.Active(), "last concurrent callback exit releases the gate");

    gate.Acquire();
    gate.Reset();
    Check(!gate.Active(), "quiescent streaming restart resets lifecycle state");

    std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << checks << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
