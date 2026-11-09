#include "../directmotion/BootstrapEvaluationPolicy.h"
#include "../directmotion/SeekFrontierPolicy.h"
#include "../directmotion/SourceCacheRetention.h"

#include <climits>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) std::cout << "PASS | " << description << '\n';
    else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}

struct EvaluationCounts {
    unsigned direct = 0;
    unsigned prefetch = 0;
};

void EvaluateOutput(int64_t index, int64_t startIndex, EvaluationCounts& counts) {
    if (DirectMotionBootstrapEvaluation::UsesPrefetchForOutput(index, startIndex)) ++counts.prefetch;
    else ++counts.direct;
}
}

int main() {
    using DirectMotionBootstrapEvaluation::RequiredSourceFramesForFirstOutput;
    using DirectMotionBootstrapEvaluation::UsesPrefetchForOutput;
    using DirectMotionBootstrapEvaluation::PrefetchRelativeIndex;
    using DirectMotionSeekFrontier::TryFirstUncoveredOutputIndex;
    using DirectMotionSeekFrontier::FallbackCoversFinalizedOutput;
    using DirectMotionSeekFrontier::TrySourceFrameAnchorForOutputIndex;
    using DirectMotionSeekFrontier::HasSourceLookaheadForOutputIndex;

    Check(!UsesPrefetchForOutput(0, 1),
        "cold-start output zero uses raw MBlockFps directly");
    Check(UsesPrefetchForOutput(1, 1),
        "cold-start output one maps to Prefetch-relative frame zero");
    Check(!UsesPrefetchForOutput(60, 61) && UsesPrefetchForOutput(61, 61),
        "seek-frontier output is direct bootstrap and its successor starts Prefetch");
    Check(PrefetchRelativeIndex(61, 61) == 0 && PrefetchRelativeIndex(62, 61) == 1,
        "Trimmed Prefetch frame zero maps to the first output after the direct frontier");
    Check(PrefetchRelativeIndex(60, 61) == -1,
        "an output before the first Prefetch frame is rejected as an invalid relative index");

    int64_t first = -99;
    Check(TryFirstUncoveredOutputIndex(false, -1, first) && first == 0,
        "no fallback watermark preserves the normal output-zero bootstrap");
    Check(TryFirstUncoveredOutputIndex(true, 59, first) && first == 60,
        "an inclusive fallback watermark of 59 selects output 60");
    Check(TryFirstUncoveredOutputIndex(true, 600, first) && first == 601,
        "a long fallback prefix selects only its first uncovered output");
    Check(!TryFirstUncoveredOutputIndex(true, -1, first),
        "a malformed negative fallback watermark is rejected");
    Check(!TryFirstUncoveredOutputIndex(true, (std::numeric_limits<int64_t>::max)(), first),
        "fallback watermark increment saturates safely instead of overflowing");
    Check(FallbackCoversFinalizedOutput(600, 600),
        "terminal EOS skips MVTools when the fallback watermark covers the full finalized output range");
    Check(!FallbackCoversFinalizedOutput(599, 600),
        "terminal EOS still requires MVTools when one finalized output slot remains uncovered");
    Check(!FallbackCoversFinalizedOutput(-1, 600) && !FallbackCoversFinalizedOutput(0, -1),
        "terminal fallback coverage rejects malformed frontier and output counts");

    int64_t sourceAnchor = -1;
    const FrucRational source{24000, 1001};
    const FrucRational target{2993, 50};
    Check(TrySourceFrameAnchorForOutputIndex(0, source, target, sourceAnchor) && sourceAnchor == 0,
        "cold-start frontier primes the source retention anchor at source frame zero");
    Check(TrySourceFrameAnchorForOutputIndex(60, source, target, sourceAnchor) && sourceAnchor == 24,
        "23.976-to-59.86 frontier 60 maps to source frame 24");
    Check(TrySourceFrameAnchorForOutputIndex(600, source, target, sourceAnchor) && sourceAnchor == 240,
        "long seek frontier 600 maps to source frame 240 without evaluating its prefix");
    Check(!TrySourceFrameAnchorForOutputIndex(-1, source, target, sourceAnchor),
        "negative output index cannot prime source-cache retention");
    Check(!TrySourceFrameAnchorForOutputIndex(60, FrucRational{0, 1}, target, sourceAnchor),
        "invalid source rate cannot prime source-cache retention");

    Check(RequiredSourceFramesForFirstOutput(2) == 3,
        "cold bootstrap still requires only current source frame plus two-frame MVTools lookahead");
    Check(RequiredSourceFramesForFirstOutput(0) == 1,
        "zero lookahead requires one source frame");
    Check(RequiredSourceFramesForFirstOutput(5) == 6,
        "first-frame source requirement follows backend lookahead");
    Check(RequiredSourceFramesForFirstOutput(-5) == 1,
        "negative lookahead is clamped to a one-frame minimum");
    Check(RequiredSourceFramesForFirstOutput(INT_MAX) == INT_MAX,
        "extreme lookahead saturates without integer overflow");

    Check(!HasSourceLookaheadForOutputIndex(60, 27, 2, source, target),
        "23.976-to-59.86 frontier waits while the next output lacks its two-frame lookahead");
    Check(HasSourceLookaheadForOutputIndex(60, 28, 2, source, target),
        "23.976-to-59.86 frontier becomes eligible as soon as its required source lookahead is published");
    Check(HasSourceLookaheadForOutputIndex(0, 3, 2, source, target),
        "output zero retains the original three-source-frame bootstrap threshold");
    Check(!HasSourceLookaheadForOutputIndex(63, 28, 2, source, target),
        "output 63 remains outside the available two-frame source lookahead");
    Check(!HasSourceLookaheadForOutputIndex(601, 243, 2, source, target),
        "long seek frontier 601 waits while source frame 241 is not yet safely available");
    Check(HasSourceLookaheadForOutputIndex(601, 244, 2, source, target),
        "long seek frontier 601 becomes ready without evaluating the covered output prefix");

    const int64_t retainedFloor = DirectMotionSourceCache::RetentionFloor(
        0, -1, 240, 32);
    Check(retainedFloor == 208,
        "seek frontier retention anchor preserves the bounded 32-frame history before source frame 240");
    size_t retainedFrames = 0;
    int64_t cacheBase = 0;
    bool cacheCapacityMaintained = true;
    for (int64_t frame = 0; frame < 244; ++frame) {
        const int64_t floor = DirectMotionSourceCache::RetentionFloor(
            cacheBase, -1, 240, 32);
        while (retainedFrames != 0 && cacheBase < floor) {
            --retainedFrames;
            ++cacheBase;
        }
        if (!DirectMotionSourceCache::CanAppend(retainedFrames, 128)) {
            cacheCapacityMaintained = false;
            break;
        }
        ++retainedFrames;
    }
    Check(cacheCapacityMaintained && cacheBase == 208 && retainedFrames == 36,
        "a 240-source-frame frontier stays within a 128-frame cache while retaining source 208..243");

    // The actual route remains one direct bootstrap followed by Prefetch-relative
    // indices, irrespective of how far the segment-local output frontier advanced.
    EvaluationCounts counts;
    constexpr int64_t startIndex = 601;
    EvaluateOutput(startIndex, startIndex + 1, counts);
    for (int64_t index = startIndex + 1; index < startIndex + 129; ++index)
        EvaluateOutput(index, startIndex + 1, counts);
    Check(counts.direct == 1 && counts.prefetch == 128,
        "a 129-frame post-seek sequence evaluates one frontier frame directly then 128 Prefetch-relative frames");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
