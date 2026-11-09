#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace DirectMotionSourceBootstrap {

constexpr size_t kDefaultByteBudget = 768u * 1024u * 1024u;

enum class AdmissionResult : uint8_t {
    Ready = 0,
    Cancelled,
    TooLarge
};

enum class PushResult : uint8_t {
    Queued = 0,
    Inactive,
    StaleGeneration,
    Full,
    Invalid
};

struct Frame final {
    uint64_t generation = 0;
    uint64_t epoch = 0;
    int64_t sourceFrameIndex = -1;
    size_t chargedBytes = 0;
    bool completed = false;
    std::shared_ptr<const std::vector<uint8_t>> nv12;
};

class Queue final {
    struct BudgetState final {
        explicit BudgetState(size_t budget) noexcept : byteBudget(budget) {}
        const size_t byteBudget;
        mutable std::mutex mutex;
        std::condition_variable changed;
        size_t bytesOutstanding = 0;
        bool admissionActive = false;
        uint64_t admissionEpoch = 0;
    };

public:
    explicit Queue(size_t byteBudget = kDefaultByteBudget)
        : m_budget(std::make_shared<BudgetState>(byteBudget == 0 ? kDefaultByteBudget : byteBudget)) {}

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void BeginGeneration(uint64_t generation, bool workerReady = true) noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ClearQueuedLocked();
                m_generationOutstandingFrames = 0;
                m_generation = generation;
                ++m_epoch;
                if (m_epoch == 0) ++m_epoch;
                m_active = generation != 0;
                m_workerReady = generation != 0 && workerReady;
                m_fallbackAdmissionActive = generation != 0;

