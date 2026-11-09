#pragma once

#include "FrucRational.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace DirectMotionSeekFrontier {

// The fallback watermark is inclusive. The first MVTools output we still need is
// exactly the next output slot; no earlier MBlockFps frame needs to be generated.
inline bool TryFirstUncoveredOutputIndex(bool hasFallbackWatermark,
                                         int64_t lastFallbackPts,
                                         int64_t& firstOutputIndex) noexcept {
    firstOutputIndex = 0;
    if (!hasFallbackWatermark) return true;
    if (lastFallbackPts < 0 || lastFallbackPts == (std::numeric_limits<int64_t>::max)())
        return false;
    firstOutputIndex = lastFallbackPts + 1;
    return true;
}

// Convert the first uncovered output slot to the source-frame coordinate that
// MBlockFps will evaluate. The source cache can use this as a bounded retention
// anchor while the decoder publishes the remaining lookahead; otherwise a long
// post-seek prefix can fill the cache before the frontier becomes evaluable.
// Once the source has been finalized, a first-uncovered index at or beyond
// the exact output count means the already queued fallback packets cover the
// entire remaining segment. No MVTools bootstrap is required; EOS can append
// its delivery barrier directly behind those packets.
inline bool FallbackCoversFinalizedOutput(int64_t firstUncoveredOutputIndex,
                                          int64_t expectedOutputCount) noexcept {
    return firstUncoveredOutputIndex >= 0 && expectedOutputCount >= 0 &&
        firstUncoveredOutputIndex >= expectedOutputCount;
}

inline bool TrySourceFrameAnchorForOutputIndex(int64_t outputIndex,
                                              FrucRational sourceRate,
                                              FrucRational targetRate,
                                              int64_t& sourceFrameAnchor) noexcept {
    sourceFrameAnchor = -1;
    if (outputIndex < 0 || sourceRate.num <= 0 || sourceRate.den <= 0 ||
        targetRate.num <= 0 || targetRate.den <= 0) return false;

    const long double sourcePosition =
        static_cast<long double>(outputIndex) *
        static_cast<long double>(sourceRate.num) *
        static_cast<long double>(targetRate.den) /
        (static_cast<long double>(sourceRate.den) *
         static_cast<long double>(targetRate.num));
    if (!std::isfinite(static_cast<double>(sourcePosition)) || sourcePosition < 0.0L ||
        sourcePosition > static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return false;

    sourceFrameAnchor = static_cast<int64_t>(std::floor(sourcePosition));
    return true;
}

// Match the backend's established temporal lookahead gate, but evaluate an
// arbitrary segment-local output index. This lets the worker wait for the source
// frames needed by the live fallback frontier before sealing further fallback.
inline bool HasSourceLookaheadForOutputIndex(int64_t outputIndex,
                                             int64_t inputFrameCount,
                                             int motionLookaheadFrames,
                                             FrucRational sourceRate,
                                             FrucRational targetRate) noexcept {
    if (outputIndex < 0 || inputFrameCount <= 0 ||
        sourceRate.num <= 0 || sourceRate.den <= 0 ||
        targetRate.num <= 0 || targetRate.den <= 0) return false;

    const int64_t newestInputIndex = inputFrameCount - 1;
    const int64_t safeInputIndex = newestInputIndex -
        (std::max)(0, motionLookaheadFrames);
    if (safeInputIndex < 0) return false;

    const long double outputTime = static_cast<long double>(outputIndex) *
        static_cast<long double>(sourceRate.num) * static_cast<long double>(targetRate.den);
    const long double safeSourceTime = static_cast<long double>(safeInputIndex) *
        static_cast<long double>(sourceRate.den) * static_cast<long double>(targetRate.num);
    return outputTime <= safeSourceTime;
}

} // namespace DirectMotionSeekFrontier
