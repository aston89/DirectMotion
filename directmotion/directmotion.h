#pragma once

#include <streams.h>
#include <dvdmedia.h>
#include <memory>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include "FrucRational.h"
#include "AviSynthMvToolsEngine.h"
#include "AsyncPipelineState.h"
#include "PauseFrameGate.h"
#include "PauseReceiveBackpressure.h"
#include "InitialPrerollPolicy.h"
#include "InitialSegmentGraphPolicy.h"
#include "PauseRetentionPolicy.h"
#include "BootstrapQueueState.h"
#include "SeekFallbackPolicy.h"
#include "SeekFallbackGenerationPolicy.h"
#include "SeekResetGenerationPolicy.h"
#include "SeekTransitionBlend.h"
#include "ReceiveCommitPolicy.h"
#include "SourceBootstrapQueue.h"
#include "LifecycleTransitionGate.h"
using ActiveFrucEngine = AviSynthMvToolsEngine;

// Persistent values deliberately avoid the legacy 1/3/4/... rate-mode IDs,
// which older releases used for removed custom/absolute-rate presets.
constexpr int kRateMode2x = 0;
constexpr int kRateMode3x = 2; // preserve the established 0.2.45 registry value
constexpr int kRateMode25x = 0x445A;
constexpr int kRateMode4x = 0x445B;
constexpr int kRateModeAuto = 0x445C;

inline bool IsSupportedRateMode(int mode) noexcept {
    return mode == kRateMode2x || mode == kRateMode25x || mode == kRateMode3x ||
        mode == kRateMode4x || mode == kRateModeAuto;
}

struct DirectMotionSettings {
    bool enabled = true;
    // 2x, 2.5x, 3x, 4x or Auto (the active display refresh rate).
    int rateMode = kRateMode2x;
    // Retained for settings-structure layout compatibility with previous property-page builds.
    // Since version 0.2.25, arbitrary rates are no longer exposed or used.
    int customRateNum = 60;
    int customRateDen = 1;
    // Complete MVTools2 quick profile: 1 quality, 2 balanced, 3 fast.
    int performance = 2;
    // Legacy-encoded MAnalyse-only grid override: 0 auto, 1=4, 2=8, 4=16, 8=32 pixels.
    int grid = 4;
    AviSynthMvToolsOptions mvtools{};
    bool saveFirstInputBmp = false;
    bool logEnabled = false;
    // The legacy Grid value is retained for registry/API compatibility. It only
    // overrides the selected profile's MAnalyse block size when this is true.
    bool gridOverrideEnabled = false;
    // Prefetch concurrency policy. Auto is intentionally the 0.2.47 logical-CPU
    // behavior. Physical and HalfLogical are concurrency caps, not thread affinity.
    int cpuProfile = kCpuProfileAuto;
    int customCpuWorkers = 8;
    // Number of input Receive samples to ingest before draining the synchronous
    // output graph. 1 preserves the original per-Receive behavior; larger values
    // add bounded source lead without introducing an output worker or sample queue.
    int receiveBatchFrames = 1;
    // AviSynth+ Prefetch frame window. 0 means automatic (2x worker threads);
    // explicit values are independent of Quality/Balanced/Fast profile selection.
    int prefetchFrames = 0;
    // Optional quick-profile experiment: retain the selected MAnalyse profile but
    // use Fast's 16px OneTime MRecalculate refinement. 0 keeps the profile default.
    int quickRecalcMode = 0;
    // Deprecated ABI slot retained for binary layout compatibility with clients
    // built against 0.2.58.20-0.2.58.24. It is always returned as zero and ignored:
    // post-seek handoff is immediate once the frontier/lookahead are genuinely ready.
    int seekHandoffDelayMs = 0;
};

struct IDirectMotionSettings : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetSettings(DirectMotionSettings* settings) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSettings(const DirectMotionSettings* settings) = 0;
};

