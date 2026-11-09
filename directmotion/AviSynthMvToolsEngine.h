#pragma once

#include <Windows.h>
#include <avisynth.h>
#include "FrucRational.h"
#include "SourceCacheRetention.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

constexpr int kCpuProfileAuto = 0;
constexpr int kCpuProfilePhysical = 1;
constexpr int kCpuProfileHalfLogical = 2;
constexpr int kCpuProfileCustom = 3;

class AviSynthEvaluationGate final {
public:
    struct Snapshot {
        bool enabled = true;
        bool changed = false;
        uint64_t blockedRequests = 0;
        uint32_t waitingRequests = 0;
    };

    Snapshot SetEnabled(bool value) noexcept {
        Snapshot snapshot;
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot.changed = (m_enabled != value);
                m_enabled = value;
                snapshot.enabled = m_enabled;
                snapshot.blockedRequests = m_blockedRequests;
                snapshot.waitingRequests = m_waitingRequests;
            }
            m_changed.notify_all();
        } catch (...) {

            try { m_changed.notify_all(); } catch (...) {}
        }
        return snapshot;
    }

    void WaitUntilEnabled() noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_enabled) {
                ++m_blockedRequests;
                ++m_waitingRequests;
                m_changed.wait(lock, [this]() { return m_enabled; });
                --m_waitingRequests;
            }
        } catch (...) {
            SetEnabled(true);
        }
    }

    uint64_t SourceInterruptEpoch() const noexcept {
        return m_sourceInterruptEpoch.load(std::memory_order_acquire);
    }

    std::mutex& SourceWaitMutex() noexcept { return m_sourceWaitMutex; }
    std::condition_variable& SourceChangedCondition() noexcept { return m_sourceChanged; }

    void NotifySourceChanged() noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_sourceWaitMutex);
            m_sourceChanged.notify_all();
        } catch (...) {
            m_sourceChanged.notify_all();
        }
    }

    uint64_t InterruptSourceRequests() noexcept {
        uint64_t epoch = 0;
        try {
            {
                std::lock_guard<std::mutex> lock(m_sourceWaitMutex);
                epoch = m_sourceInterruptEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
                m_sourceChanged.notify_all();
            }
        } catch (...) {
            epoch = m_sourceInterruptEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_sourceChanged.notify_all();
        }
        (void)SetEnabled(true);
        return epoch;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_enabled = true;
    uint64_t m_blockedRequests = 0;
    uint32_t m_waitingRequests = 0;
    std::mutex m_sourceWaitMutex;
    std::condition_variable m_sourceChanged;
    std::atomic<uint64_t> m_sourceInterruptEpoch{0};
};

class AviSynthRuntimeSession final {
public:
    AviSynthRuntimeSession() = default;
    ~AviSynthRuntimeSession();
    AviSynthRuntimeSession(const AviSynthRuntimeSession&) = delete;
    AviSynthRuntimeSession& operator=(const AviSynthRuntimeSession&) = delete;

    bool EnsureReady(std::string& error, bool* createdNow = nullptr);
    IScriptEnvironment* Environment() const noexcept { return m_env; }

private:
    void DestroyEnvironment() noexcept;

    mutable std::mutex m_mutex;
    HMODULE m_avisynthModule = nullptr;
    int m_avisynthInterfaceVersion = 0;
    IScriptEnvironment* m_env = nullptr;
    bool m_ready = false;
};

struct AviSynthMvToolsOptions {
    bool advanced = false;
    int superPel = 1;
    int superSharp = 2;
    int superRFilter = 2;
    int superLevels = 0;
    int analyseBlockSize = 32;
    int analyseSearch = 3;
    int analyseSearchParam = 3;
    int analyseBadRange = -24;
    int analyseOverlap = 0;
    bool analyseChroma = false;
    bool analyseTrueMotion = true;
    int recalcBlockSize = 8;
    int recalcSearch = 3;
    int recalcSearchParam = 0;
    int recalcOverlap = 0;
    int blockFpsMode = 2;
    double blockFpsMaskScale = 100.0;
    bool blockFpsBlend = true;
    int thSCD1 = 400;
    int thSCD2 = 130;
};

class AviSynthMvToolsEngine final {
public:
    using SourceCachePressureCallback = void (*)(void*) noexcept;
    struct OutputFrame {
        int64_t pts = 0;
        std::vector<uint8_t> nv12;
        int outputLineSizeY = 0;
        int outputLineSizeUV = 0;
    };

    AviSynthMvToolsEngine() = default;
    ~AviSynthMvToolsEngine();
    AviSynthMvToolsEngine(const AviSynthMvToolsEngine&) = delete;
    AviSynthMvToolsEngine& operator=(const AviSynthMvToolsEngine&) = delete;

