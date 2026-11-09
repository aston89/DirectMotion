#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace DirectMotionLifecycle {

// Counts active lifecycle owners instead of representing overlapping Flush or
// NewSegment callbacks with a single boolean. One callback can release only its
// own lease; an older callback cannot clear a newer transition.
class TransitionGate final {
public:
    TransitionGate() noexcept = default;
    TransitionGate(const TransitionGate&) = delete;
    TransitionGate& operator=(const TransitionGate&) = delete;

    void Acquire() noexcept {
        uint32_t current = m_active.load(std::memory_order_acquire);
        while (current != (std::numeric_limits<uint32_t>::max)()) {
            if (m_active.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return;
        }
    }

    void Release() noexcept {
        uint32_t current = m_active.load(std::memory_order_acquire);
        while (current != 0) {
            if (m_active.compare_exchange_weak(current, current - 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return;
        }
    }

    bool Active() const noexcept {
        return m_active.load(std::memory_order_acquire) != 0;
    }

    uint32_t Count() const noexcept {
        return m_active.load(std::memory_order_acquire);
    }

    // Used only at a fully quiescent StartStreaming boundary.
    void Reset() noexcept {
        m_active.store(0, std::memory_order_release);
    }

private:
    std::atomic<uint32_t> m_active{0};
};

// Scope-bound lease for transitions whose lifetime is contained in one call.
// Release() allows a callback to end the gate before doing unrelated work.
class TransitionLease final {
public:
    explicit TransitionLease(TransitionGate& gate) noexcept : m_gate(&gate) {
        m_gate->Acquire();
    }
    ~TransitionLease() noexcept { Release(); }

    TransitionLease(const TransitionLease&) = delete;
    TransitionLease& operator=(const TransitionLease&) = delete;

    TransitionLease(TransitionLease&& other) noexcept : m_gate(other.m_gate) {
        other.m_gate = nullptr;
    }
    TransitionLease& operator=(TransitionLease&& other) noexcept {
        if (this != &other) {
            Release();
            m_gate = other.m_gate;
            other.m_gate = nullptr;
        }
        return *this;
    }

    void Release() noexcept {
        if (m_gate) {
            m_gate->Release();
            m_gate = nullptr;
        }
    }

private:
    TransitionGate* m_gate;
};

} // namespace DirectMotionLifecycle
