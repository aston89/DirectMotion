#include "../directmotion/SeekFallbackPolicy.h"

#include <cstdint>
#include <iostream>
#include <string>

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

DirectMotionSeekFallback::OutputSpan Span(int64_t sourceIndex,
                                           FrucRational source,
                                           FrucRational target) {
    DirectMotionSeekFallback::OutputSpan span{};
    (void)DirectMotionSeekFallback::ComputeOutputSpan(sourceIndex, source, target, span);
    return span;
}
}

int main() {
    using DirectMotionSeekFallback::ClassifyInterpolation;
    using DirectMotionSeekFallback::ComputeOutputSpan;
    using DirectMotionSeekFallback::InterpolationDecision;

    const FrucRational source23976{24000, 1001};
    const FrucRational target5986{2993, 50};
    const auto s0 = Span(0, source23976, target5986);
    const auto s1 = Span(1, source23976, target5986);
    const auto s2 = Span(2, source23976, target5986);
    const auto s3 = Span(3, source23976, target5986);

    Check(s0.firstPts == 0 && s0.endPtsExclusive == 2 && s0.repeatCount == 2,
        "23.976-to-59.86 source frame zero covers output PTS 0..1");
    Check(s1.firstPts == 2 && s1.endPtsExclusive == 5 && s1.repeatCount == 3,
        "fractional cadence assigns three output slots to source frame one");
    Check(s2.firstPts == 5 && s2.endPtsExclusive == 7 && s2.repeatCount == 2,
        "fractional cadence continues without a gap after source frame one");
    Check(s3.firstPts == s2.endPtsExclusive && s3.repeatCount >= 2 && s3.repeatCount <= 3,
        "successive fractional source intervals remain adjacent");

    const FrucRational source30{30, 1};
    const FrucRational target60{60, 1};
    const auto s30 = Span(7, source30, target60);
    Check(s30.firstPts == 14 && s30.endPtsExclusive == 16 && s30.repeatCount == 2,
        "integer 30-to-60 cadence repeats each source frame exactly twice");

    const FrucRational source60{60, 1};
    const FrucRational target24{24, 1};
    const auto downsampled0 = Span(0, source60, target24);
    const auto downsampled1 = Span(1, source60, target24);
    Check(downsampled0.repeatCount == 0 && downsampled1.firstPts == 0 &&
          downsampled1.endPtsExclusive == 1 && downsampled1.repeatCount == 1,
        "down-rate source intervals may contribute zero or one target slot without negative counts");
    const auto downsampled2 = Span(2, source60, target24);
    Check(downsampled2.firstPts == 1 && downsampled2.endPtsExclusive == 1 && downsampled2.repeatCount == 0,
        "rounding preserves a zero-slot interval at fractional down-rate cadence");

    DirectMotionSeekFallback::OutputSpan invalid{};
    Check(!ComputeOutputSpan(-1, source23976, target5986, invalid),
        "negative source frame indices are rejected");
    Check(!ComputeOutputSpan(0, FrucRational{0, 1}, target5986, invalid),
        "invalid source rates are rejected");
    Check(!ComputeOutputSpan(0, source23976, FrucRational{0, 1}, invalid),
        "invalid target rates are rejected");

    Check(ClassifyInterpolation(true, true, 12, 12, false) ==
          InterpolationDecision::DropCoveredFrame,
        "interpolated PTS equal to the fallback watermark is suppressed");
    Check(ClassifyInterpolation(true, true, 12, 11, false) ==
          InterpolationDecision::DropCoveredFrame,
        "interpolated PTS behind the fallback watermark is suppressed");
    Check(ClassifyInterpolation(true, true, 12, 13, false) ==
          InterpolationDecision::HandoffToInterpolation,
        "handoff begins strictly after the last fallback PTS");
    Check(ClassifyInterpolation(true, false, -1, 0, true) ==
          InterpolationDecision::DropCoveredFrame,
        "bootstrap preroll cannot prematurely end fallback mode");
    Check(ClassifyInterpolation(true, false, -1, 1, false) ==
          InterpolationDecision::HandoffToInterpolation,
        "a visible interpolated frame can take over when fallback has no queued packet");
    Check(ClassifyInterpolation(false, true, 12, 4, false) ==
          InterpolationDecision::HandoffToInterpolation,
        "normal interpolation is unaffected after fallback mode is inactive");
    Check(DirectMotionSeekFallback::ShouldQueueSourceFallback(true, true),
        "source fallback is admitted while the seek warm-up is active");
    Check(!DirectMotionSeekFallback::ShouldQueueSourceFallback(false, true),
        "inactive published fallback state makes Receive-side source fallback admission a no-op");
    Check(!DirectMotionSeekFallback::ShouldQueueSourceFallback(true, false),
        "source fallback stops admitting new duplicates after the first visible interpolated result");
    Check(!DirectMotionSeekFallback::ShouldSealSourceFallback(true, true, true),
        "hidden bootstrap preroll does not seal source fallback");
    Check(DirectMotionSeekFallback::ShouldSealSourceFallback(true, true, false),
        "first visible interpolated result seals source fallback admission");

    Check(DirectMotionSeekFallback::ShouldTreatInterpolationAsPreroll(false, false, false),
        "MVTools output remains hidden until the first non-preroll source boundary is known");
    Check(DirectMotionSeekFallback::ShouldTreatInterpolationAsPreroll(true, true, false),
        "explicit bootstrap preroll remains hidden after the source boundary is known");
    Check(DirectMotionSeekFallback::ShouldTreatInterpolationAsPreroll(false, true, true),
        "MVTools output before the established visible boundary remains hidden");
    Check(!DirectMotionSeekFallback::ShouldTreatInterpolationAsPreroll(false, true, false),
        "MVTools output at or after the established visible boundary may complete handoff");

    const FrucRational outputTimeBase5986{50, 2993};
    int64_t mappedOutputPts = -1;
    Check(DirectMotionSeekFallback::TryComputeSourceFrameOutputPts(
            48, source23976, outputTimeBase5986, mappedOutputPts) && mappedOutputPts == 120,
        "23.976 source index maps to the same rounded 59.86-FPS output lattice as fallback");

    int64_t rebasedOrigin = 0;
    int64_t rebasedOutputPts = -1;
    const int64_t timedSampleStart = 50000000;
    const bool rebased = DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
        timedSampleStart, 48, source23976, outputTimeBase5986,
        rebasedOrigin, rebasedOutputPts);
    const int64_t rebasedFrameOffset = FrucRescaleRound(rebasedOutputPts,
        outputTimeBase5986, FrucRational{1, 10000000});
    Check(rebased && rebasedOutputPts == 120 &&
          rebasedOrigin + rebasedFrameOffset == timedSampleStart,
        "first timestamped frame after untimed preroll is aligned to its actual output PTS, not frame zero");

    Check(DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
            timedSampleStart, 0, source23976, outputTimeBase5986,
            rebasedOrigin, rebasedOutputPts) &&
          rebasedOrigin == timedSampleStart && rebasedOutputPts == 0,
        "source frame zero preserves the original first-timestamp stream origin");
    Check(!DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
            timedSampleStart, -1, source23976, outputTimeBase5986,
            rebasedOrigin, rebasedOutputPts),
        "stream-origin rebase rejects a negative source index");
    Check(!DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
            timedSampleStart, 48, FrucRational{0, 1}, outputTimeBase5986,
            rebasedOrigin, rebasedOutputPts),
        "stream-origin rebase rejects an invalid source rate");
    Check(!DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
            timedSampleStart, 48, source23976, FrucRational{0, 1},
            rebasedOrigin, rebasedOutputPts),
        "stream-origin rebase rejects an invalid output time base");
    Check(!DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
            (std::numeric_limits<int64_t>::min)(), 48, source23976,
            outputTimeBase5986, rebasedOrigin, rebasedOutputPts),
        "stream-origin rebase rejects signed underflow instead of wrapping timestamps");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
