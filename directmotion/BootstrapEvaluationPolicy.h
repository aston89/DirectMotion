#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>

// The first uncovered MBlockFps output is fetched directly. Prefetch is trimmed
// to the following output so its relative frame zero is useful work, not a duplicate
// of the direct bootstrap frame.
namespace DirectMotionBootstrapEvaluation {

inline bool UsesPrefetchForOutput(int64_t outputIndex, int64_t firstPrefetchOutputIndex) noexcept {
    return outputIndex >= firstPrefetchOutputIndex;
}

inline int64_t PrefetchRelativeIndex(int64_t outputIndex, int64_t firstPrefetchOutputIndex) noexcept {
    return outputIndex >= firstPrefetchOutputIndex ? outputIndex - firstPrefetchOutputIndex : -1;
}

inline int RequiredSourceFramesForFirstOutput(int motionLookaheadFrames) noexcept {
    const int safeLookahead = (std::max)(0, motionLookaheadFrames);
    return safeLookahead >= INT_MAX ? INT_MAX : safeLookahead + 1;
}

} // namespace DirectMotionBootstrapEvaluation