    bool Initialize(int width, int height, FrucRational sourceRate, FrucRational targetRate,
                    int performance, int grid, int cpuProfile, int customCpuWorkers,
                    int quickRecalcMode, int requestedPrefetchFrames,
                    const AviSynthMvToolsOptions& options,
                    bool resumeWarmStart,
                    const std::shared_ptr<AviSynthRuntimeSession>& runtimeSession,
                    const std::shared_ptr<AviSynthEvaluationGate>& evaluationGate,
                    std::string& error);

    DirectMotionSourceCache::PushResult PushSourceNv12(
        const uint8_t* data, int sourceStride, int64_t inputPts,
        std::string& error, void* pressureContext = nullptr,
        SourceCachePressureCallback pressureCallback = nullptr);

    bool TryRebindInitialSourceGeneration(
        uint64_t expectedPreviousEpoch, uint64_t newSourceInterruptEpoch,
        bool pipelineGenerationMatches, bool noFilterInputPublished,
        bool noFilterOutputEmitted, std::string& error);

    bool CanEmitOutputIndex(int64_t outputIndex) const noexcept;

    bool PrimeSourceCacheForOutputIndex(int64_t outputIndex, std::string& error);
    bool EmitBootstrapFrame(int64_t startOutputIndex, OutputFrame& output, std::string& error);
    bool EmitNextAvailable(OutputFrame& output, bool& available, bool& finished, std::string& error);
    bool FinalizeSource(std::string& error);
    void InterruptSourceWaits() noexcept;
    int64_t InputFrameCount() const noexcept { return m_inputFrameCount.load(std::memory_order_acquire); }
    bool IsSourceFinalized() const noexcept { return m_flushed.load(std::memory_order_acquire); }
    int64_t ExpectedOutputCount() const noexcept { return m_expectedOutputCount.load(std::memory_order_acquire); }
    void Reset() noexcept;

    FrucRational OutputTimeBase() const noexcept { return m_outputTimeBase; }

    int BootstrapInputFramesRequired() const noexcept { return m_bootstrapInputFramesRequired; }
    bool IsInitialized() const noexcept { return m_initialized && m_env && m_rawFlowClip != nullptr; }

private:
    class InputClip;
    bool BuildGraph(const std::shared_ptr<AviSynthEvaluationGate>& evaluationGate, std::string& error);
    bool BuildPrefetchFromOutputStart(int64_t startOutputIndex, std::string& error);
    bool EmitFrame(int outputIndex, std::vector<OutputFrame>& output, std::string& error,
                   bool forceRawMBlockFps = false);
    static std::wstring ResolveRuntimePath();
    static std::wstring ResolveMvToolsPath();
    static std::wstring GetModuleDirectory();
    static std::string WideToUtf8(const std::wstring& value);
    static std::string AvsErrorText(const AvisynthError& error);
    friend class AviSynthRuntimeSession;

    int m_width = 0;
    int m_height = 0;
    int m_performance = 2;
    int m_requestedGrid = 0;
    int m_cpuProfile = kCpuProfileAuto;
    int m_customCpuWorkers = 8;
    int m_quickRecalcMode = 0;
    int m_requestedPrefetchFrames = 0;
    int m_bootstrapInputFramesRequired = 3;
    int m_prefetchThreads = 1;
    int m_prefetchFrames = 1;
    int64_t m_outputStartIndex = 0;
    int64_t m_firstPrefetchOutputIndex = 1;
    int64_t m_sourceRetentionPrimedForOutputIndex = -1;
    bool m_bootstrapEmitted = false;
    bool m_resumeWarmStart = false;
    AviSynthMvToolsOptions m_options{};
    FrucRational m_sourceRate{24, 1};
    FrucRational m_targetRate{48, 1};
    FrucRational m_outputTimeBase{1, 48};

    std::shared_ptr<AviSynthRuntimeSession> m_runtimeSession;
    IScriptEnvironment* m_env = nullptr;

    std::unique_ptr<PClip> m_sourceClip;
    std::unique_ptr<PClip> m_superClip;
    std::unique_ptr<PClip> m_mvBackwardClip;
    std::unique_ptr<PClip> m_mvForwardClip;
    std::unique_ptr<PClip> m_rawFlowClip;
    std::unique_ptr<PClip> m_flowClip;
    InputClip* m_input = nullptr;

    std::atomic<int64_t> m_inputFrameCount{0};
    int64_t m_nextOutputIndex = 0;
    std::atomic<int64_t> m_expectedOutputCount{0};
    bool m_initialized = false;
    std::atomic<bool> m_flushed{false};
};
