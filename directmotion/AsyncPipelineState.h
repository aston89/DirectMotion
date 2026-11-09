#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AsyncSourceFrame {
    uint64_t generation = 0;
    int64_t sequence = 0;
    int64_t sourcePts = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    int64_t streamStart100ns = 0;
    int64_t streamEnd100ns = 0;
    int64_t mediaStart = 0;
    int64_t mediaEnd = 0;
    uint32_t typeSpecificFlags = 0;
    bool hasStreamTime = false;
    bool hasMediaTime = false;
    bool discontinuity = false;
    bool preroll = false;
    std::vector<uint8_t> nv12;
};

class AsyncOutputCompletion final {
public:
    AsyncOutputCompletion() = default;
    AsyncOutputCompletion(const AsyncOutputCompletion&) = delete;
    AsyncOutputCompletion& operator=(const AsyncOutputCompletion&) = delete;

    void Complete(int32_t result) noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_completed) return;
                m_result = result;
                m_completed = true;
            }
            m_changed.notify_all();
        } catch (...) {
            m_changed.notify_all();
        }
    }

    int32_t Wait() noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this]() { return m_completed; });
            return m_result;
        } catch (...) {
            return static_cast<int32_t>(0x80004005u);
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_completed = false;
    int32_t m_result = static_cast<int32_t>(0x80004005u);
};

struct AsyncOutputFrame {
    uint64_t generation = 0;
    int64_t sequence = 0;
    int64_t pts = 0;
    int outputLineSizeY = 0;
    int outputLineSizeUV = 0;
    bool explicitPreroll = false;
    bool drainBarrier = false;
    bool terminalDrain = false;

    uint32_t repeatCount = 1;
    bool seekFallback = false;
    bool seekFallbackFirstPacket = false;
    uint64_t seekFallbackArmedTickNs = 0;
    int64_t fallbackSourceFrameIndex = -1;

    int64_t streamTimeOrigin100ns = 0;
    int64_t targetFrameDuration100ns = 1;
    int64_t outputTimeBaseNumerator = 0;
    int64_t outputTimeBaseDenominator = 1;
    int64_t targetFpsNumerator = 0;
    int64_t targetFpsDenominator = 1;
    int64_t prerollBoundaryTime100ns = 0;
    int64_t prerollBoundaryOutputPts = 0;
    int64_t segmentStart100ns = 0;
    uint32_t typeSpecificFlags = 0;
    bool prerollBoundaryTimeValid = false;
    bool prerollBoundaryOutputPtsValid = false;
    bool segmentMetadataValid = false;
    std::vector<uint8_t> nv12;

    std::shared_ptr<const std::vector<uint8_t>> sharedNv12;

    std::shared_ptr<const std::vector<uint8_t>> transitionFromNv12;
    uint32_t transitionBlendIndex = 0;
    uint32_t transitionBlendFrames = 0;
    bool transitionBlendApplied = false;
    std::shared_ptr<AsyncOutputCompletion> completion;
};

enum class AsyncQueueStatus : uint8_t {
    Queued,
    Item,
    Full,
    Empty,
    Closed,
    Cancelled,
    StaleGeneration
};

enum class AsyncWorkerKind : uint8_t {
    Processing,
    Delivery
};

enum class AsyncWorkerState : uint8_t {
    Dormant,
    Starting,
    Running,
    StopRequested,
    Exited,
    Failed
};
enum class AsyncPipelinePhase : uint8_t {
    Dormant,
    AcceptingInput,
    InputComplete,
    Cancelled,
    Failed
};

struct AsyncQueueMetrics {
    size_t queued = 0;
    size_t capacity = 0;
    size_t highWater = 0;
    uint64_t enqueued = 0;
    uint64_t dequeued = 0;
    uint64_t discarded = 0;
    uint64_t fullRejects = 0;
    uint64_t staleRejects = 0;
    uint64_t cancelledRejects = 0;
    uint64_t closedRejects = 0;
    uint64_t producerWaits = 0;
    uint64_t producerWaitTotalUs = 0;
    uint64_t producerWaitMaxUs = 0;
    uint64_t consumerWaits = 0;
    uint64_t consumerWaitTotalUs = 0;
    uint64_t consumerWaitMaxUs = 0;

