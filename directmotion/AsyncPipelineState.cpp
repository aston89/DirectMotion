#include "AsyncPipelineState.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace {

void CompleteDiscarded(AsyncOutputFrame& frame) noexcept {
    if (frame.completion) frame.completion->Complete(static_cast<int32_t>(0x80004004u));
}

template <typename T>
void CompleteDiscarded(T&) noexcept {}

template <typename T>
class GenerationBoundedQueue final {
public:
    explicit GenerationBoundedQueue(size_t capacity)
        : m_capacity((std::max)(static_cast<size_t>(1), capacity)) {}

    void Reset(uint64_t generation) {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_generation = generation;
            m_closed = false;
            m_cancelled = false;
            discarded.swap(m_items);
            m_metrics.discarded += static_cast<uint64_t>(discarded.size());
        }
        for (auto& item : discarded) CompleteDiscarded(item);
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    AsyncQueueStatus TryPush(uint64_t generation, T&& item) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation != m_generation) { ++m_metrics.staleRejects; return AsyncQueueStatus::StaleGeneration; }
            if (m_cancelled) { ++m_metrics.cancelledRejects; return AsyncQueueStatus::Cancelled; }
            if (m_closed) { ++m_metrics.closedRejects; return AsyncQueueStatus::Closed; }
            if (m_items.size() >= m_capacity) { ++m_metrics.fullRejects; return AsyncQueueStatus::Full; }
            m_items.emplace_back(std::move(item));
            RecordEnqueueLocked();
        }
        m_notEmpty.notify_one();
        return AsyncQueueStatus::Queued;
    }

    AsyncQueueStatus WaitPush(uint64_t generation, T&& item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool waited = m_items.size() >= m_capacity;
        const auto waitStarted = waited ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_notFull.wait(lock, [&]() {
            return generation != m_generation || m_closed || m_cancelled ||
                m_items.size() < m_capacity;
        });
        if (waited) RecordProducerWaitLocked(std::chrono::steady_clock::now() - waitStarted);
        if (generation != m_generation) { ++m_metrics.staleRejects; return AsyncQueueStatus::StaleGeneration; }
        if (m_cancelled) { ++m_metrics.cancelledRejects; return AsyncQueueStatus::Cancelled; }
        if (m_closed) { ++m_metrics.closedRejects; return AsyncQueueStatus::Closed; }
        m_items.emplace_back(std::move(item));
        RecordEnqueueLocked();
        lock.unlock();
        m_notEmpty.notify_one();
        return AsyncQueueStatus::Queued;
    }

    AsyncQueueStatus WaitPop(uint64_t generation, T& item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool waited = m_items.empty() && generation == m_generation && !m_cancelled && !m_closed;
        const auto waitStarted = waited ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_notEmpty.wait(lock, [&]() {
            return generation != m_generation || m_cancelled || m_closed || !m_items.empty();
        });
        if (waited) RecordConsumerWaitLocked(std::chrono::steady_clock::now() - waitStarted);
        if (generation != m_generation) { ++m_metrics.staleRejects; return AsyncQueueStatus::StaleGeneration; }
        if (m_cancelled) { ++m_metrics.cancelledRejects; return AsyncQueueStatus::Cancelled; }
        if (m_items.empty()) { ++m_metrics.closedRejects; return AsyncQueueStatus::Closed; }
        item = std::move(m_items.front());
        m_items.pop_front();
        ++m_metrics.dequeued;
        lock.unlock();
        m_notFull.notify_one();
        return AsyncQueueStatus::Item;
    }

    void Close(bool discardQueued, bool cancelled) noexcept {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cancelled = cancelled;
            if (discardQueued) {
                discarded.swap(m_items);
                m_metrics.discarded += static_cast<uint64_t>(discarded.size());
            }
        }
        for (auto& item : discarded) CompleteDiscarded(item);
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void Snapshot(AsyncQueueMetrics& metrics, bool& closed) const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        metrics = m_metrics;
        metrics.queued = m_items.size();
        metrics.capacity = m_capacity;
        closed = m_closed;
    }