                std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
                m_budget->admissionActive = generation != 0;
                ++m_budget->admissionEpoch;
                if (m_budget->admissionEpoch == 0) ++m_budget->admissionEpoch;
            }
            NotifyWaiters();
        } catch (...) {
            Cancel();
        }
    }

    void Cancel() noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ClearQueuedLocked();
                m_generationOutstandingFrames = 0;
                m_generation = 0;
                m_active = false;
                m_workerReady = false;
                m_fallbackAdmissionActive = false;
                ++m_epoch;
                if (m_epoch == 0) ++m_epoch;
                std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
                m_budget->admissionActive = false;
                ++m_budget->admissionEpoch;
                if (m_budget->admissionEpoch == 0) ++m_budget->admissionEpoch;
            }
            NotifyWaiters();
        } catch (...) {
            NotifyWaiters();
        }
    }

    void NotifyWaiters() noexcept {
        m_changed.notify_all();
        m_budget->changed.notify_all();
    }

    void SetFallbackAdmissionActive(bool active) noexcept {
        try {
            {
                std::lock_guard<std::mutex> queueLock(m_mutex);
                m_fallbackAdmissionActive = active;

                std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
                m_budget->admissionActive = active || m_active;
            }
            m_budget->changed.notify_all();
        } catch (...) {
        }
    }

    bool IsActiveForGeneration(uint64_t generation) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_active && generation != 0 && m_generation == generation;
        } catch (...) {
            return false;
        }
    }

    bool IsWorkerReadyForGeneration(uint64_t generation) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_active && m_workerReady && generation != 0 &&
                m_generation == generation;
        } catch (...) {
            return false;
        }
    }

    bool RebindDormantGeneration(uint64_t previousGeneration,
                                 uint64_t nextGeneration) noexcept {
        if (previousGeneration == 0 || nextGeneration == 0 ||
            previousGeneration == nextGeneration) return false;
        try {
            {
                std::lock_guard<std::mutex> queueLock(m_mutex);
                if (!m_active || m_workerReady ||
                    m_generation != previousGeneration ||
                    m_generationOutstandingFrames != m_frames.size()) {
                    return false;
                }

                ++m_epoch;
                if (m_epoch == 0) ++m_epoch;
                m_generation = nextGeneration;
                for (auto& frame : m_frames) {
                    frame.generation = nextGeneration;
                    frame.epoch = m_epoch;
                }

                std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
                m_budget->admissionActive = true;
                ++m_budget->admissionEpoch;
                if (m_budget->admissionEpoch == 0) ++m_budget->admissionEpoch;
            }
            NotifyWaiters();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool SetWorkerReadyForGeneration(uint64_t generation, bool ready) noexcept {
        if (generation == 0) return false;
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_active || m_generation != generation) return false;
                m_workerReady = ready;
            }
            NotifyWaiters();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool IsCurrent(const Frame& frame) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_active && frame.generation != 0 &&
                frame.generation == m_generation && frame.epoch == m_epoch;
        } catch (...) {
            return false;
        }
    }

    bool ShouldBackpressure(size_t frameBytes) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_budget->mutex);
            return m_budget->admissionActive && !FitsLocked(frameBytes);
        } catch (...) {
            return true;
        }
    }

    template <typename CancelPredicate>
    AdmissionResult WaitForAdmission(size_t frameBytes, CancelPredicate cancelled) noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_budget->mutex);
            const uint64_t capturedEpoch = m_budget->admissionEpoch;
            m_budget->changed.wait(lock, [this, frameBytes, capturedEpoch, &cancelled]() {
                return !m_budget->admissionActive || frameBytes > m_budget->byteBudget ||
                    FitsLocked(frameBytes) || cancelled() ||
                    capturedEpoch != m_budget->admissionEpoch;
            });
            if (cancelled() || capturedEpoch != m_budget->admissionEpoch)
                return AdmissionResult::Cancelled;
            if (frameBytes > m_budget->byteBudget) return AdmissionResult::TooLarge;
            return AdmissionResult::Ready;
        } catch (...) {
            return AdmissionResult::Cancelled;
        }
    }

    std::shared_ptr<const std::vector<uint8_t>> CreatePayload(
        std::vector<uint8_t>& source) noexcept {
        if (source.empty()) return {};

        const size_t bytes = source.capacity();
        std::vector<uint8_t>* raw = nullptr;
        try {
            raw = new std::vector<uint8_t>();
        } catch (...) {
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(m_budget->mutex);
            if (!m_budget->admissionActive || !FitsLocked(bytes)) {
                delete raw;
                return {};
            }
            m_budget->bytesOutstanding += bytes;
        }
        raw->swap(source);
        const auto budget = m_budget;
        try {
            return std::shared_ptr<const std::vector<uint8_t>>(raw,
                [budget, bytes](const std::vector<uint8_t>* payload) noexcept {
                    delete payload;
                    {
                        std::lock_guard<std::mutex> lock(budget->mutex);
                        budget->bytesOutstanding = bytes > budget->bytesOutstanding
                            ? 0 : budget->bytesOutstanding - bytes;
                    }
                    budget->changed.notify_all();
                });
        } catch (...) {

            return {};
        }
    }

    PushResult TryPush(uint64_t generation, int64_t sourceFrameIndex,
                       const std::shared_ptr<const std::vector<uint8_t>>& payload,
                       bool notify = true) noexcept {
        if (!payload || payload->empty() || generation == 0 || sourceFrameIndex < 0)
            return PushResult::Invalid;
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_active) return PushResult::Inactive;
                if (generation != m_generation) return PushResult::StaleGeneration;
                Frame frame;
                frame.generation = generation;
                frame.epoch = m_epoch;
                frame.sourceFrameIndex = sourceFrameIndex;
                frame.chargedBytes = payload->capacity();
                frame.nv12 = payload;
                m_frames.push_back(std::move(frame));
                ++m_generationOutstandingFrames;
            }
            if (notify) m_changed.notify_all();
            return PushResult::Queued;
        } catch (...) {
            return PushResult::Invalid;
        }
    }

    PushResult TryPush(uint64_t generation, int64_t sourceFrameIndex,
                       std::vector<uint8_t>& nv12) noexcept {
        if (nv12.empty() || generation == 0 || sourceFrameIndex < 0)
            return PushResult::Invalid;

        {
            std::lock_guard<std::mutex> queueLock(m_mutex);
            if (!m_active) return PushResult::Inactive;
            if (generation != m_generation) return PushResult::StaleGeneration;
        }
        const auto payload = CreatePayload(nv12);
        if (!payload) {
            std::lock_guard<std::mutex> queueLock(m_mutex);
            if (!m_active) return PushResult::Inactive;
            if (generation != m_generation) return PushResult::StaleGeneration;
            std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
            return FitsLocked(nv12.capacity()) ? PushResult::Invalid : PushResult::Full;
        }
        return TryPush(generation, sourceFrameIndex, payload);
    }

    PushResult TryPush(uint64_t generation, int64_t sourceFrameIndex,
                       std::vector<uint8_t>&& nv12) noexcept {
        return TryPush(generation, sourceFrameIndex, nv12);
    }

    template <typename StopPredicate>
    bool WaitPop(Frame& frame, StopPredicate stopping) noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this, &stopping]() {
                return stopping() || (m_workerReady && !m_frames.empty());
            });
            if (stopping() || !m_workerReady || m_frames.empty()) return false;
            frame = std::move(m_frames.front());
            m_frames.pop_front();
            return true;
        } catch (...) {
            return false;
        }
    }

    void CompleteFrame(Frame& frame) noexcept {

        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (frame.completed) return;
                frame.completed = true;
                if (frame.generation == m_generation && frame.epoch == m_epoch &&
                    m_generationOutstandingFrames > 0) {
                    --m_generationOutstandingFrames;
                }
            }

            frame.nv12.reset();
            frame.chargedBytes = 0;
            m_changed.notify_all();
            m_budget->changed.notify_all();
        } catch (...) {
            m_changed.notify_all();
            m_budget->changed.notify_all();
        }
    }

    bool TryCompleteGeneration(uint64_t generation) noexcept {
        try {
            bool completed = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_active && generation != 0 && generation == m_generation &&
                    m_frames.empty() && m_generationOutstandingFrames == 0) {
                    m_active = false;
                    m_generation = 0;
                    if (!m_fallbackAdmissionActive) {
                        std::lock_guard<std::mutex> budgetLock(m_budget->mutex);
                        m_budget->admissionActive = false;
                    }
                    completed = true;
                }
            }
            if (completed) {
                NotifyWaiters();
            }
            return completed;
        } catch (...) {
            return false;
        }
    }

    size_t BytesOutstanding() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_budget->mutex);
            return m_budget->bytesOutstanding;
        } catch (...) {
            return 0;
        }
    }

    size_t QueuedFrames() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_frames.size();
        } catch (...) {
            return 0;
        }
    }

    size_t ByteBudget() const noexcept { return m_budget->byteBudget; }

private:
    bool FitsLocked(size_t frameBytes) const noexcept {
        return frameBytes != 0 && frameBytes <= m_budget->byteBudget &&
            m_budget->bytesOutstanding <= m_budget->byteBudget - frameBytes;
    }

    void ClearQueuedLocked() noexcept {
        const size_t queued = m_frames.size();
        m_generationOutstandingFrames = queued > m_generationOutstandingFrames
            ? 0 : m_generationOutstandingFrames - queued;
        m_frames.clear();
    }

    std::shared_ptr<BudgetState> m_budget;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Frame> m_frames;
    size_t m_generationOutstandingFrames = 0;
    uint64_t m_generation = 0;
    uint64_t m_epoch = 0;
    bool m_active = false;
    bool m_workerReady = false;
    bool m_fallbackAdmissionActive = false;
};

} // namespace DirectMotionSourceBootstrap