class CDirectMotion final : public CTransformFilter,
                           public ISpecifyPropertyPages,
                           public IDirectMotionSettings {
public:
    DECLARE_IUNKNOWN;
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN punk, HRESULT* phr);

    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    int GetPinCount() override;
    CBasePin* GetPin(int index) override;

    HRESULT CheckInputType(const CMediaType* mtIn) override;
    HRESULT CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut) override;
    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* properties) override;
    HRESULT GetMediaType(int position, CMediaType* mediaType) override;
    HRESULT SetMediaType(PIN_DIRECTION direction, const CMediaType* mediaType) override;
    HRESULT CompleteConnect(PIN_DIRECTION direction, IPin* receivePin) override;
    HRESULT BreakConnect(PIN_DIRECTION direction) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Stop() override;
    HRESULT Run(REFERENCE_TIME start) override;

    // Custom Receive is required because FRUC can emit zero, one or multiple frames per input sample.
    HRESULT Receive(IMediaSample* sample) override;
    HRESULT WaitForReceiveAdmission(IMediaSample* sample) override;
    bool ShouldWaitForReceiveAdmission(IMediaSample* sample) override;
    HRESULT EndOfStream() override;
    HRESULT BeginFlush() override;
    HRESULT EndFlush() override;
    HRESULT NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;
    HRESULT StartStreaming() override;
    HRESULT StopStreaming() override;

    STDMETHODIMP GetPages(CAUUID* pages) override;

    STDMETHODIMP GetSettings(DirectMotionSettings* settings) override;
    STDMETHODIMP SetSettings(const DirectMotionSettings* settings) override;

    CDirectMotion(LPCTSTR name, LPUNKNOWN punk, HRESULT* phr);
    ~CDirectMotion() override;