private:
    static uint64_t ToMicroseconds(std::chrono::steady_clock::duration duration) noexcept {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        return us > 0 ? static_cast<uint64_t>(us) : 0;
    }
    void RecordEnqueueLocked() noexcept {
        ++m_metrics.enqueued;
        m_metrics.highWater = (std::max)(m_metrics.highWater, m_items.size());
    }
    void RecordProducerWaitLocked(std::chrono::steady_clock::duration duration) noexcept {
        const uint64_t us = ToMicroseconds(duration);
        ++m_metrics.producerWaits;
        m_metrics.producerWaitTotalUs += us;
        m_metrics.producerWaitMaxUs = (std::max)(m_metrics.producerWaitMaxUs, us);
    }
    void RecordConsumerWaitLocked(std::chrono::steady_clock::duration duration) noexcept {
        const uint64_t us = ToMicroseconds(duration);
        ++m_metrics.consumerWaits;
        m_metrics.consumerWaitTotalUs += us;
        m_metrics.consumerWaitMaxUs = (std::max)(m_metrics.consumerWaitMaxUs, us);
    }

    const size_t m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<T> m_items;
    uint64_t m_generation = 0;
    AsyncQueueMetrics m_metrics;
    bool m_closed = true;
    bool m_cancelled = false;
};

class GenerationPriorityOutputQueue final {
public:
    GenerationPriorityOutputQueue(size_t outputCapacity, size_t fallbackCapacity)
        : m_outputCapacity((std::max)(static_cast<size_t>(1), outputCapacity)),
          m_fallbackCapacity((std::max)(static_cast<size_t>(1), fallbackCapacity)) {}

