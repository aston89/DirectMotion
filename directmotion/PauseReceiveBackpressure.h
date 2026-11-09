#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Lifecycle signal used by the DirectShow input pin to hold the next source
// Receive after the single visible paused frame. The token is captured before
// the paused/presented predicate is checked, so a Run/Cancel racing with entry
// cannot be lost between the predicate check and condition-variable wait.
class PauseReceiveBackpressure final {
public:
    struct Token {
        uint64_t runEpoch = 0;
        uint64_t cancelEpoch = 0;
    };

    enum class WaitResult {
        Resumed,
        Cancelled,
        Failed
    };

    Token Capture() const noexcept {
        return {
            m_runEpoch.load(std::memory_order_acquire),
            m_cancelEpoch.load(std::memory_order_acquire)
        };
    }

    uint64_t NotifyRun() noexcept {
        const uint64_t epoch = m_runEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        NotifyAll();
        return epoch;
    }

    uint64_t Cancel() noexcept {
        const uint64_t epoch = m_cancelEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        NotifyAll();
        return epoch;
    }

    WaitResult WaitForChange(Token token) noexcept {
        bool registered = false;
        try {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_waiters.fetch_add(1, std::memory_order_acq_rel);
            registered = true;
            m_changed.wait(lock, [this, token]() {
                return m_runEpoch.load(std::memory_order_acquire) != token.runEpoch ||
                    m_cancelEpoch.load(std::memory_order_acquire) != token.cancelEpoch;
            });
            m_waiters.fetch_sub(1, std::memory_order_acq_rel);
            registered = false;
            return m_cancelEpoch.load(std::memory_order_acquire) != token.cancelEpoch
                ? WaitResult::Cancelled : WaitResult::Resumed;
        } catch (...) {
            // A synchronization failure must not strand the DirectShow graph.
            // Report failure so the caller can abort this Receive explicitly.
            if (registered) m_waiters.fetch_sub(1, std::memory_order_acq_rel);
            return WaitResult::Failed;
        }
    }

    uint32_t WaitingCount() const noexcept {
        return m_waiters.load(std::memory_order_acquire);
    }

    uint64_t RunEpoch() const noexcept {
        return m_runEpoch.load(std::memory_order_acquire);
    }

private:
    void NotifyAll() noexcept {
        try {
            // Pair notification with the wait mutex. The waiter captures its
            // token before entering WaitForChange; this lock closes the lost-wake
            // window between checking the atomic predicate and sleeping.
            std::lock_guard<std::mutex> lock(m_mutex);
            m_changed.notify_all();
        } catch (...) {
            m_changed.notify_all();
        }
    }

    std::atomic<uint64_t> m_runEpoch{0};
    std::atomic<uint64_t> m_cancelEpoch{0};
    std::atomic<uint32_t> m_waiters{0};
    std::mutex m_mutex;
    std::condition_variable m_changed;
};