    size_t fallbackQueued = 0;
    size_t fallbackCapacity = 0;
    size_t fallbackHighWater = 0;
    uint64_t fallbackEnqueued = 0;
    uint64_t fallbackDequeued = 0;
    uint64_t fallbackDiscarded = 0;
    uint64_t fallbackFullRejects = 0;
    uint64_t fallbackStaleRejects = 0;
    uint64_t fallbackCancelledRejects = 0;
    uint64_t fallbackClosedRejects = 0;
    uint64_t fallbackProducerWaits = 0;
    uint64_t fallbackProducerWaitTotalUs = 0;
    uint64_t fallbackProducerWaitMaxUs = 0;
    uint64_t fallbackConsumerWaits = 0;
    uint64_t fallbackConsumerWaitTotalUs = 0;
    uint64_t fallbackConsumerWaitMaxUs = 0;
};

struct AsyncPipelineSnapshot {
    uint64_t generation = 0;
    AsyncPipelinePhase phase = AsyncPipelinePhase::Dormant;
    AsyncWorkerState processingWorker = AsyncWorkerState::Dormant;
    AsyncWorkerState deliveryWorker = AsyncWorkerState::Dormant;
    AsyncQueueMetrics input;
    AsyncQueueMetrics output;
    bool processingBusy = false;
    bool deliveryBusy = false;
    uint64_t processingWorkItems = 0;
    uint64_t deliveryWorkItems = 0;
    bool inputClosed = true;
    bool outputClosed = true;
    bool cancelled = false;
    bool inputComplete = false;
    bool outputComplete = false;
    std::string reason;
};

class AsyncPipelineState final {
public:
    using Generation = uint64_t;

    static constexpr size_t kDefaultInputCapacity = 8;
    static constexpr size_t kDefaultOutputCapacity = 16;

    static constexpr size_t kDefaultFallbackOutputCapacity = 1024;

    explicit AsyncPipelineState(
        size_t inputCapacity = kDefaultInputCapacity,
        size_t outputCapacity = kDefaultOutputCapacity,
        size_t fallbackOutputCapacity = kDefaultFallbackOutputCapacity);
    ~AsyncPipelineState();

    AsyncPipelineState(const AsyncPipelineState&) = delete;
    AsyncPipelineState& operator=(const AsyncPipelineState&) = delete;
    AsyncPipelineState(AsyncPipelineState&&) = delete;
    AsyncPipelineState& operator=(AsyncPipelineState&&) = delete;

    Generation BeginGeneration();
    Generation CurrentGeneration() const noexcept;
    bool IsCurrentGeneration(Generation generation) const noexcept;

    AsyncQueueStatus TryEnqueueInput(AsyncSourceFrame&& frame);
    AsyncQueueStatus WaitDequeueInput(Generation generation, AsyncSourceFrame& frame);

    AsyncQueueStatus TryEnqueueOutput(AsyncOutputFrame&& frame);
    AsyncQueueStatus WaitEnqueueOutput(AsyncOutputFrame&& frame);
    AsyncQueueStatus WaitDequeueOutput(Generation generation, AsyncOutputFrame& frame);

    bool MarkInputComplete(Generation generation) noexcept;
    bool MarkOutputComplete(Generation generation) noexcept;
    bool CancelGeneration(Generation generation, const std::string& reason) noexcept;
    bool FailGeneration(Generation generation, const std::string& reason) noexcept;

    bool SetWorkerState(Generation generation, AsyncWorkerKind worker,
                        AsyncWorkerState state) noexcept;
    bool SetWorkerBusy(Generation generation, AsyncWorkerKind worker, bool busy) noexcept;
    AsyncPipelineSnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