    void Reset(uint64_t generation) {
        std::deque<AsyncOutputFrame> discardedOutput;
        std::deque<AsyncOutputFrame> discardedFallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_generation = generation;
            m_closed = false;
            m_cancelled = false;
            discardedOutput.swap(m_outputItems);
            discardedFallback.swap(m_fallbackItems);
            m_outputMetrics.discarded += static_cast<uint64_t>(discardedOutput.size());
            m_fallbackMetrics.discarded += static_cast<uint64_t>(discardedFallback.size());
        }
        for (auto& item : discardedOutput) CompleteDiscarded(item);
        for (auto& item : discardedFallback) CompleteDiscarded(item);
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    AsyncQueueStatus TryPush(AsyncOutputFrame&& item) {
        const bool fallback = item.seekFallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            AsyncQueueMetrics& metrics = fallback ? m_fallbackMetrics : m_outputMetrics;
            if (item.generation != m_generation) {
                ++metrics.staleRejects;
                return AsyncQueueStatus::StaleGeneration;
            }
            if (m_cancelled) {
                ++metrics.cancelledRejects;
                return AsyncQueueStatus::Cancelled;
            }
            if (m_closed) {
                ++metrics.closedRejects;
                return AsyncQueueStatus::Closed;
            }
            auto& queue = fallback ? m_fallbackItems : m_outputItems;
            const size_t capacity = fallback ? m_fallbackCapacity : m_outputCapacity;
            if (queue.size() >= capacity) {
                ++metrics.fullRejects;
                return AsyncQueueStatus::Full;
            }
            queue.emplace_back(std::move(item));
            RecordEnqueueLocked(metrics, queue.size());
        }
        m_notEmpty.notify_one();
        return AsyncQueueStatus::Queued;
    }

    AsyncQueueStatus WaitPush(AsyncOutputFrame&& item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool fallback = item.seekFallback;
        auto& queue = fallback ? m_fallbackItems : m_outputItems;
        const size_t capacity = fallback ? m_fallbackCapacity : m_outputCapacity;
        AsyncQueueMetrics& metrics = fallback ? m_fallbackMetrics : m_outputMetrics;
        const bool waited = queue.size() >= capacity;
        const auto waitStarted = waited ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        m_notFull.wait(lock, [&]() {
            return item.generation != m_generation || m_closed || m_cancelled ||
                queue.size() < capacity;
        });
        if (waited) RecordProducerWaitLocked(metrics, std::chrono::steady_clock::now() - waitStarted);
        if (item.generation != m_generation) {
            ++metrics.staleRejects;
            return AsyncQueueStatus::StaleGeneration;
        }
        if (m_cancelled) {
            ++metrics.cancelledRejects;
            return AsyncQueueStatus::Cancelled;
        }
        if (m_closed) {
            ++metrics.closedRejects;
            return AsyncQueueStatus::Closed;
        }
        queue.emplace_back(std::move(item));
        RecordEnqueueLocked(metrics, queue.size());
        lock.unlock();
        m_notEmpty.notify_one();
        return AsyncQueueStatus::Queued;
    }

    AsyncQueueStatus WaitPop(uint64_t generation, AsyncOutputFrame& item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool waited = m_outputItems.empty() && m_fallbackItems.empty() &&
            generation == m_generation && !m_cancelled && !m_closed;
        const auto waitStarted = waited ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        m_notEmpty.wait(lock, [&]() {
            return generation != m_generation || m_cancelled || m_closed ||
                !m_outputItems.empty() || !m_fallbackItems.empty();
        });
        if (generation != m_generation) {
            ++m_outputMetrics.staleRejects;
            return AsyncQueueStatus::StaleGeneration;
        }
        if (m_cancelled) {
            ++m_outputMetrics.cancelledRejects;
            return AsyncQueueStatus::Cancelled;
        }
        if (m_outputItems.empty() && m_fallbackItems.empty()) {
            ++m_outputMetrics.closedRejects;
            return AsyncQueueStatus::Closed;
        }

        const bool takeFallback = ShouldTakeFallbackLocked();
        auto& queue = takeFallback ? m_fallbackItems : m_outputItems;
        AsyncQueueMetrics& metrics = takeFallback ? m_fallbackMetrics : m_outputMetrics;
        if (waited) RecordConsumerWaitLocked(metrics, std::chrono::steady_clock::now() - waitStarted);
        item = std::move(queue.front());
        queue.pop_front();
        ++metrics.dequeued;
        lock.unlock();
        m_notFull.notify_all();
        return AsyncQueueStatus::Item;
    }

    void Close(bool discardQueued, bool cancelled) noexcept {
        std::deque<AsyncOutputFrame> discardedOutput;
        std::deque<AsyncOutputFrame> discardedFallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cancelled = cancelled;
            if (discardQueued) {
                discardedOutput.swap(m_outputItems);
                discardedFallback.swap(m_fallbackItems);
                m_outputMetrics.discarded += static_cast<uint64_t>(discardedOutput.size());
                m_fallbackMetrics.discarded += static_cast<uint64_t>(discardedFallback.size());
            }
        }
        for (auto& item : discardedOutput) CompleteDiscarded(item);
        for (auto& item : discardedFallback) CompleteDiscarded(item);
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void Snapshot(AsyncQueueMetrics& metrics, bool& closed) const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        metrics = m_outputMetrics;
        metrics.queued = m_outputItems.size();
        metrics.capacity = m_outputCapacity;
        metrics.fallbackQueued = m_fallbackItems.size();
        metrics.fallbackCapacity = m_fallbackCapacity;
        metrics.fallbackHighWater = m_fallbackMetrics.highWater;
        metrics.fallbackEnqueued = m_fallbackMetrics.enqueued;
        metrics.fallbackDequeued = m_fallbackMetrics.dequeued;
        metrics.fallbackDiscarded = m_fallbackMetrics.discarded;
        metrics.fallbackFullRejects = m_fallbackMetrics.fullRejects;
        metrics.fallbackStaleRejects = m_fallbackMetrics.staleRejects;
        metrics.fallbackCancelledRejects = m_fallbackMetrics.cancelledRejects;
        metrics.fallbackClosedRejects = m_fallbackMetrics.closedRejects;
        metrics.fallbackProducerWaits = m_fallbackMetrics.producerWaits;
        metrics.fallbackProducerWaitTotalUs = m_fallbackMetrics.producerWaitTotalUs;
        metrics.fallbackProducerWaitMaxUs = m_fallbackMetrics.producerWaitMaxUs;
        metrics.fallbackConsumerWaits = m_fallbackMetrics.consumerWaits;
        metrics.fallbackConsumerWaitTotalUs = m_fallbackMetrics.consumerWaitTotalUs;
        metrics.fallbackConsumerWaitMaxUs = m_fallbackMetrics.consumerWaitMaxUs;
        closed = m_closed;
    }

private:
    static uint64_t ToMicroseconds(std::chrono::steady_clock::duration duration) noexcept {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        return us > 0 ? static_cast<uint64_t>(us) : 0;
    }

    static void RecordEnqueueLocked(AsyncQueueMetrics& metrics, size_t queued) noexcept {
        ++metrics.enqueued;
        metrics.highWater = (std::max)(metrics.highWater, queued);
    }

    static void RecordProducerWaitLocked(AsyncQueueMetrics& metrics,
                                         std::chrono::steady_clock::duration duration) noexcept {
        const uint64_t us = ToMicroseconds(duration);
        ++metrics.producerWaits;
        metrics.producerWaitTotalUs += us;
        metrics.producerWaitMaxUs = (std::max)(metrics.producerWaitMaxUs, us);
    }

    static void RecordConsumerWaitLocked(AsyncQueueMetrics& metrics,
                                         std::chrono::steady_clock::duration duration) noexcept {
        const uint64_t us = ToMicroseconds(duration);
        ++metrics.consumerWaits;
        metrics.consumerWaitTotalUs += us;
        metrics.consumerWaitMaxUs = (std::max)(metrics.consumerWaitMaxUs, us);
    }

    bool ShouldTakeFallbackLocked() const noexcept {
        if (m_fallbackItems.empty()) return false;
        if (m_outputItems.empty()) return true;
        const AsyncOutputFrame& fallback = m_fallbackItems.front();
        const AsyncOutputFrame& output = m_outputItems.front();
        if (fallback.drainBarrier != output.drainBarrier) return !fallback.drainBarrier;
        if (fallback.pts != output.pts) return fallback.pts < output.pts;

        return true;
    }

    const size_t m_outputCapacity;
    const size_t m_fallbackCapacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<AsyncOutputFrame> m_outputItems;
    std::deque<AsyncOutputFrame> m_fallbackItems;
    uint64_t m_generation = 0;
    AsyncQueueMetrics m_outputMetrics;
    AsyncQueueMetrics m_fallbackMetrics;
    bool m_closed = true;
    bool m_cancelled = false;
};

