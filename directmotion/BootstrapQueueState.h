#pragma once

#include <atomic>

// Tracks only whether the bootstrap packet has been accepted by the output FIFO.
// It deliberately does not claim that DeliveryWorker has popped the packet or that
// the downstream allocator/renderer has accepted it. FIFO ordering keeps this queued
// preroll ahead of later generated output while downstream delivery proceeds separately.
class BootstrapQueueState final {
public:
    BootstrapQueueState() = default;
    BootstrapQueueState(const BootstrapQueueState&) = delete;
    BootstrapQueueState& operator=(const BootstrapQueueState&) = delete;

    bool HasBeenEnqueued() const noexcept {
        return m_queued.load(std::memory_order_acquire);
    }

    // Call only after WaitEnqueueOutput() returns AsyncQueueStatus::Queued.
    void MarkEnqueued() noexcept {
        m_queued.store(true, std::memory_order_release);
    }

    // Called when ResetEngine retires the output generation and rebuilds the graph.
    void Reset() noexcept {
        m_queued.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> m_queued{false};
};
