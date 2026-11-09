#pragma once

#include <cstddef>
#include <cstdint>

// Pure diagnostic classifier. It observes counters and worker/queue snapshots only;
// it must never participate in queue predicates, scheduling decisions or frame flow.
namespace directmotion_diagnostics {

enum class PipelineStallKind : uint8_t {
    None = 0,
    ReceiveNormalization,
    SourceCachePublication,
    SourceCacheBackpressure,
    SourceFrameExpired,
    OutputQueueBackpressure,
    DownstreamDelivery,
    PrefetchSourceWait,
    AviSynthProcessing,
    ProcessingNotScheduled,
    UpstreamInputIdle
};

struct PipelineStageCounters {
    uint64_t received = 0;
    uint64_t normalized = 0;
    uint64_t published = 0;
    uint64_t generated = 0;
    uint64_t delivered = 0;
};

struct PipelineStallObservation {
    PipelineStageCounters counters;
    uint64_t receiveAgeMs = 0;
    uint64_t normalizeAgeMs = 0;
    uint64_t publishAgeMs = 0;
    uint64_t generateAgeMs = 0;
    uint64_t deliverAgeMs = 0;
    uint64_t anyProgressAgeMs = 0;
    size_t outputQueued = 0;
    size_t outputCapacity = 0;
    bool processingBusy = false;
    bool processingPending = false;
    bool processingPassCapped = false;
    bool deliveryBusy = false;
    bool sourceCacheProducerWaiting = false;
    bool sourceFrameExpired = false;
    int sourceWaiters = 0;
};

constexpr uint64_t kPipelineStallThresholdMs = 1500;

inline PipelineStallKind ClassifyPipelineStall(
    const PipelineStallObservation& state) noexcept {
    // A source frame which has already expired is a terminal correctness failure,
    // not a time-based stall. Report it immediately even if progress timestamps are fresh.
    if (state.sourceFrameExpired) return PipelineStallKind::SourceFrameExpired;
    if (state.counters.received == 0) return PipelineStallKind::None;

    // A producer explicitly waiting for source-cache capacity is more specific
    // evidence than the generic Receive counter gap: normalization already finished,
    // but publication cannot advance until the consumer retires historical frames.
    if (state.sourceCacheProducerWaiting &&
        state.counters.normalized > state.counters.published &&
        state.publishAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::SourceCacheBackpressure;
    }

    // Input stages are serial in Receive. A positive gap which remains unchanged
    // beyond the threshold identifies the exact in-flight Receive boundary.
    if (state.counters.received > state.counters.normalized &&
        state.normalizeAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::ReceiveNormalization;
    }
    if (state.counters.normalized > state.counters.published &&
        state.publishAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::SourceCachePublication;
    }

    // A full bounded FIFO plus a live processing producer is direct evidence of
    // backpressure at the output queue boundary; do not mislabel it as GetFrame.
    if (state.outputCapacity != 0 &&
        state.outputQueued >= state.outputCapacity &&
        state.processingBusy &&
        state.deliverAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::OutputQueueBackpressure;
    }

    // Once a generated frame is outstanding, stale delivery progress points to
    // the delivery worker/allocator/renderer path, whether the packet is queued
    // or currently in flight downstream.
    if (state.counters.generated > state.counters.delivered &&
        state.deliverAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::DownstreamDelivery;
    }

    // A processing worker that is busy and waiting on the live source bridge is
    // distinct from one stuck inside AviSynth/MVTools evaluation.
    if (state.processingBusy && state.sourceWaiters > 0 &&
        state.generateAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::PrefetchSourceWait;
    }
    if (state.processingBusy && state.counters.published > 0 &&
        state.generateAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::AviSynthProcessing;
    }

    // A pending notification or the explicit 128-frame pass-cap marker with no
    // active processing pass identifies a likely continuation/scheduling defect.
    // Cache frames alone are insufficient: Prefetch may legitimately wait for input.
    if ((state.processingPending || state.processingPassCapped) && !state.processingBusy &&
        state.generateAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::ProcessingNotScheduled;
    }

    if (state.anyProgressAgeMs >= kPipelineStallThresholdMs) {
        return PipelineStallKind::UpstreamInputIdle;
    }
    return PipelineStallKind::None;
}

inline const char* PipelineStallKindName(PipelineStallKind kind) noexcept {
    switch (kind) {
    case PipelineStallKind::ReceiveNormalization: return "RECEIVE_NORMALIZATION";
    case PipelineStallKind::SourceCachePublication: return "SOURCE_CACHE_PUBLICATION";
    case PipelineStallKind::SourceCacheBackpressure: return "SOURCE_CACHE_BACKPRESSURE";
    case PipelineStallKind::SourceFrameExpired: return "SOURCE_FRAME_EXPIRED";
    case PipelineStallKind::OutputQueueBackpressure: return "OUTPUT_FIFO_BACKPRESSURE";
    case PipelineStallKind::DownstreamDelivery: return "DOWNSTREAM_DELIVERY";
    case PipelineStallKind::PrefetchSourceWait: return "PREFETCH_SOURCE_WAIT";
    case PipelineStallKind::AviSynthProcessing: return "AVISYNTH_MVTOOLS_PROCESSING";
    case PipelineStallKind::ProcessingNotScheduled: return "PROCESSING_NOT_SCHEDULED";
    case PipelineStallKind::UpstreamInputIdle: return "UPSTREAM_INPUT_IDLE";
    default: return "NONE";
    }
}

} // namespace directmotion_diagnostics