bool IsWorkerActive(AsyncWorkerState state) noexcept {
    return state == AsyncWorkerState::Starting || state == AsyncWorkerState::Running;
}

} // namespace

struct AsyncPipelineState::Impl {
    Impl(size_t inputCapacity, size_t outputCapacity, size_t fallbackOutputCapacity)
        : input(inputCapacity), output(outputCapacity, fallbackOutputCapacity) {}

    mutable std::mutex lifecycleMutex;
    GenerationBoundedQueue<AsyncSourceFrame> input;
    GenerationPriorityOutputQueue output;
    Generation generation = 0;
    AsyncWorkerState processingWorker = AsyncWorkerState::Dormant;
    AsyncWorkerState deliveryWorker = AsyncWorkerState::Dormant;
    bool acceptingInput = false;
    bool inputComplete = false;
    bool outputComplete = false;
    bool processingBusy = false;
    bool deliveryBusy = false;
    uint64_t processingWorkItems = 0;
    uint64_t deliveryWorkItems = 0;
    bool cancelled = false;
    bool failed = false;
    std::string reason;
};

AsyncPipelineState::AsyncPipelineState(size_t inputCapacity, size_t outputCapacity,
                                       size_t fallbackOutputCapacity)
    : m_impl(std::make_unique<Impl>(inputCapacity, outputCapacity, fallbackOutputCapacity)) {}

AsyncPipelineState::~AsyncPipelineState() {

    if (!m_impl) return;
    const Generation generation = CurrentGeneration();
    if (generation != 0) CancelGeneration(generation, "AsyncPipelineState destruction");
}

AsyncPipelineState::Generation AsyncPipelineState::BeginGeneration() {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    Generation next = m_impl->generation + 1;
    if (next == 0) next = 1;

    m_impl->input.Reset(next);
    m_impl->output.Reset(next);
    m_impl->processingWorker = AsyncWorkerState::Dormant;
    m_impl->deliveryWorker = AsyncWorkerState::Dormant;
    m_impl->acceptingInput = true;
    m_impl->inputComplete = false;
    m_impl->outputComplete = false;
    m_impl->processingBusy = false;
    m_impl->deliveryBusy = false;
    m_impl->processingWorkItems = 0;
    m_impl->deliveryWorkItems = 0;
    m_impl->cancelled = false;
    m_impl->failed = false;
    m_impl->reason.clear();
    m_impl->generation = next;
    return next;
}

AsyncPipelineState::Generation AsyncPipelineState::CurrentGeneration() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    return m_impl->generation;
}

bool AsyncPipelineState::IsCurrentGeneration(Generation generation) const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    return generation != 0 && generation == m_impl->generation && !m_impl->cancelled;
}

AsyncQueueStatus AsyncPipelineState::TryEnqueueInput(AsyncSourceFrame&& frame) {
    return m_impl->input.TryPush(frame.generation, std::move(frame));
}

AsyncQueueStatus AsyncPipelineState::WaitDequeueInput(Generation generation, AsyncSourceFrame& frame) {
    return m_impl->input.WaitPop(generation, frame);
}

AsyncQueueStatus AsyncPipelineState::TryEnqueueOutput(AsyncOutputFrame&& frame) {
    return m_impl->output.TryPush(std::move(frame));
}

AsyncQueueStatus AsyncPipelineState::WaitEnqueueOutput(AsyncOutputFrame&& frame) {
    return m_impl->output.WaitPush(std::move(frame));
}

AsyncQueueStatus AsyncPipelineState::WaitDequeueOutput(Generation generation, AsyncOutputFrame& frame) {
    return m_impl->output.WaitPop(generation, frame);
}

