#include "../directmotion/SampleTimePolicy.h"

#include <cstdint>
#include <iostream>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS | " << description << '\n';
    } else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}
}

int main() {
    // Values from DirectShow's Vfwmsgs.h. A portable policy test avoids pulling
    // Windows SDK headers into the C++17 regression suite.
    constexpr int32_t kSOk = 0;
    constexpr int32_t kVfwSNoStopTime = 0x00040270;
    constexpr int32_t kVfwESampleTimeNotSet = static_cast<int32_t>(0x80040249u);
    constexpr int32_t kGenericFailure = static_cast<int32_t>(0x80004005u);

    using DirectMotionSampleTime::ClassifyGetTimeResult;

    const auto complete = ClassifyGetTimeResult(kSOk, kSOk, kVfwSNoStopTime);
    Check(complete.hasStart, "S_OK exposes a valid sample start timestamp");
    Check(complete.hasStop, "S_OK exposes a valid sample stop timestamp");

    const auto startOnly = ClassifyGetTimeResult(kVfwSNoStopTime, kSOk, kVfwSNoStopTime);
    Check(startOnly.hasStart, "VFW_S_NO_STOP_TIME preserves the valid sample start timestamp");
    Check(!startOnly.hasStop, "VFW_S_NO_STOP_TIME does not advertise a valid sample stop timestamp");

    const auto untimed = ClassifyGetTimeResult(kVfwESampleTimeNotSet, kSOk, kVfwSNoStopTime);
    Check(!untimed.hasStart, "VFW_E_SAMPLE_TIME_NOT_SET does not fabricate a sample start timestamp");
    Check(!untimed.hasStop, "VFW_E_SAMPLE_TIME_NOT_SET does not fabricate a sample stop timestamp");

    const auto failed = ClassifyGetTimeResult(kGenericFailure, kSOk, kVfwSNoStopTime);
    Check(!failed.hasStart && !failed.hasStop,
        "unrelated failing HRESULTs are not treated as timestamp availability");

    const auto otherSuccess = ClassifyGetTimeResult<int32_t>(1, kSOk, kVfwSNoStopTime);
    Check(!otherSuccess.hasStart && !otherSuccess.hasStop,
        "unrelated success HRESULTs are not mistaken for GetTime timestamp results");

    Check(DirectMotionSampleTime::PreviousSampleContinuityAnchor(
              true, int64_t{1000}, int64_t{417667}) == 417667,
        "timestamp continuity uses the real stop when one is available");
    Check(DirectMotionSampleTime::PreviousSampleContinuityAnchor(
              false, int64_t{1000}, int64_t{1001}) == 1000,
        "timestamp continuity ignores GetTime's synthetic start-plus-one stop sentinel");

    std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << checks << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
