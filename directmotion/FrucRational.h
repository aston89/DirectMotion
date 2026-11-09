#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

// Small rational/time-base helpers used by the AviSynth-only implementation.
// This deliberately avoids an FFmpeg runtime or FFmpeg development dependency.
struct FrucRational {
    int64_t num = 0;
    int64_t den = 1;
};

inline FrucRational FrucNormalizeRational(int64_t num, int64_t den) noexcept {
    if (den == 0) return FrucRational{0, 1};
    if (den < 0) { num = -num; den = -den; }
    if (num == 0) return FrucRational{0, 1};
    const int64_t divisor = std::gcd(num < 0 ? -num : num, den);
    if (divisor > 1) { num /= divisor; den /= divisor; }
    return FrucRational{num, den};
}

inline FrucRational FrucRationalFromDouble(double value, int64_t maxDenominator = 100000) noexcept {
    if (!std::isfinite(value) || value <= 0.0 || maxDenominator <= 0)
        return FrucRational{24, 1};
    const long double scaled = static_cast<long double>(value) * maxDenominator;
    if (scaled > static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return FrucRational{24, 1};
    return FrucNormalizeRational(static_cast<int64_t>(std::llround(scaled)), maxDenominator);
}

inline int64_t FrucRescaleRound(int64_t value, FrucRational fromTimeBase,
                                FrucRational toTimeBase) noexcept {
    if (fromTimeBase.den <= 0 || toTimeBase.num <= 0 || toTimeBase.den <= 0)
        return 0;
    const long double scaled = static_cast<long double>(value) * fromTimeBase.num *
        toTimeBase.den / (static_cast<long double>(fromTimeBase.den) * toTimeBase.num);
    if (scaled >= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return (std::numeric_limits<int64_t>::max)();
    if (scaled <= static_cast<long double>((std::numeric_limits<int64_t>::min)()))
        return (std::numeric_limits<int64_t>::min)();
    return static_cast<int64_t>(std::llround(scaled));
}

inline int64_t FrucRescaleFloor(int64_t value, FrucRational fromTimeBase,
                                FrucRational toTimeBase) noexcept {
    if (fromTimeBase.den <= 0 || toTimeBase.num <= 0 || toTimeBase.den <= 0)
        return 0;
    const long double scaled = static_cast<long double>(value) * fromTimeBase.num *
        toTimeBase.den / (static_cast<long double>(fromTimeBase.den) * toTimeBase.num);
    if (scaled >= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return (std::numeric_limits<int64_t>::max)();
    if (scaled <= static_cast<long double>((std::numeric_limits<int64_t>::min)()))
        return (std::numeric_limits<int64_t>::min)();
    return static_cast<int64_t>(std::floor(scaled));
}

inline int64_t FrucFrameDuration100ns(FrucRational fps) noexcept {
    if (fps.num <= 0 || fps.den <= 0) return 0;
    const long double duration = 10000000.0L * fps.den / fps.num;
    if (duration >= static_cast<long double>((std::numeric_limits<int64_t>::max)()))
        return (std::numeric_limits<int64_t>::max)();
    return static_cast<int64_t>(std::llround(duration));
}
