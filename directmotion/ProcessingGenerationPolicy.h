#pragma once

#include <cstdint>

// A ProcessingWorker pass is valid only for the exact engine generation that
// produced its graph. The FIFO generation alone is insufficient: Pause can
// retire the output timeline before Run has quiesced and rebuilt the old engine.
namespace DirectMotionProcessingGeneration {

inline bool MatchesEngineGeneration(
    std::uint64_t processingGeneration,
    std::uint64_t engineGeneration) noexcept {
    return processingGeneration != 0 && engineGeneration != 0 &&
        processingGeneration == engineGeneration;
}

} // namespace DirectMotionProcessingGeneration