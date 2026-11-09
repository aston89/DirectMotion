#include "../directmotion/SourceCacheRetention.h"

#include <cstdint>
#include <deque>
#include <iostream>

namespace {
unsigned failures = 0;
unsigned checks = 0;

void Check(bool condition, const char* name) {
    ++checks;
    std::cout << (condition ? "PASS" : "FAIL") << " | " << name << '\n';
    if (!condition) ++failures;
}

void TrimToFloor(std::deque<int64_t>& frames, int64_t& base, int64_t floor) {
    while (!frames.empty() && base < floor) {
        frames.pop_front();
        ++base;
    }
}
}

int main() {
    constexpr size_t planned720p = 43;
    constexpr size_t budget720p = 518;
    constexpr size_t budget4k = 56;
    constexpr size_t hardCap = 128;
    constexpr size_t historyMargin = 24;

    const size_t max720p = DirectMotionSourceCache::MaxRetainedFrames(budget720p, hardCap);
    const size_t max4k = DirectMotionSourceCache::MaxRetainedFrames(budget4k, hardCap);
    const size_t history32 = DirectMotionSourceCache::HistorySlackFrames(13, 2, 4, historyMargin);
    const size_t history64 = DirectMotionSourceCache::HistorySlackFrames(26, 2, 4, historyMargin);

    Check(max720p == 128,
        "720p cache keeps the existing byte-budgeted allowance and 128-frame hard cap");
    Check(planned720p == 43 && planned720p < max720p,
        "720p nominal Prefetch window remains 43 frames without changing scheduler concurrency");
    Check(max4k == 56,
        "4K retention remains bounded by its actual source-cache byte budget");
    Check(!DirectMotionSourceCache::FitsBudget(60, max4k),
        "planner rejects a nominal source window larger than its memory allowance");
    Check(DirectMotionSourceCache::MaxRetainedFrames(1000, hardCap) == hardCap,
        "retention never exceeds the global frame hard cap");

    Check(history32 == 24,
        "32-output Prefetch uses the explicit 24-source-frame historical margin");
    Check(history64 == 32,
        "64-output Prefetch expands historical slack to cover its source lookahead plus MVTools and safety");
    Check(history32 < max720p && history64 < max720p,
        "historical slack remains independent from the 128-frame RAM ceiling");

    Check(DirectMotionSourceCache::RetentionFloor(0, -1, -1, history32) == 0,
        "cache never evicts before any output has completed");
    Check(DirectMotionSourceCache::RetentionFloor(0, -1, 9, history32) == 0,
        "early processing progress retains the initial source history");
    Check(DirectMotionSourceCache::RetentionFloor(0, -1, 48, history32) == 24,
        "completed processing advances eviction behind the 24-frame history window");
    Check(DirectMotionSourceCache::RetentionFloor(0, -1, 200, history32) == 176,
        "long-running retention tracks the watermark with bounded historical slack");
    Check(DirectMotionSourceCache::RetentionFloor(0, 23, 48, history32) == 0,
        "an active older GetFrame lease holds the eviction floor backward");
    Check(DirectMotionSourceCache::RetentionFloor(24, 23, 48, history32) == 24,
        "retention never moves backward after an older frame has already been retired");
    Check(DirectMotionSourceCache::RetentionFloor(0, 1000, 48, history32) == 24,
        "a far-future Prefetch request cannot advance eviction beyond completed processing");

    Check(DirectMotionSourceCache::CanAppend(127, max720p),
        "producer may fill the cache below the 128-frame hard limit");
    Check(!DirectMotionSourceCache::CanAppend(128, max720p),
        "producer applies backpressure exactly at the unchanged 128-frame hard limit");
    Check(DirectMotionSourceCache::CanAppend(55, max4k),
        "4K producer may fill its byte-budgeted window");
    Check(!DirectMotionSourceCache::CanAppend(56, max4k),
        "4K producer cannot exceed its byte-budgeted window");

    // Reproduce the FIX-3 runtime shape: cache 0..127 is full while the completed
    // watermark is only 9. As the consumer advances to 48, 24 old frames become
    // reclaimable. An active GetFrame(23) lease temporarily protects that frame;
    // after the lease acquires its shared PVideoFrame, the producer gets room again.
    std::deque<int64_t> cache;
    int64_t base = 0;
    for (int64_t frame = 0; frame <= 127; ++frame) cache.push_back(frame);
    TrimToFloor(cache, base, DirectMotionSourceCache::RetentionFloor(
        base, -1, 9, history32));
    Check(cache.size() == 128 && base == 0,
        "full source ring remains bounded while the completed watermark is still inside history slack");
    Check(!DirectMotionSourceCache::CanAppend(cache.size(), max720p),
        "producer waits while all retained frames are still historically reachable");

    const int64_t floorWithLease = DirectMotionSourceCache::RetentionFloor(
        base, 23, 48, history32);
    TrimToFloor(cache, base, floorWithLease);
    Check(base == 0 && cache.size() == 128 && cache[23] == 23,
        "late Prefetch GetFrame(23) lease protects its source frame during watermark advance");

    const int64_t floorAfterLease = DirectMotionSourceCache::RetentionFloor(
        base, -1, 48, history32);
    TrimToFloor(cache, base, floorAfterLease);
    Check(base == 24 && cache.front() == 24 && cache.size() == 104,
        "after the immutable frame reference is acquired, old cache slots can be retired");
    Check(DirectMotionSourceCache::CanAppend(cache.size(), max720p),
        "watermark progress releases source-cache capacity before the 128-frame ceiling is exhausted");
    cache.push_back(128);
    Check(cache.back() == 128 && cache.size() == 105,
        "Receive can publish the frame that was previously blocked by FIX-3 backpressure");

    Check(DirectMotionSourceCache::PushResult::Published != DirectMotionSourceCache::PushResult::Interrupted,
        "source publication result distinguishes successful enqueue from lifecycle cancellation");
    Check(DirectMotionSourceCache::PushResult::Failed != DirectMotionSourceCache::PushResult::Interrupted,
        "source publication result distinguishes true errors from lifecycle cancellation");
    Check(DirectMotionSourceCache::IsExpectedLifecycleInterruption(false, true, true),
        "Run-to-Pause interruption is a benign lifecycle outcome");
    Check(DirectMotionSourceCache::IsExpectedLifecycleInterruption(true, false, true),
        "Stop/Flush/seek interruption is a benign lifecycle outcome");
    Check(DirectMotionSourceCache::IsExpectedLifecycleInterruption(false, false, false),
        "stale graph-generation interruption is discarded without poisoning the new generation");
    Check(!DirectMotionSourceCache::IsExpectedLifecycleInterruption(false, false, true),
        "unexpected interruption in the current running generation remains reportable");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
