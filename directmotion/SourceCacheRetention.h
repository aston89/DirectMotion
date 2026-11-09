#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

// Pure source-cache retention policy shared by InputClip and portable regression
// tests. The byte-budgeted hard cap and the historical slack behind the completed
// consumer watermark are deliberately independent: using the RAM ceiling as the
// history window can prevent the watermark from ever reaching the eviction point.
namespace DirectMotionSourceCache {

enum class PushResult : uint8_t {
    Published = 0,
    Interrupted,
    Failed
};

inline size_t MaxRetainedFrames(size_t memoryBudgetFrames, size_t hardCap) noexcept {
    return (std::min)(memoryBudgetFrames, hardCap);
}

inline bool FitsBudget(size_t plannedCapacity, size_t retentionLimit) noexcept {
    return plannedCapacity != 0 && plannedCapacity <= retentionLimit;
}

// Preserve enough source history for a late Prefetch/MVTools request, but never
// let that history silently expand to the whole memory ceiling. The fixed margin
// absorbs scheduler wake-up skew; the dynamic component scales with the source
// span represented by the effective Prefetch window plus MVTools/safety lookahead.
inline size_t HistorySlackFrames(size_t prefetchSourceLookaheadFrames,
                                 size_t motionLookaheadFrames,
                                 size_t safetyFrames,
                                 size_t minimumHistoryMarginFrames) noexcept {
    const size_t maxSize = (std::numeric_limits<size_t>::max)();
    size_t required = prefetchSourceLookaheadFrames;
    required = required > maxSize - motionLookaheadFrames
        ? maxSize : required + motionLookaheadFrames;
    required = required > maxSize - safetyFrames
        ? maxSize : required + safetyFrames;
    return (std::max)(minimumHistoryMarginFrames, required);
}

// Returns the oldest source index that may be discarded. The source-retention
// anchor is normally advanced after a completed output, but a seek-frontier
// bootstrap may prime it to the source coordinate of the first output not already
// covered by fallback. The bounded history slack preserves the local MVTools
// neighborhood. An active GetFrame lease may move the floor backward.
inline int64_t RetentionFloor(int64_t currentBase, int64_t oldestActiveRequest,
                              int64_t sourceRetentionAnchor,
                              size_t historySlackFrames) noexcept {
    if (sourceRetentionAnchor < 0) return currentBase;

    int64_t safeAnchor = sourceRetentionAnchor;
    if (oldestActiveRequest >= 0)
        safeAnchor = (std::min)(safeAnchor, oldestActiveRequest);

    const size_t boundedHistory = (std::min)(historySlackFrames,
        static_cast<size_t>((std::numeric_limits<int64_t>::max)()));
    const int64_t history = static_cast<int64_t>(boundedHistory);
    const int64_t candidate = safeAnchor > history ? safeAnchor - history : 0;
    return (std::max)(currentBase, candidate);
}

inline bool CanAppend(size_t queuedFrames, size_t maxRetainedFrames) noexcept {
    return queuedFrames < maxRetainedFrames;
}

// Cancellation is benign only when an actual DirectShow lifecycle transition or
// a generation change explains it. An interrupted push while the same graph is
// otherwise live remains an actionable runtime failure.
inline bool IsExpectedLifecycleInterruption(bool graphTransitioning,
                                           bool pauseTransitionPending,
                                           bool generationStillCurrent) noexcept {
    return graphTransitioning || pauseTransitionPending || !generationStillCurrent;
}

} // namespace DirectMotionSourceCache
