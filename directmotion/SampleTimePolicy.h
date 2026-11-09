#pragma once

namespace DirectMotionSampleTime {

struct Availability {
    bool hasStart = false;
    bool hasStop = false;
};

// IMediaSample::GetTime has two meaningful success results: S_OK means both
// timestamps are valid, while VFW_S_NO_STOP_TIME means only the start is valid.
// Keep this policy independent of Windows headers so its HRESULT classification
// can be covered by the portable C++17 regression suite.
template <typename HResult>
inline Availability ClassifyGetTimeResult(HResult result,
                                          HResult successCode,
                                          HResult noStopTimeCode) noexcept {
    return Availability{
        result == successCode || result == noStopTimeCode,
        result == successCode
    };
}

template <typename TimeValue>
inline TimeValue PreviousSampleContinuityAnchor(bool hasStopTime,
                                                TimeValue sampleStart,
                                                TimeValue sampleStop) noexcept {
    // Without a stop timestamp, compare the next sample against this sample's
    // start rather than the synthetic start+1 value returned by GetTime().
    return hasStopTime ? sampleStop : sampleStart;
}

} // namespace DirectMotionSampleTime