private:
    bool ParseInputMediaType(const CMediaType& mediaType);
    FrucRational SourceFrameRate() const;
    FrucRational TargetFrameRate() const;
    REFERENCE_TIME TargetFrameDuration() const;
    bool EnsureEngine(std::string& error,
                      AsyncPipelineState::Generation expectedReceiveGeneration = 0);
    // Post-seek graph creation runs outside m_csReceive. A local candidate is
    // committed under the lock only after its pipeline/source generation is revalidated.
    bool EnsureEngineForBootstrapGeneration(
        AsyncPipelineState::Generation expectedGeneration, std::string& error);
    bool IsReceiveCommitCurrent(AsyncPipelineState::Generation generation) const noexcept;
    void ResetEngine(bool prepareResumeWarmStart = false) noexcept;
    void ResetEngineGenerationState(
        bool preserveOutputSequence, AsyncPipelineState::Generation expectedFallbackGeneration) noexcept;
    bool BeginDeferredSeekEngineReset(
        AsyncPipelineState::Generation& expectedGeneration,
        bool awaitNewSegment = false) noexcept;
    bool CompleteDeferredSeekEngineReset(AsyncPipelineState::Generation expectedGeneration = 0) noexcept;
    HRESULT DeliverDownstreamTracked(IMediaSample* sample, bool allowPausedControlSample = true) noexcept;
    void SetEvaluationEnabled(bool enabled, const char* reason) noexcept;
    uint64_t InterruptPendingSourceRequests(const char* reason) noexcept;
    bool StartDeliveryWorker(std::string& error) noexcept;
    void StopDeliveryWorker(const char* reason) noexcept;
    void DeliveryWorkerMain() noexcept;
    bool StartProcessingWorker(std::string& error) noexcept;
    void StopProcessingWorker(const char* reason) noexcept;
    void NotifyProcessingWorker() noexcept;
    bool StartSourceBootstrapWorker(std::string& error) noexcept;
    void StopSourceBootstrapWorker(const char* reason) noexcept;
    void NotifySourceBootstrapWorker() noexcept;
    void SourceBootstrapWorkerMain() noexcept;
    size_t NormalizedSourceFrameBytes() const noexcept;
    bool SuspendProcessingWorkerForEngineReset() noexcept;
    void ResumeProcessingWorkerAfterEngineReset() noexcept;
    void ProcessingWorkerMain() noexcept;
    HRESULT ProcessAvailableFrames(
        AsyncPipelineState::Generation generation,
        bool terminalDrain,
        bool& continuationRequired);
    HRESULT RequestProcessingEndOfStream(AsyncPipelineState::Generation generation) noexcept;
    HRESULT QueueInterpolatedForDelivery(ActiveFrucEngine::OutputFrame& frame, bool prerollSample,
        AsyncPipelineState::Generation generation, bool awaitDelivery = false);
    HRESULT QueueDeliveryDrainBarrier(AsyncPipelineState::Generation generation);
    HRESULT QueueSeekFallbackForDelivery(const std::vector<uint8_t>& nv12,
        int64_t sourceFrameIndex, bool sourcePreroll,
        AsyncPipelineState::Generation generation) noexcept;
    HRESULT QueueSeekFallbackForDelivery(
        const std::shared_ptr<const std::vector<uint8_t>>& sharedNv12,
        int64_t sourceFrameIndex, bool sourcePreroll,
        AsyncPipelineState::Generation generation) noexcept;
    HRESULT QueueSeekFallbackPayloadForDelivery(
        const uint8_t* nv12Data, size_t nv12Bytes,
        const std::shared_ptr<const std::vector<uint8_t>>& sharedNv12,
        int64_t sourceFrameIndex, bool sourcePreroll,
        AsyncPipelineState::Generation generation) noexcept;
    void ArmSeekFallback(const char* reason, AsyncPipelineState::Generation generation) noexcept;
    void ClearSeekFallback(AsyncPipelineState::Generation expectedGeneration = 0) noexcept;
    void ResetSeekFallbackProgress(AsyncPipelineState::Generation expectedGeneration) noexcept;
    // Delivery progress is computed from the immutable output time base captured with
    // the frame, not by dereferencing the engine from a second worker thread.
    void RecordOutputProgress(const ActiveFrucEngine::OutputFrame& frame,
        FrucRational outputTimeBase) noexcept;
    void AuditDownstreamRenderer(long long deliveredFrames) noexcept;
    void CaptureInterpolatedSampleFlags(IMediaSample* sample) noexcept;
    // Synthesized output delivery runs on DeliveryWorkerMain. Paused-frame and
    // pass-through/control samples retain their synchronous Receive-thread path.
    HRESULT DeliverInterpolated(AsyncOutputFrame& packet, int64_t outputPts);
    HRESULT ApplyAllocatorMediaType(IMediaSample* output, const char* site,
                                    bool logWhenUnchanged, bool* hasDynamicOutputType);
    HRESULT DeliverOriginalSample(IMediaSample* input, const char* reason,
                                  PauseFrameGate::Epoch expectedPauseEpoch = 0);
    bool IsPauseFrameRequestCurrent(PauseFrameGate::Epoch epoch) const noexcept;
    bool WaitForDeliveryAdmission(AsyncPipelineState::Generation generation,
                                  bool terminalDrain) noexcept;
    void NotifyDeliveryWorker() noexcept;
    HRESULT ApplyDynamicInputSampleType(IMediaSample* sample, const char* site);
    bool ConvertInputToNv12(const BYTE* input, std::vector<uint8_t>& nv12, std::string& error);
    bool ConvertNv12ToOutput(const uint8_t* nv12, BYTE* output, size_t outputBytes, std::string& error);
    void FailRuntime(const char* where, const std::string& reason, HRESULT failureHr = E_FAIL) noexcept;
    HRESULT UpdateOutputConnectionType();
    void LoadSettings();
    HRESULT SaveSettings() const;
    void LogInfo(const char* where, const std::string& message) const;
    void LogError(const char* where, const std::string& message) const;
    void RefreshTrayRuntimeInfo() const noexcept;
    void StartDiagnosticsWorker() noexcept;
    void StopDiagnosticsWorker() noexcept;
    void DiagnosticsWorkerMain() noexcept;
    void RefreshAsyncPipelineTrayInfo(const std::wstring& stallStatus) const noexcept;
    void LogAsyncPipelineTelemetry() const noexcept;
    bool IsGraphTransitioning() const noexcept {
        return m_stopRequested.load(std::memory_order_acquire) || m_flushTransition.Active() || m_segmentTransition.Active();
    }
    bool IsPauseTransitionPending() const noexcept { return m_pauseTransition.load(); }

    std::atomic<bool> m_stopRequested{false};
    DirectMotionLifecycle::TransitionGate m_flushTransition;
    // Some hosts decommit the downstream allocator during a Run -> Pause transition.
    // That is a delivery-state transition, not evidence that MVTools failed.
    std::atomic<bool> m_pauseTransition{false};
    // Distinguish the active Run-to-Pause callback from the stable paused state.
    // Stable pause must still allow a new synchronous preroll after seek.
    std::atomic<bool> m_pauseCallInProgress{false};
    // Run/preroll coordination: output generation is enabled only after Run starts.
    std::atomic<bool> m_runActive{false};
    // A graph entering Paused from Stopped must ingest enough decoded frames to
    // complete the AviSynth/MVTools bootstrap preroll. This is distinct from a
    // user-paused frame step and is cleared by Run/Stop; strict pause hold wins.
    std::atomic<bool> m_initialPrerollActive{false};
    // Running-to-Paused keeps the last delivered image on the renderer. The next
    // media Receive is held upstream until Run; unlike paused-frame stepping this
    // does not present a newer decoded source sample. Cleared by Run/flush/seek.
    std::atomic<bool> m_pauseHoldActive{false};
    // Monotonic strict-pause epoch lets workers recognize a packet/GetFrame interrupted
    // by Pause even if the thread is rescheduled only after Run has already succeeded.
    std::atomic<uint64_t> m_strictPauseEpoch{0};
    // Run marks the next rebuilt graph as a resume warm-start. Its processing worker
    // begins Prefetch after MVTools' minimum source lookahead rather than waiting for
    // the entire Prefetch read-ahead window to accumulate in Receive first.
    std::atomic<bool> m_resumeWarmStartPending{false};
    // NewSegment can race Receive; announce the transition before acquiring the receive
    // lock so the synchronous delivery path can unwind.
    DirectMotionLifecycle::TransitionGate m_segmentTransition;
    // Correlate DirectShow BeginFlush/EndFlush callbacks where the host pairs them.
    std::atomic<unsigned long long> m_currentFlushId{0};
    // Guarded by m_csReceive. A flush may be followed by Receive before NewSegment;
    // those samples can be staged, but the bootstrap worker must not consume them
    // until NewSegment rebinds reset ownership to the final segment generation.
    DirectMotionSeekReset::State m_seekResetState;
    bool m_firstSampleAfterSegment = false;
    // While the graph is paused, a non-preroll input is the actual frame requested
    // by frame-step (or the frame that completes initial Pause). Present it directly
    // instead of waiting for MVTools' future-frame lookahead. The graph is reset after
    // that presentation so the next Run starts a clean interpolation generation.
    PauseFrameGate m_pauseFrameGate; // one visible source frame per paused lifecycle epoch
    // After that frame is visible, the input pin holds the next source Receive
    // outside m_csReceive until Run, or cancels it on Stop/Flush/seek/failure.
    PauseReceiveBackpressure m_pauseReceiveBackpressure;

    // Negotiation methods are called repeatedly by DirectShow. Log accepted types and
    // candidate details once per filter instance; rejection/failure messages remain visible.
    std::atomic<bool> m_loggedAcceptedInputType{ false };
    std::atomic<bool> m_loggedAcceptedTransform{ false };
    std::atomic<unsigned> m_loggedMediaTypeCandidateMask{ 0 };

    DirectMotionSettings m_settings;
    std::shared_ptr<AviSynthEvaluationGate> m_sourceEvaluationGate = std::make_shared<AviSynthEvaluationGate>();
    // Declared before m_engine so reverse member destruction releases every graph
    // and PClip wrapper before the persistent AviSynth environment is destroyed.
    std::shared_ptr<AviSynthRuntimeSession> m_avisynthRuntimeSession =
        std::make_shared<AviSynthRuntimeSession>();
    std::unique_ptr<ActiveFrucEngine> m_engine;
    // Post-seek Receive samples are staged here while the replacement graph is
    // destroyed/rebuilt. The bootstrap worker publishes them to AviSynth in order.
    // Candidate graph construction must not hold m_csReceive, so decoded source
    // samples can continue to enter this bounded queue and the fallback output lane.
    DirectMotionSourceBootstrap::Queue m_sourceBootstrapQueue;
    std::thread m_sourceBootstrapWorkerThread;
    std::mutex m_sourceBootstrapWorkerLifecycleMutex;
    std::atomic<bool> m_sourceBootstrapWorkerStop{ true };
    std::atomic<bool> m_sourceBootstrapWorkerStarted{ false };
    // Generation to which the current AviSynth graph belongs. ResetEngine advances the
    // transport generation only when it still matches this value, avoiding duplicate
    // invalidations after BeginFlush/NewSegment have already retired the old generation.
    std::atomic<AsyncPipelineState::Generation> m_engineGeneration{0};
    AsyncPipelineState m_asyncPipeline;
    // During post-seek warm-up, Receive publishes source-frame fallback packets into
    // the same ordered output FIFO. The mutex serializes fallback enqueue with the
    // processing worker's PTS handoff decision.
    std::mutex m_seekFallbackMutex;
    bool m_seekFallbackActive = false;
    // The mutex serializes fallback fields, while this owner fence prevents a
    // retired ProcessingWorker pass from mutating a fallback armed by a newer seek.
    AsyncPipelineState::Generation m_seekFallbackGeneration = 0;
    // Published mirror used only to skip fallback classification while inactive.
    // The mutex remains authoritative for the compound fallback state below.
    std::atomic<bool> m_seekFallbackActivePublished{ false };
    bool m_seekFallbackAcceptingSource = false;
    bool m_seekFallbackHasQueuedFrame = false;
    int64_t m_seekFallbackLastQueuedPts = -1;
    // Keep only the newest successfully queued source fallback image. At PTS
    // handoff it is transferred to a generation-owned, six-frame NV12 blend.
    std::shared_ptr<const std::vector<uint8_t>> m_seekFallbackLastSourceFrame;
    std::shared_ptr<const std::vector<uint8_t>> m_seekTransitionSourceFrame;
    DirectMotionSeekTransition::State m_seekTransitionState;
    // Lock-free steady-state gate: the fallback mutex is acquired only while
    // fallback or the short visual transition is actually active.
    std::atomic<bool> m_seekTransitionActivePublished{ false };
    std::atomic<uint64_t> m_seekFallbackArmedTickNs{ 0 };
    std::atomic<bool> m_seekTraceFirstReceiveLogged{ false };
    std::atomic<bool> m_seekTraceFirstVisibleLogged{ false };
    std::atomic<uint32_t> m_seekTracePrerollSamples{ 0 };
    uint64_t m_seekFallbackQueuedPackets = 0;
    uint64_t m_seekFallbackQueuedFrames = 0;
    uint64_t m_seekFallbackDroppedPackets = 0;
    std::thread m_deliveryWorkerThread;
    std::mutex m_deliveryWorkerLifecycleMutex;
    std::atomic<bool> m_deliveryWorkerStop{false};
    // Normal Running-to-Paused retains queued output and the AviSynth source history.
    // DeliveryWorkerMain parks its current packet here until Run or lifecycle invalidation.
    std::mutex m_deliveryPauseMutex;
    std::condition_variable m_deliveryPauseChanged;
    std::thread m_processingWorkerThread;
    std::thread m_diagnosticsWorkerThread;
    std::mutex m_diagnosticsWorkerLifecycleMutex;
    std::atomic<bool> m_diagnosticsWorkerStop{true};
    mutable std::atomic<ULONGLONG> m_lastAsyncTrayInfoRefreshTick{0};
    // Avoid racing std::thread::joinable() reads against StopProcessingWorker's join.
    std::atomic<bool> m_processingWorkerStarted{false};
    std::mutex m_processingWorkerMutex;
    std::condition_variable m_processingWorkerChanged;
    bool m_processingWorkerStop = false;
    bool m_processingWorkerSuspend = false;
    bool m_processingWorkPending = false;
    bool m_processingWorkerActive = false;
    bool m_processingEosRequested = false;
    bool m_processingEosCompleted = false;
    HRESULT m_processingEosResult = S_OK;
    std::mutex m_processingWorkerLifecycleMutex;
    std::atomic<bool> m_processingNeedsReset{false};

    int m_width = 0;
    int m_height = 0;
    int m_inputStride = 0;           // physical byte pitch of each NV12/NV21 plane row
    int m_inputSurfaceHeight = 0;    // physical luma surface height; UV plane begins after this many rows
    int m_inputCropLeft = 0;         // visible rcSource origin inside the physical surface
    int m_inputCropTop = 0;
    int m_chromaStride = 0;
    int m_inputLayout = 0; // 8-bit semi-planar NV12 or NV21
    bool m_inputBottomUp = false;
    size_t m_inputBufferBytes = 0;
    int m_outputStride = 0;             // physical byte pitch of each NV12/NV21 output row
    int m_outputSurfaceHeight = 0;      // physical luma height; UV plane starts after these rows
    int m_outputTargetLeft = 0;         // visible picture placement inside renderer surface
    int m_outputTargetTop = 0;
    size_t m_outputBufferBytes = 0;
    FrucRational m_sourceRate{24, 1};
    FrucRational m_targetRate{48, 1};
    // Resolved during GetMediaType before DirectShow negotiates the output cadence.
    // Auto uses the exact rational rate of the monitor hosting the EVR video window.
    FrucRational m_autoDisplayRate{0, 1};
    std::string m_autoDisplayRateSource;
    REFERENCE_TIME m_targetFrameDuration = 208333;
    FrucRational m_outputTimeBase{1, 48};
    mutable std::mutex m_outputTimelineMutex;

    int64_t m_nextInputPts = 0;
    REFERENCE_TIME m_streamTimeOrigin = 0;
    bool m_streamTimeOriginSet = false;
    // Most recently announced DirectShow segment; reused if a local recovery flush is needed.
    REFERENCE_TIME m_currentSegmentStart = 0;
    REFERENCE_TIME m_currentSegmentStop = 0;
    double m_currentSegmentRate = 1.0;
    bool m_currentSegmentMetadataValid = false;
    // Protected by m_csReceive. True only for the graph built by the current
    // StartStreaming call, until either its first NewSegment adopts it or a
    // source sample/other lifecycle transition consumes the pristine window.
    std::atomic<bool> m_preparedInitialGraphAwaitingSegment{ false };
    // Source timestamp continuity detector. It is reset with every engine/cache generation.
    REFERENCE_TIME m_lastInputSampleStart = 0;
    REFERENCE_TIME m_lastInputSampleEnd = 0;
    bool m_lastInputTimestampValid = false;
    bool m_lastInputSampleStopTimeValid = false;
    // The first non-preroll source boundary for this graph generation. A valid stream-
    // time is authoritative; the mapped output-frame index is the fallback for an
    // untimed first visible sample. Neither is copied from a Prefetch trigger sample.
    bool m_prerollBoundaryInitialized = false;
    REFERENCE_TIME m_prerollBoundaryTime = 0;
    bool m_prerollBoundaryTimeValid = false;
    int64_t m_prerollBoundaryOutputPts = 0;
    bool m_prerollBoundaryOutputPtsValid = false;
    // Last accepted output end time, used only to preserve output timeline continuity
    // when a timestamp jump arrives without DirectShow's NewSegment callback.
    std::atomic<REFERENCE_TIME> m_lastDeliveredOutputEnd{0};
    std::atomic<bool> m_lastDeliveredOutputTimeValid{false};
    std::atomic<bool> m_firstOutput{true};
    bool m_firstInputBmpAttempted = false; // Reset only at StartStreaming, not on seeks/discontinuities.
    bool m_streamInitialized = false;
    std::atomic<bool> m_runtimeFailed{false};
    std::atomic<int64_t> m_inputFrameCount{0};
    std::atomic<int64_t> m_outputFrameCount{0};
    // Diagnostic-only cumulative counters for this streaming session. They do not
    // participate in graph scheduling, timestamps, queue predicates or frame indices.
    std::atomic<uint64_t> m_diagInputReceivedFrames{0};
    std::atomic<uint64_t> m_diagInputNormalizedFrames{0};
    std::atomic<uint64_t> m_diagInputPublishedFrames{0};
    std::atomic<uint64_t> m_diagOutputGeneratedFrames{0};
    std::atomic<uint64_t> m_diagOutputDeliveredFrames{0};
    std::atomic<bool> m_diagProcessingPassCapped{false};
    std::atomic<int64_t> m_passthroughFrameCount{0};
    std::atomic<bool> m_inputInterlacedTwoFieldsPerSample{false};
    std::atomic<DWORD> m_interpolatedTypeSpecificFlags{0};
    // DIRECTMOTION_VERBOSE_TRACE is intentionally bounded: only the first few source and
    // output samples after a segment transition are fingerprinted, for at most 16 segments.
    std::atomic<unsigned> m_stepAuditSegmentCount{0};
    std::atomic<int> m_stepAuditInputSamplesRemaining{0};
    std::atomic<int> m_stepAuditOutputSamplesRemaining{0};
    static constexpr unsigned kStepAuditMaxSegments = 16;
    static constexpr int kStepAuditInputSamplesPerSegment = 3;
    static constexpr int kStepAuditOutputSamplesPerSegment = 4;
    std::atomic<double> m_lastPushMs{-1.0};
    std::atomic<double> m_avgPushMs{-1.0};
    std::atomic<uint64_t> m_pushTimingSamples{0};
    std::atomic<double> m_lastGraphMs{-1.0};
    std::atomic<double> m_avgGraphMs{-1.0};
    std::atomic<uint64_t> m_graphTimingSamples{0};
    std::atomic<double> m_lastAllocatorWaitMs{-1.0};
    std::atomic<double> m_lastDownstreamMs{-1.0};
    std::atomic<double> m_avgDownstreamMs{-1.0};
    std::atomic<uint64_t> m_downstreamTimingSamples{0};
    // Diagnostic-only conversion/reset timings. These counters are sampled only
    // while file logging is enabled and are reported by the existing 5 s telemetry.
    // They never participate in scheduling, queue predicates, timestamps or frames.
    std::atomic<uint64_t> m_diagInputNormalizeSamples{0};
    std::atomic<uint64_t> m_diagInputNormalizeTotalNs{0};
    std::atomic<uint64_t> m_diagInputNormalizeLastNs{0};
    std::atomic<uint64_t> m_diagInputNormalizeMaxNs{0};
    std::atomic<uint64_t> m_diagOutputCopySamples{0};
    std::atomic<uint64_t> m_diagOutputCopyTotalNs{0};
    std::atomic<uint64_t> m_diagOutputCopyLastNs{0};
    std::atomic<uint64_t> m_diagOutputCopyMaxNs{0};
    std::atomic<uint64_t> m_diagDeferredResetSamples{0};
    std::atomic<uint64_t> m_diagDeferredResetTotalNs{0};
    std::atomic<uint64_t> m_diagDeferredResetLastNs{0};
    std::atomic<uint64_t> m_diagDeferredResetMaxNs{0};
    std::atomic<uint64_t> m_diagDeferredQuiescenceSamples{0};
    std::atomic<uint64_t> m_diagDeferredQuiescenceTotalNs{0};
    std::atomic<uint64_t> m_diagDeferredQuiescenceLastNs{0};
    std::atomic<uint64_t> m_diagDeferredQuiescenceMaxNs{0};
    std::atomic<uint64_t> m_diagDeferredDestroySamples{0};
    std::atomic<uint64_t> m_diagDeferredDestroyTotalNs{0};
    std::atomic<uint64_t> m_diagDeferredDestroyLastNs{0};
    std::atomic<uint64_t> m_diagDeferredDestroyMaxNs{0};
    std::atomic<int64_t> m_lastOutputSourceIndex{-1};
    std::atomic<int64_t> m_outputPacketSequence{0};
    mutable std::atomic<ULONGLONG> m_lastAsyncTelemetryLogTick{0};
    std::recursive_mutex m_deliveryMutex; // Serialize delivery and generation-transition fences.
    BootstrapQueueState m_bootstrapQueueState; // True after FIFO enqueue, not downstream delivery.
};
