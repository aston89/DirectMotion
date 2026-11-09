#pragma once

// Historical staged-Prefetch bootstrap math retained for regression documentation.
// Production startup now uses BootstrapEvaluationPolicy.h and direct raw MBlockFps output zero.

#include <algorithm>
#include <cmath>
#include <climits>

// Resume warm-start policy. A cold graph keeps the conservative staged bootstrap
// used by the original startup path. After Running -> Paused -> Run, the processing
// worker may enter Prefetch as soon as MVTools' actual future-source lookahead exists;
// Prefetch then waits asynchronously while Receive continues publishing into InputClip.
namespace DirectMotionResumeWarmStart {

inline int ColdBootstrapInputFramesRequired(int prefetchOutputFrames,
                                            long double outputPerSource,
                                            int motionLookaheadFrames) noexcept {
    const int safeLookahead = (std::max)(0, motionLookaheadFrames);
    const int minimum = safeLookahead >= INT_MAX ? INT_MAX : safeLookahead + 1;
    if (prefetchOutputFrames < 0 || !(outputPerSource > 0.0L) ||
        !std::isfinite(static_cast<double>(outputPerSource))) return minimum;

    const long double sourceLead = std::ceil(
        (static_cast<long double>(prefetchOutputFrames) + 1.0L) / outputPerSource);
    if (!std::isfinite(static_cast<double>(sourceLead)) ||
        sourceLead > static_cast<long double>(INT_MAX - safeLookahead))
        return INT_MAX;
    const int required = static_cast<int>(sourceLead) + safeLookahead;
    return (std::max)(minimum, required);
}

inline int ResumeBootstrapInputFramesRequired(int motionLookaheadFrames) noexcept {
    const int safeLookahead = (std::max)(0, motionLookaheadFrames);
    return safeLookahead >= INT_MAX ? INT_MAX : safeLookahead + 1;
}

inline int BootstrapInputFramesRequired(bool resumeWarmStart,
                                        int prefetchOutputFrames,
                                        long double outputPerSource,
                                        int motionLookaheadFrames) noexcept {
    return resumeWarmStart
        ? ResumeBootstrapInputFramesRequired(motionLookaheadFrames)
        : ColdBootstrapInputFramesRequired(prefetchOutputFrames, outputPerSource,
                                           motionLookaheadFrames);
}

} // namespace DirectMotionResumeWarmStart
