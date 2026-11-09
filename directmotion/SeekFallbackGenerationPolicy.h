#pragma once

#include <cstdint>

namespace DirectMotionSeekFallback {

// The fallback state is protected by m_seekFallbackMutex, but mutual exclusion
// alone is not enough: an old ProcessingWorker pass may acquire that mutex after
// a newer seek has armed fallback. Only the owner generation may mutate it.
inline bool OwnsFallbackGeneration(uint64_t ownerGeneration,
                                  uint64_t requestGeneration) noexcept {
    return ownerGeneration != 0 && requestGeneration != 0 &&
        ownerGeneration == requestGeneration;
}

inline bool CanArmFallbackGeneration(uint64_t requestedGeneration,
                                     uint64_t currentGeneration) noexcept {
    return requestedGeneration != 0 &&
        requestedGeneration == currentGeneration;
}

} // namespace DirectMotionSeekFallback