bool AsyncPipelineState::MarkInputComplete(Generation generation) noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (generation == 0 || generation != m_impl->generation ||
        m_impl->cancelled || m_impl->failed) return false;
    if (m_impl->inputComplete) return true;
    m_impl->acceptingInput = false;
    m_impl->inputComplete = true;
    m_impl->input.Close(false, false);
    return true;
}

bool AsyncPipelineState::MarkOutputComplete(Generation generation) noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (generation == 0 || generation != m_impl->generation ||
        m_impl->cancelled || m_impl->failed) return false;
    if (m_impl->outputComplete) return true;
    m_impl->outputComplete = true;
    m_impl->output.Close(false, false);
    return true;
}

bool AsyncPipelineState::CancelGeneration(Generation generation, const std::string& reason) noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
        if (generation == 0 || generation != m_impl->generation) return false;
        if (m_impl->cancelled) return true;
        m_impl->cancelled = true;
        m_impl->acceptingInput = false;
        m_impl->inputComplete = true;
        m_impl->outputComplete = true;
        m_impl->reason = reason;
        if (IsWorkerActive(m_impl->processingWorker))
            m_impl->processingWorker = AsyncWorkerState::StopRequested;
        if (IsWorkerActive(m_impl->deliveryWorker))
            m_impl->deliveryWorker = AsyncWorkerState::StopRequested;
        m_impl->input.Close(true, true);
        m_impl->output.Close(true, true);
        return true;
    } catch (...) {
        return false;
    }
}

bool AsyncPipelineState::FailGeneration(Generation generation, const std::string& reason) noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
        if (generation == 0 || generation != m_impl->generation) return false;
        m_impl->failed = true;
        m_impl->cancelled = true;
        m_impl->acceptingInput = false;
        m_impl->inputComplete = true;
        m_impl->outputComplete = true;
        m_impl->reason = reason;
        m_impl->processingWorker = AsyncWorkerState::Failed;
        m_impl->deliveryWorker = AsyncWorkerState::Failed;
        m_impl->input.Close(true, true);
        m_impl->output.Close(true, true);
        return true;
    } catch (...) {
        return false;
    }
}

bool AsyncPipelineState::SetWorkerState(Generation generation, AsyncWorkerKind worker,
                                        AsyncWorkerState state) noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (generation == 0 || generation != m_impl->generation) return false;
    if (worker == AsyncWorkerKind::Processing) m_impl->processingWorker = state;
    else m_impl->deliveryWorker = state;
    return true;
}

bool AsyncPipelineState::SetWorkerBusy(Generation generation, AsyncWorkerKind worker, bool busy) noexcept {
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    if (generation == 0 || generation != m_impl->generation) return false;
    bool& current = worker == AsyncWorkerKind::Processing ? m_impl->processingBusy : m_impl->deliveryBusy;
    if (busy && !current) {
        if (worker == AsyncWorkerKind::Processing) ++m_impl->processingWorkItems;
        else ++m_impl->deliveryWorkItems;
    }
    current = busy;
    return true;
}

AsyncPipelineSnapshot AsyncPipelineState::Snapshot() const {
    AsyncPipelineSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_impl->lifecycleMutex);
    snapshot.generation = m_impl->generation;
    snapshot.processingWorker = m_impl->processingWorker;
    snapshot.deliveryWorker = m_impl->deliveryWorker;
    snapshot.cancelled = m_impl->cancelled;
    snapshot.inputComplete = m_impl->inputComplete;
    snapshot.outputComplete = m_impl->outputComplete;
    snapshot.processingBusy = m_impl->processingBusy;
    snapshot.deliveryBusy = m_impl->deliveryBusy;
    snapshot.processingWorkItems = m_impl->processingWorkItems;
    snapshot.deliveryWorkItems = m_impl->deliveryWorkItems;
    snapshot.reason = m_impl->reason;
    m_impl->input.Snapshot(snapshot.input, snapshot.inputClosed);
    m_impl->output.Snapshot(snapshot.output, snapshot.outputClosed);

    if (m_impl->failed) snapshot.phase = AsyncPipelinePhase::Failed;
    else if (m_impl->cancelled) snapshot.phase = AsyncPipelinePhase::Cancelled;
    else if (m_impl->inputComplete) snapshot.phase = AsyncPipelinePhase::InputComplete;
    else if (m_impl->acceptingInput) snapshot.phase = AsyncPipelinePhase::AcceptingInput;
    else snapshot.phase = AsyncPipelinePhase::Dormant;
    return snapshot;
}
