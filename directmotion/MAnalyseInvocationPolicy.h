#pragma once

#include <cstddef>

// Shared, SDK-independent description of the only MAnalyse signatures that
// DirectMotion permits: temporal=false makes analysis safely index-addressable
// after a seek-frontier jump; mt=false leaves AviSynth+ Prefetch as the sole
// scheduler for frame-level graph evaluation.
namespace DirectMotionAviSynth {

inline constexpr bool kMAnalyseInternalThreadingEnabled = false;
inline constexpr bool kMAnalyseTemporalPredictorsEnabled = false;

struct MAnalyseInvocationContract {
    const char** argumentNames;
    std::size_t argumentCount;
};

inline const char* kMAnalyseStandardMtFalseNames[] = {
    nullptr, "chroma", "isb", "blksize", "blksizev",
    "searchparam", "plevel", "search", "badrange", "mt", "temporal"
};

inline const char* kMAnalyseAdvancedMtFalseNames[] = {
    nullptr, "chroma", "isb", "blksize", "blksizev", "searchparam",
    "plevel", "search", "badrange", "overlap", "truemotion", "mt", "temporal"
};

inline const MAnalyseInvocationContract kMAnalyseStandardMtFalseContract = {
    kMAnalyseStandardMtFalseNames,
    sizeof(kMAnalyseStandardMtFalseNames) / sizeof(kMAnalyseStandardMtFalseNames[0])
};

inline const MAnalyseInvocationContract kMAnalyseAdvancedMtFalseContract = {
    kMAnalyseAdvancedMtFalseNames,
    sizeof(kMAnalyseAdvancedMtFalseNames) / sizeof(kMAnalyseAdvancedMtFalseNames[0])
};

inline const MAnalyseInvocationContract& GetMAnalyseInvocationContract(
    bool advanced) noexcept {
    return advanced
        ? kMAnalyseAdvancedMtFalseContract
        : kMAnalyseStandardMtFalseContract;
}

} // namespace DirectMotionAviSynth
