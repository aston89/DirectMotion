#pragma once

#include "FrucRational.h"

#include <cstdint>
#include <limits>

namespace DirectMotionSeekFallback {

struct OutputSpan {
    int64_t firstPts = 0;
    int64_t endPtsExclusive = 0;
    uint32_t repeatCount = 0;
};

// Maps one decoded source-frame interval to the target output-frame lattice.
// Rounding each interval boundary (rather than multiplying a rounded frame
// duration) keeps 23.976 -> 59.86 and other fractional rates gap-free.
inline bool ComputeOutputSpan(int64_t sourceFrameIndex,
                              FrucRational sourceRate,
                              FrucRational targetRate,
                              OutputSpan& span) noexcept {
    span = {};
    if (sourceFrameIndex < 0 || sourceFrameIndex == (std::numeric_limits<int64_t>::max)() ||
        sourceRate.num <= 0 || sourceRate.den <= 0 ||
        targetRate.num <= 0 || targetRate.den <= 0) return false;

    const FrucRational sourceFrameTimeBase =
        FrucNormalizeRational(sourceRate.den, sourceRate.num);
    const FrucRational targetFrameTimeBase =
        FrucNormalizeRational(targetRate.den, targetRate.num);
    const int64_t first = FrucRescaleRound(sourceFrameIndex,
        sourceFrameTimeBase, targetFrameTimeBase);
    const int64_t end = FrucRescaleRound(sourceFrameIndex + 1,
        sourceFrameTimeBase, targetFrameTimeBase);
    if (first < 0 || end < first) return false;

    const int64_t count = end - first;
    if (count > static_cast<int64_t>((std::numeric_limits<uint32_t>::max)())) return false;
    span.firstPts = first;
    span.endPtsExclusive = end;
    span.repeatCount = static_cast<uint32_t>(count);
    return true;
}

enum class InterpolationDecision : uint8_t {
    KeepFallback,
    DropCoveredFrame,
    HandoffToInterpolation
};

// Continue publishing duplicated source images only until the first visible
// interpolated result proves that MVTools has completed its warm-up request.
inline bool ShouldQueueSourceFallback(bool fallbackActive, bool acceptingSourceFrames) noexcept {
    return fallbackActive && acceptingSourceFrames;
}

inline bool ShouldSealSourceFallback(bool fallbackActive, bool acceptingSourceFrames,
                                     bool explicitPreroll) noexcept {
    return fallbackActive && acceptingSourceFrames && !explicitPreroll;
}

// A fallback packet already covers all target PTS through lastFallbackPts.
// Bootstrap preroll must not end fallback mode before a visible source frame
// has been accepted by the delivery FIFO.
inline InterpolationDecision ClassifyInterpolation(
    bool fallbackActive,
    bool hasQueuedFallback,
    int64_t lastFallbackPts,
    int64_t interpolationPts,
    bool explicitPreroll) noexcept {
    if (!fallbackActive) return InterpolationDecision::HandoffToInterpolation;
    if (hasQueuedFallback && interpolationPts <= lastFallbackPts)
        return InterpolationDecision::DropCoveredFrame;
    if (explicitPreroll) return InterpolationDecision::DropCoveredFrame;
    return InterpolationDecision::HandoffToInterpolation;
}


// Treat any generated frame as hidden while the decoder has not yet supplied the
// first non-preroll source sample for this segment. The output PTS alone cannot
// establish visibility: hidden decoder preroll can already produce MVTools frames.
inline bool ShouldTreatInterpolationAsPreroll(bool explicitPreroll,
                                               bool visibleSourceBoundaryKnown,
                                               bool beforeVisibleBoundary) noexcept {
    return explicitPreroll || !visibleSourceBoundaryKnown || beforeVisibleBoundary;
}

// Map a source-frame index to the exact output-frame lattice used by MBlockFps.
// This is shared by the stream-time rebase and the visible-preroll boundary so
// both calculations use identical rational rounding.
inline bool TryComputeSourceFrameOutputPts(int64_t sourceFrameIndex,
                                           FrucRational sourceRate,
                                           FrucRational outputTimeBase,
                                           int64_t& outputPts) noexcept {
    outputPts = 0;
    if (sourceFrameIndex < 0 || sourceRate.num <= 0 || sourceRate.den <= 0 ||
        outputTimeBase.num <= 0 || outputTimeBase.den <= 0) return false;

    const FrucRational sourceTimeBase =
        FrucNormalizeRational(sourceRate.den, sourceRate.num);
    const int64_t mapped = FrucRescaleRound(sourceFrameIndex, sourceTimeBase, outputTimeBase);
    if (sourceFrameIndex > 0 &&
        (mapped == (std::numeric_limits<int64_t>::max)() ||
         mapped == (std::numeric_limits<int64_t>::min)())) return false;
    outputPts = mapped;
    return true;
}

// If the first timestamped decoder sample arrives after one or more untimed
// preroll frames, its timestamp belongs to sourceFrameIndex, not source frame 0.
// Rebase the stream origin backwards by that frame's output-lattice timestamp;
// otherwise MVTools and source fallback both publish the first visible frame in
// the future by exactly the accumulated preroll duration.
inline bool TryComputeRebasedStreamTimeOrigin100ns(
    int64_t sampleStart100ns, int64_t sourceFrameIndex, FrucRational sourceRate,
    FrucRational outputTimeBase, int64_t& streamOrigin100ns,
    int64_t& sourceOutputPts) noexcept {
    streamOrigin100ns = sampleStart100ns;
    sourceOutputPts = 0;
    if (!TryComputeSourceFrameOutputPts(sourceFrameIndex, sourceRate,
            outputTimeBase, sourceOutputPts)) return false;

    const int64_t frameOffset100ns = FrucRescaleRound(sourceOutputPts,
        outputTimeBase, FrucRational{1, 10000000});
    if (sourceOutputPts != 0 &&
        (frameOffset100ns == (std::numeric_limits<int64_t>::max)() ||
         frameOffset100ns == (std::numeric_limits<int64_t>::min)())) return false;

    // Source indices and a positive output time base make this offset
    // non-negative. Check subtraction using integer bounds so MSVC's
    // long-double == double implementation cannot lose 100ns ticks.
    if (frameOffset100ns < 0 ||
        sampleStart100ns < (std::numeric_limits<int64_t>::min)() + frameOffset100ns)
        return false;

    streamOrigin100ns = sampleStart100ns - frameOffset100ns;
    return true;
}

} // namespace DirectMotionSeekFallback
