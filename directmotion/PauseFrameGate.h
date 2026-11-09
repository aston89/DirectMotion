#pragma once

#include <atomic>
#include <cstdint>

// Tracks the single decoded source frame that may be presented for a paused
// lifecycle epoch. A monotonically increasing epoch invalidates allocator waits
// that complete after Run/Stop/Flush/seek has already superseded the pause.
class PauseFrameGate final {
public:
    using Epoch = uint64_t;

    Epoch CurrentEpoch() const noexcept {
        return m_epoch.load(std::memory_order_acquire);
    }

    // Starts a new paused-display opportunity (or invalidates an old one).
    Epoch Invalidate() noexcept {
        return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    bool IsCurrent(Epoch epoch) const noexcept {
        return epoch != 0 && CurrentEpoch() == epoch;
    }

    // At most one Receive callback may own the frame attempt for an epoch.
    // The epoch is rechecked after claiming to close the invalidate/claim race.
    bool TryClaim(Epoch epoch) noexcept {
        if (!IsCurrent(epoch) || WasPresented(epoch)) return false;
        Epoch claimed = m_claimedEpoch.load(std::memory_order_acquire);
        while (claimed != epoch) {
            if (m_claimedEpoch.compare_exchange_weak(claimed, epoch,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (IsCurrent(epoch) && !WasPresented(epoch)) return true;
                ReleaseClaim(epoch);
                return false;
            }
        }
        return false;
    }

    void ReleaseClaim(Epoch epoch) noexcept {
        Epoch expected = epoch;
        m_claimedEpoch.compare_exchange_strong(expected, 0,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void MarkPresented(Epoch epoch) noexcept {
        if (IsCurrent(epoch)) m_presentedEpoch.store(epoch, std::memory_order_release);
        // Retire the one-shot claim whether it completed in its own epoch or was
        // superseded while Receive was finishing the post-Deliver engine reset.
        // A current presented marker still blocks a second claim for this epoch.
        ReleaseClaim(epoch);
    }

    bool WasPresented(Epoch epoch) const noexcept {
        return IsCurrent(epoch) &&
            m_presentedEpoch.load(std::memory_order_acquire) == epoch;
    }

    // A successful Run closes the previous pause epoch. It also reports whether
    // a claimed frame attempt had not yet completed its visible presentation. The
    // caller must flush downstream before taking the delivery fence in that case:
    // invalidating the token alone would discard the eventual result but leave a
    // blocked GetDeliveryBuffer/Deliver call alive until some unrelated later flush.
    bool AdvanceForRun(bool* pendingFrameRequest = nullptr) noexcept {
        const Epoch previousEpoch = m_epoch.fetch_add(1, std::memory_order_acq_rel);
        const Epoch presented = m_presentedEpoch.exchange(0, std::memory_order_acq_rel);
        const Epoch claimed = m_claimedEpoch.load(std::memory_order_acquire);
        const bool framePresented = presented == previousEpoch;
        if (framePresented) ReleaseClaim(previousEpoch);
        // A previous Run may have failed to enter downstream flush. In that case
        // the claim belongs to an older epoch but still identifies a blocked request;
        // keep asking Run to retry the flush until that Receive releases its claim.
        if (pendingFrameRequest) *pendingFrameRequest = claimed != 0 && !framePresented;
        return framePresented;
    }

    // ResetEngine clears display bookkeeping but deliberately keeps the claim:
    // the paused-frame path resets the engine after delivery and then marks this
    // same epoch as presented. The claim prevents another input sample meanwhile.
    void ClearPresented() noexcept {
        m_presentedEpoch.store(0, std::memory_order_release);
    }

private:
    std::atomic<Epoch> m_epoch{1};
    std::atomic<Epoch> m_claimedEpoch{0};
    std::atomic<Epoch> m_presentedEpoch{0};
};
