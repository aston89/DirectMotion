#include "../directmotion/PipelineStallDiagnostics.h"
#include <iostream>
#include <string>
#include <vector>

using directmotion_diagnostics::ClassifyPipelineStall;
using directmotion_diagnostics::PipelineStallKind;
using directmotion_diagnostics::PipelineStallObservation;
using directmotion_diagnostics::kPipelineStallThresholdMs;

int main() {
    struct TestCase { const char* name; PipelineStallObservation state; PipelineStallKind expected; };
    std::vector<TestCase> tests;

    PipelineStallObservation s{};
    tests.push_back({"no input is not a stall", s, PipelineStallKind::None});

    s = {};
    s.counters.received = 10; s.counters.normalized = 9;
    s.normalizeAgeMs = kPipelineStallThresholdMs;
    tests.push_back({"Receive normalization stall", s, PipelineStallKind::ReceiveNormalization});

    s = {};
    s.counters.received = 10; s.counters.normalized = 10; s.counters.published = 9;
    s.publishAgeMs = kPipelineStallThresholdMs;
    tests.push_back({"source cache publication stall", s, PipelineStallKind::SourceCachePublication});

    s = {};
    s.counters.received = 20; s.counters.normalized = 20; s.counters.published = 20;
    s.counters.generated = 18; s.counters.delivered = 2;
    s.outputQueued = 16; s.outputCapacity = 16; s.processingBusy = true;
    s.deliverAgeMs = kPipelineStallThresholdMs;
    tests.push_back({"full FIFO wins over generic delivery diagnosis", s, PipelineStallKind::OutputQueueBackpressure});

    s = {};
    s.counters.received = 20; s.outputQueued = 16; s.outputCapacity = 16;
    s.processingBusy = true; s.deliverAgeMs = 250;
    tests.push_back({"brief FIFO saturation is not called a stall", s, PipelineStallKind::None});

    s = {};
    s.sourceFrameExpired = true;
    tests.push_back({"expired source frame is an immediate terminal diagnosis", s, PipelineStallKind::SourceFrameExpired});

    s = {};
    s.counters.received = 10; s.counters.normalized = 10; s.counters.published = 9;
    s.publishAgeMs = kPipelineStallThresholdMs; s.sourceCacheProducerWaiting = true;
    tests.push_back({"bounded source cache producer backpressure is distinguished from a generic publish stall", s, PipelineStallKind::SourceCacheBackpressure});

    s = {};
    s.counters.received = 10; s.counters.normalized = 9; s.counters.published = 8;
    s.normalizeAgeMs = kPipelineStallThresholdMs; s.publishAgeMs = kPipelineStallThresholdMs;
    s.sourceCacheProducerWaiting = true;
    tests.push_back({"explicit source-cache producer wait outranks the generic Receive counter gap", s, PipelineStallKind::SourceCacheBackpressure});

    s = {};
    s.counters.received = 20; s.counters.generated = 12; s.counters.delivered = 11;
    s.deliverAgeMs = kPipelineStallThresholdMs; s.deliveryBusy = true;
    tests.push_back({"downstream delivery stall", s, PipelineStallKind::DownstreamDelivery});

    s = {};
    s.counters.received = 20; s.counters.published = 18;
    s.generateAgeMs = kPipelineStallThresholdMs; s.processingBusy = true; s.sourceWaiters = 3;
    tests.push_back({"Prefetch waiting for source", s, PipelineStallKind::PrefetchSourceWait});

    s = {};
    s.counters.received = 20; s.counters.published = 18;
    s.generateAgeMs = kPipelineStallThresholdMs; s.processingBusy = true;
    tests.push_back({"AviSynth/MVTools processing stall", s, PipelineStallKind::AviSynthProcessing});

    s = {};
    s.counters.received = 20; s.counters.published = 18;
    s.generateAgeMs = kPipelineStallThresholdMs; s.processingPending = true;
    tests.push_back({"pending work without active processing", s, PipelineStallKind::ProcessingNotScheduled});

    s = {};
    s.counters.received = 20; s.counters.published = 18;
    s.generateAgeMs = kPipelineStallThresholdMs; s.processingPassCapped = true;
    tests.push_back({"128-frame pass cap without a follow-up pass", s, PipelineStallKind::ProcessingNotScheduled});

    s = {};
    s.counters.received = 20; s.anyProgressAgeMs = kPipelineStallThresholdMs;
    tests.push_back({"no progress classified as upstream idle", s, PipelineStallKind::UpstreamInputIdle});

    s = {};
    s.counters.received = 20; s.anyProgressAgeMs = 100;
    tests.push_back({"recent progress is not a stall", s, PipelineStallKind::None});

    unsigned failed = 0;
    for (const auto& test : tests) {
        const auto actual = ClassifyPipelineStall(test.state);
        const bool ok = actual == test.expected;
        std::cout << (ok ? "PASS" : "FAIL") << " | " << test.name
                  << " | actual=" << directmotion_diagnostics::PipelineStallKindName(actual)
                  << " | expected=" << directmotion_diagnostics::PipelineStallKindName(test.expected)
                  << '\n';
        if (!ok) ++failed;
    }
    std::cout << "\nRESULT: " << (tests.size() - failed) << '/' << tests.size()
              << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
