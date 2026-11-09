#include "../directmotion/ResumeWarmStart.h"
#include "../directmotion/SourceCacheRetention.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* name) {
    ++checks;
    if (condition) std::cout << "PASS: " << name << '\n';
    else {
        ++failures;
        std::cout << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using DirectMotionResumeWarmStart::BootstrapInputFramesRequired;
    using DirectMotionResumeWarmStart::ColdBootstrapInputFramesRequired;
    using DirectMotionResumeWarmStart::ResumeBootstrapInputFramesRequired;

    constexpr int motionLookahead = 2;
    constexpr int maximumPrefetchWindow = 64;

    const int cold2x = ColdBootstrapInputFramesRequired(maximumPrefetchWindow, 2.0L, motionLookahead);
    const int resume2x = ResumeBootstrapInputFramesRequired(motionLookahead);
    Check(cold2x == 35,
        "cold-start bootstrap retains the conservative 64-output Prefetch lead at 2x (35 source frames)");
    Check(resume2x == 3,
        "resume bootstrap starts as soon as the two-frame MVTools future lookahead is available");
    Check(cold2x - resume2x == 32,
        "64-output Prefetch at 2x no longer imposes 32 avoidable source frames of resume latency");

    Check(ColdBootstrapInputFramesRequired(64, 2.5L, motionLookahead) == 28,
        "cold-start staging uses rational 2.5x source/output conversion");
    Check(ColdBootstrapInputFramesRequired(64, 4.0L, motionLookahead) == 19,
        "cold-start staging uses rational 4x source/output conversion");
    Check(ColdBootstrapInputFramesRequired(8, 4.0L, motionLookahead) == 5,
        "smaller Prefetch windows retain their matching conservative cold-start lead");

    Check(BootstrapInputFramesRequired(true, 64, 2.0L, motionLookahead) == 3,
        "resume bootstrap is independent of a 64-frame Prefetch window");
    Check(BootstrapInputFramesRequired(true, 8, 4.0L, motionLookahead) == 3,
        "resume bootstrap is independent of a small Prefetch window and output ratio");
    Check(BootstrapInputFramesRequired(false, 64, 2.0L, motionLookahead) == cold2x,
        "initial cold startup continues using the unchanged staged bootstrap policy");
    Check(BootstrapInputFramesRequired(false, 64, 2.5L, motionLookahead) == 28,
        "non-resume initialization never accidentally selects the warm-start threshold");

    Check(ResumeBootstrapInputFramesRequired(0) == 1,
        "zero motion lookahead still requires one source frame");
    Check(ResumeBootstrapInputFramesRequired(5) == 6,
        "resume threshold scales with a larger backend motion lookahead");
    Check(ColdBootstrapInputFramesRequired(64, 0.0L, motionLookahead) == 3,
        "invalid output ratio falls back to the minimum safe source lookahead");
    Check(ColdBootstrapInputFramesRequired(64,
        std::numeric_limits<long double>::infinity(), motionLookahead) == 3,
        "non-finite output ratio cannot overflow the cold-start policy");
    Check(ColdBootstrapInputFramesRequired((std::numeric_limits<int>::max)(), 0.000001L,
        motionLookahead) == (std::numeric_limits<int>::max)(),
        "extreme cold-start source lead saturates instead of overflowing an int");
    Check(ResumeBootstrapInputFramesRequired((std::numeric_limits<int>::max)()) ==
        (std::numeric_limits<int>::max)(),
        "extreme MVTools lookahead saturates instead of overflowing the resume threshold");
    Check(ColdBootstrapInputFramesRequired(64, 2.0L,
        (std::numeric_limits<int>::max)()) == (std::numeric_limits<int>::max)(),
        "extreme motion lookahead saturates the cold-start minimum safely");

    const size_t retainedHistory = DirectMotionSourceCache::HistorySlackFrames(26, 2, 4, 24);
    Check(retainedHistory == 32,
        "source cache keeps the established bounded Prefetch plus MVTools historical slack");
    Check(retainedHistory >= 26 + motionLookahead + 4,
        "bounded source-cache history still covers the complete source lookahead and safety margin");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
