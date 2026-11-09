#include "AviSynthMvToolsEngine.h"
#include "ResumeWarmStart.h"
#include "BootstrapEvaluationPolicy.h"
#include "SeekFrontierPolicy.h"
#include "RuntimeDiagnostics.h"
#include "SourceCacheRetention.h"
#include "MAnalyseInvocationPolicy.h"
#include "InitialSourceGenerationRebindPolicy.h"

#include <Windows.h>
#include <emmintrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <iterator>
#include <new>
#include <exception>
#include <mutex>
#include <numeric>
#include <sstream>
#include <set>
#include <shared_mutex>
#include <utility>
const AVS_Linkage* AVS_linkage = nullptr;

namespace {
constexpr int kSyntheticClipFrameCount = 10000000;
constexpr int kAviSynthCacheInformNumThreadsHintV12 = 514;
constexpr size_t kBaseSourceCacheBudgetBytes = 768u * 1024u * 1024u;
constexpr size_t kMinWorkingSetBudgetBytes = 1024u * 1024u * 1024u;
constexpr size_t kMaxWorkingSetBudgetBytes = 6u * 1024u * 1024u * 1024u;
constexpr size_t kMinSourceCacheBudgetBytes = 384u * 1024u * 1024u;
constexpr size_t kMaxSourceCacheBudgetBytes = 2u * 1024u * 1024u * 1024u;
constexpr size_t kSourceCacheHardCapFrames = 128;
constexpr uint64_t kReferenceFramePixels = 1280ull * 720ull;
constexpr size_t kSourceCacheSafetyFrames = 4;
constexpr size_t kSourceCacheHistoryMarginFrames = 24;
constexpr int kAviSynthMemoryMaxMiB = 4608;
std::mutex g_avisynthRuntimeMutex;
HMODULE g_avisynthRuntimeModule = nullptr;
std::wstring g_avisynthRuntimePath;
HMODULE g_mvtoolsRuntimeModule = nullptr;
std::wstring g_mvtoolsRuntimePath;
constexpr int kMotionLookaheadFrames = 2;
constexpr int kPreferredAviSynthInterfaceVersion = 12;
constexpr int kFallbackAviSynthInterfaceVersion = 8;
std::atomic<uint64_t> g_sourceCacheGeneration{0};

using CreateScriptEnvironmentFn = IScriptEnvironment* (__stdcall*)(int version);

bool IsValidRate(FrucRational rate) {
    return rate.num > 0 && rate.den > 0;
}

bool TraceFrameRequestsEnabled() noexcept {
    static const bool enabled = []() noexcept {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"DIRECTMOTION_FULL_TRACE", value,
            static_cast<DWORD>(_countof(value)));
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool AviSynthInternalLoggingEnabled() noexcept {
    static const bool enabled = []() noexcept {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"DIRECTMOTION_AVISYNTH_LOG", value,
            static_cast<DWORD>(_countof(value)));
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool AviSynthGraphAnalysisEnabled() noexcept {
    static const bool enabled = []() noexcept {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"DIRECTMOTION_AVISYNTH_GRAPH", value,
            static_cast<DWORD>(_countof(value)));
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}

void CopyPlane(BYTE* dst, int dstPitch, const uint8_t* src,
               int srcPitch, int rowBytes, int rows) {
    for (int y = 0; y < rows; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dstPitch,
                    src + static_cast<size_t>(y) * srcPitch,
                    static_cast<size_t>(rowBytes));
}

void InterleaveChromaRow(uint8_t* dstUv, const uint8_t* srcU,
                         const uint8_t* srcV, int chromaWidth) noexcept {
    int x = 0;
    for (; x + 16 <= chromaWidth; x += 16) {
        const __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcU + x));
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcV + x));
        const __m128i uvLo = _mm_unpacklo_epi8(u, v);
        const __m128i uvHi = _mm_unpackhi_epi8(u, v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dstUv + 2 * x), uvLo);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dstUv + 2 * x + 16), uvHi);
    }
    for (; x < chromaWidth; ++x) {
        dstUv[2 * x] = srcU[x];
        dstUv[2 * x + 1] = srcV[x];
    }
}

void DeinterleaveNv12ChromaRow(uint8_t* dstU, uint8_t* dstV,
                               const uint8_t* srcUv, int chromaWidth) noexcept {
    const __m128i evenMask = _mm_set1_epi16(0x00FF);
    int x = 0;
    for (; x + 16 <= chromaWidth; x += 16) {
        const __m128i uv0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcUv + 2 * x));
        const __m128i uv1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcUv + 2 * x + 16));
        const __m128i u0 = _mm_and_si128(uv0, evenMask);
        const __m128i u1 = _mm_and_si128(uv1, evenMask);
        const __m128i v0 = _mm_srli_epi16(uv0, 8);
        const __m128i v1 = _mm_srli_epi16(uv1, 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dstU + x), _mm_packus_epi16(u0, u1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dstV + x), _mm_packus_epi16(v0, v1));
    }
    for (; x < chromaWidth; ++x) {
        dstU[x] = srcUv[2 * x];
        dstV[x] = srcUv[2 * x + 1];
    }
}

bool GetLoadedModulePath(HMODULE module, std::wstring& path) noexcept {
    path.clear();
    if (!module) return false;
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return false;
    buffer.resize(length);
    path = std::move(buffer);
    return true;
}

bool ModuleMatchesRequestedPath(HMODULE module, const std::wstring& expected,
                                std::wstring& actual) noexcept {
    if (!GetLoadedModulePath(module, actual)) return false;
    return CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

DWORD QueryLogicalProcessorCount() noexcept {
    const DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active != 0) return active;
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    return (std::max)(static_cast<DWORD>(1), systemInfo.dwNumberOfProcessors);
}

bool QueryPhysicalCoreCount(DWORD& physicalCores) noexcept {
    physicalCores = 0;
    DWORD bytes = 0;
    (void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return false;
    try {
        std::vector<BYTE> storage(bytes);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()), &bytes))
            return false;
        DWORD offset = 0;
        while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                storage.data() + offset);
            if (entry->Size < sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ||
                entry->Size > bytes - offset) return false;
            if (entry->Relationship == RelationProcessorCore) ++physicalCores;
            offset += entry->Size;
        }
        return offset == bytes && physicalCores != 0;
    } catch (...) {
        physicalCores = 0;
        return false;
    }
}

const char* CpuProfileName(int profile) noexcept {
    switch (profile) {
    case kCpuProfilePhysical: return "PhysicalCores";
    case kCpuProfileHalfLogical: return "HalfLogical/HTEstimate";
    case kCpuProfileCustom: return "Custom";
    default: return "Auto/Logical";
    }
}
} // namespace

class AviSynthMvToolsEngine::InputClip final : public IClip {
public:
    InputClip(int width, int height, FrucRational fps, size_t cacheCapacity,
              size_t maxRetainedFrames, size_t historySlackFrames, uint64_t generation,
              std::shared_ptr<AviSynthEvaluationGate> evaluationGate)
        : m_width(width), m_height(height),
          m_cacheCapacity((std::max)(cacheCapacity, static_cast<size_t>(kMotionLookaheadFrames + 2))),
          m_maxRetainedFrames((std::max)(m_cacheCapacity, maxRetainedFrames)),
          m_historySlackFrames(historySlackFrames),
          m_generation(generation), m_evaluationGate(std::move(evaluationGate)),
          m_sourceInterruptEpoch(m_evaluationGate ? m_evaluationGate->SourceInterruptEpoch() : 0) {
        std::memset(&m_vi, 0, sizeof(m_vi));
        m_vi.width = width;
        m_vi.height = height;
        m_vi.pixel_type = VideoInfo::CS_YV12;
        m_vi.fps_numerator = static_cast<unsigned>(fps.num);
        m_vi.fps_denominator = static_cast<unsigned>(fps.den);
        m_vi.num_frames = kSyntheticClipFrameCount;
    }

    DirectMotionSourceCache::PushResult PushNv12(
        const uint8_t* data, int stride, IScriptEnvironment* env, std::string& error,
        void* pressureContext, AviSynthMvToolsEngine::SourceCachePressureCallback pressureCallback) {
        if (!data || stride < m_width || !env) {
            error = "AviSynth input bridge received a null buffer, invalid NV12 stride, or null environment.";
            return DirectMotionSourceCache::PushResult::Failed;
        }
        try {
            PVideoFrame planar = env->NewVideoFrame(m_vi);
            const uint8_t* srcY = data;
            const uint8_t* srcUV = data + static_cast<size_t>(stride) * m_height;
            CopyPlane(planar->GetWritePtr(PLANAR_Y), planar->GetPitch(PLANAR_Y),
                      srcY, stride, m_width, m_height);
            BYTE* dstU = planar->GetWritePtr(PLANAR_U);
            BYTE* dstV = planar->GetWritePtr(PLANAR_V);
            const int chromaWidth = m_width / 2;
            for (int y = 0; y < m_height / 2; ++y) {
                const uint8_t* row = srcUV + static_cast<size_t>(y) * stride;
                uint8_t* u = dstU + static_cast<size_t>(y) * planar->GetPitch(PLANAR_U);
                uint8_t* v = dstV + static_cast<size_t>(y) * planar->GetPitch(PLANAR_V);
                DeinterleaveNv12ChromaRow(u, v, row, chromaWidth);
            }
            bool cacheWaitEntered = false;
            struct CacheProducerWaitFlagGuard {
                bool active = false;
                void Begin() noexcept {
                    if (!active) {
                        active = true;
                        DirectMotionTraySetSourceCacheProducerWaiting(true);
                    }
                }
                ~CacheProducerWaitFlagGuard() {
                    if (active) DirectMotionTraySetSourceCacheProducerWaiting(false);
                }
            } producerWaitFlag;
            const auto cacheWaitStarted = std::chrono::steady_clock::now();
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                for (;;) {
                    if (m_interrupted || IsSourceGenerationInterrupted()) {
                        error = "AviSynth input bridge was interrupted during a graph transition.";
                        return DirectMotionSourceCache::PushResult::Interrupted;
                    }
                    if (m_eos) {
                        error = "AviSynth input bridge received a frame after end-of-stream.";
                        return DirectMotionSourceCache::PushResult::Failed;
                    }

                    TrimLocked();
                    if (DirectMotionSourceCache::CanAppend(m_frames.size(), m_maxRetainedFrames)) {
                        m_frames.emplace_back(planar);
                        PublishCacheRangeLocked();
                        break;
                    }

                    if (!cacheWaitEntered) {
                        cacheWaitEntered = true;
                        producerWaitFlag.Begin();
                        DirectMotionLog("INFO", "AviSynth/SourceCache",
                            "WAIT_BEGIN producer backpressure: cache retained the oldest source history "
                            "still reachable by Prefetch; queued=" + std::to_string(m_frames.size()) +
                            ", normalCapacity=" + std::to_string(m_cacheCapacity) +
                            ", maxRetained=" + std::to_string(m_maxRetainedFrames) +
                            ", historySlackFrames=" + std::to_string(m_historySlackFrames) +
                            ", cacheBudgetFrames=" + std::to_string(m_maxRetainedFrames) +
                            ", cacheRange=" + std::to_string(m_baseIndex) + ".." +
                                std::to_string(m_baseIndex + static_cast<int64_t>(m_frames.size()) - 1) +
                            ", sourceRetentionAnchor=" + std::to_string(m_sourceRetentionAnchor) +
                            ", oldestActiveGetFrame=" + std::to_string(OldestActiveSourceRequest()) +
                            ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                            ", threadId=" + std::to_string(GetCurrentThreadId()));
                    }
                    lock.unlock();
                    if (pressureCallback) pressureCallback(pressureContext);
                    lock.lock();
                    m_cacheSpaceChanged.wait_for(lock, std::chrono::milliseconds(250));
                }
            }
            m_framesChanged.notify_all();
            if (m_evaluationGate) m_evaluationGate->NotifySourceChanged();
            PublishSourceWaitInfo();
            if (cacheWaitEntered) {
                const double waitMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - cacheWaitStarted).count();
                DirectMotionLog("INFO", "AviSynth/SourceCache",
                    "WAIT_END producer backpressure released: waitMs=" + std::to_string(waitMs) +
                    ", cacheRange=" + std::to_string(m_diagCacheBase.load(std::memory_order_acquire)) + ".." +
                        std::to_string(m_diagCacheEnd.load(std::memory_order_acquire)) +
                    ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                    ", threadId=" + std::to_string(GetCurrentThreadId()));
            }
            return DirectMotionSourceCache::PushResult::Published;
        } catch (const AvisynthError& ex) {
            error = std::string("AviSynth could not allocate/materialize a source frame: ") +
                (ex.msg ? ex.msg : "unknown AviSynth allocation error");
            return DirectMotionSourceCache::PushResult::Failed;
        } catch (const std::bad_alloc&) {
            error = "Out of memory while allocating the shared AviSynth YV12 source frame.";
            return DirectMotionSourceCache::PushResult::Failed;
        }
    }

    bool CompleteEndOfStream(std::string& error) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_frames.empty()) {
                error = "Cannot finalize the AviSynth bridge without an input frame.";
                return false;
            }
            if (m_interrupted) {
                error = "Cannot finalize an interrupted AviSynth input bridge.";
                return false;
            }
            m_eos = true;
        }
        m_framesChanged.notify_all();
        m_cacheSpaceChanged.notify_all();
        if (m_evaluationGate) m_evaluationGate->NotifySourceChanged();
        PublishSourceWaitInfo();
        return true;
    }

    void InterruptSourceWaits() noexcept {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_interrupted = true;
        }
        m_framesChanged.notify_all();
        m_cacheSpaceChanged.notify_all();
        if (m_evaluationGate) {
            const auto epoch = m_evaluationGate->InterruptSourceRequests();
            DirectMotionLog("INFO", "AviSynth/SourceWait",
                "Interrupted source requests for graph generation=" +
                std::to_string(m_generation.load(std::memory_order_acquire)) +
                ", sharedInterruptEpoch=" + std::to_string(epoch) + ".");
        }
    }

    bool TryRebindPristineInitialGeneration(
        uint64_t expectedPreviousEpoch, uint64_t newSourceInterruptEpoch,
        bool pipelineGenerationMatches, bool noFilterInputPublished,
        bool noFilterOutputEmitted, bool noEngineInputAccepted,
        bool noEngineOutputEmitted, std::string& error) {
        if (!m_evaluationGate) {
            error = "Initial source-generation rebinding requires the shared evaluation gate.";
            return false;
        }

        uint64_t previousCacheGeneration = 0;
        uint64_t nextCacheGeneration = 0;
        {
            std::unique_lock<std::shared_mutex> generationLock(m_generationMutex);
            std::unique_lock<std::mutex> sourceWaitLock(m_evaluationGate->SourceWaitMutex());
            std::lock_guard<std::mutex> cacheLock(m_mutex);
            std::lock_guard<std::mutex> waitersLock(m_waitersMutex);

            previousCacheGeneration = m_generation.load(std::memory_order_acquire);
            nextCacheGeneration = g_sourceCacheGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

            DirectMotionInitialSourceGenerationRebindPolicy::Snapshot state;
            state.graphInitialized = true;
            state.inputClipPresent = true;
            state.pipelineGenerationMatches = pipelineGenerationMatches;
            state.noFilterInputPublished = noFilterInputPublished;
            state.noFilterOutputEmitted = noFilterOutputEmitted;
            state.noEngineInputAccepted = noEngineInputAccepted;
            state.noEngineOutputEmitted = noEngineOutputEmitted;
            state.cacheEmpty = m_frames.empty() && m_baseIndex == 0 &&
                m_sourceRetentionAnchor == -1 &&
                m_diagCacheBase.load(std::memory_order_acquire) == 0 &&
                m_diagCacheEnd.load(std::memory_order_acquire) == -1;
            state.noActiveGetFrames = m_activeGetFrames.load(std::memory_order_acquire) == 0;
            state.noSourceWaiters = m_waitingSourceFrames.empty() && m_activeSourceFrames.empty();
            state.notEndOfStream = !m_eos;
            state.notLocallyInterrupted = !m_interrupted;
            state.inputClipEpoch = m_sourceInterruptEpoch.load(std::memory_order_acquire);
            state.expectedPreviousEpoch = expectedPreviousEpoch;
            state.newSourceInterruptEpoch = newSourceInterruptEpoch;
            state.currentGateEpoch = m_evaluationGate->SourceInterruptEpoch();
            state.currentCacheGeneration = previousCacheGeneration;
            state.newCacheGeneration = nextCacheGeneration;

            if (!DirectMotionInitialSourceGenerationRebindPolicy::CanRebindPristineInitialSourceGeneration(state)) {
                error = "Initial source-generation rebinding rejected: the graph/cache/waiter state is no longer pristine or the NewSegment interrupt epoch is not the exact next generation. inputClipEpoch=" +
                    std::to_string(state.inputClipEpoch) + ", expectedPreviousEpoch=" +
                    std::to_string(state.expectedPreviousEpoch) + ", newEpoch=" +
                    std::to_string(state.newSourceInterruptEpoch) + ", currentGateEpoch=" +
                    std::to_string(state.currentGateEpoch) + ", activeGetFrames=" +
                    std::to_string(m_activeGetFrames.load(std::memory_order_acquire)) +
                    ", cacheFrames=" + std::to_string(m_frames.size()) +
                    ", activeSourceFrames=" + std::to_string(m_activeSourceFrames.size()) +
                    ", waitingSourceFrames=" + std::to_string(m_waitingSourceFrames.size()) + ".";
                return false;
            }

            m_sourceInterruptEpoch.store(newSourceInterruptEpoch, std::memory_order_release);
            m_generation.store(nextCacheGeneration, std::memory_order_release);
        }

        PublishSourceWaitInfo();
        DirectMotionLog("INFO", "AviSynth/SourceGeneration",
            "Rebound the pristine initial InputClip to the exact first-NewSegment cancellation epoch. oldCacheGeneration=" +
            std::to_string(previousCacheGeneration) + ", newCacheGeneration=" +
            std::to_string(nextCacheGeneration) + ", oldInterruptEpoch=" +
            std::to_string(expectedPreviousEpoch) + ", newInterruptEpoch=" +
            std::to_string(newSourceInterruptEpoch) +
            "; the existing graph is retained and subsequent source publication is accepted under the new epoch.");
        return true;
    }

    void AdvanceConsumerSourceRetentionAnchor(int64_t sourceFrame) noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_sourceRetentionAnchor = (std::max)(m_sourceRetentionAnchor, sourceFrame);
                TrimLocked();
            }
            m_cacheSpaceChanged.notify_all();
        } catch (...) {
        }
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
        std::shared_lock<std::shared_mutex> generationLock(m_generationMutex);
        const uint32_t activeCalls = m_activeGetFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t peak = m_peakConcurrentGetFrames.load(std::memory_order_acquire);
        while (activeCalls > peak && !m_peakConcurrentGetFrames.compare_exchange_weak(
                   peak, activeCalls, std::memory_order_acq_rel, std::memory_order_acquire)) {}
        struct ActiveCallGuard {
            std::atomic<uint32_t>& active;
            ~ActiveCallGuard() { active.fetch_sub(1, std::memory_order_acq_rel); }
        } activeCallGuard{ m_activeGetFrames };
        const bool trace = TraceFrameRequestsEnabled();
        const auto requestStarted = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        PVideoFrame source;
        int64_t base = 0;
        int64_t cacheEnd = -1;
        size_t cacheCount = 0;
        int64_t consumerWatermark = -1;
        bool terminalDuplicate = false;
        std::string rangeError;
        double waitMs = 0.0;
        if (m_evaluationGate) m_evaluationGate->WaitUntilEnabled();
        if (!SetActiveSourceRequest(n, true)) {
            env->ThrowError("Unable to register an AviSynth source-frame retention lease.");
            return PVideoFrame();
        }
        struct ActiveSourceRequestGuard {
            InputClip* owner;
            int64_t frame;
            ~ActiveSourceRequestGuard() {
                if (owner) owner->SetActiveSourceRequest(frame, false);
            }
        } activeSourceRequestGuard{ this, n };
        const auto waitStarted = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto sourceReadyOrInterrupted = [&]() {
            if (m_evaluationGate && m_evaluationGate->SourceInterruptEpoch() !=
                m_sourceInterruptEpoch.load(std::memory_order_acquire)) return true;
            std::lock_guard<std::mutex> cacheLock(m_mutex);
            const int64_t end = m_baseIndex + static_cast<int64_t>(m_frames.size()) - 1;
            return m_interrupted || n < m_baseIndex || static_cast<int64_t>(n) <= end || m_eos;
        };
        bool sourceWaitEntered = false;
        if (m_evaluationGate) {
            std::unique_lock<std::mutex> sourceWaitLock(m_evaluationGate->SourceWaitMutex());
            if (!sourceReadyOrInterrupted()) {
                sourceWaitEntered = true;
                SetSourceWaitRegistration(n, true);
                if (trace) {
                    int64_t waitBase = 0;
                    int64_t waitEnd = -1;
                    size_t waitCount = 0;
                    {
                        std::lock_guard<std::mutex> cacheLock(m_mutex);
                        waitBase = m_baseIndex;
                        waitCount = m_frames.size();
                        waitEnd = waitBase + static_cast<int64_t>(waitCount) - 1;
                    }
                    DirectMotionLog("TRACE", "AviSynth/SourceWait", "WAIT_BEGIN requested=" +
                        std::to_string(n) + ", cacheRange=" + std::to_string(waitBase) + ".." +
                        std::to_string(waitEnd) + ", cacheFrames=" + std::to_string(waitCount) +
                        ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                        ", interruptEpoch=" + std::to_string(m_sourceInterruptEpoch.load(std::memory_order_acquire)) +
                        ", threadId=" + std::to_string(GetCurrentThreadId()));
                }
                try {
                    m_evaluationGate->SourceChangedCondition().wait(sourceWaitLock, sourceReadyOrInterrupted);
                } catch (...) {
                    SetSourceWaitRegistration(n, false);
                    throw;
                }
            }
        } else {
            std::unique_lock<std::mutex> lock(m_mutex);
            const int64_t end = m_baseIndex + static_cast<int64_t>(m_frames.size()) - 1;
            if (!(m_interrupted || n < m_baseIndex || static_cast<int64_t>(n) <= end || m_eos)) {
                sourceWaitEntered = true;
                SetSourceWaitRegistration(n, true);
                try {
                    m_framesChanged.wait(lock, [&]() {
                        const int64_t currentEnd = m_baseIndex + static_cast<int64_t>(m_frames.size()) - 1;
                        return m_interrupted || n < m_baseIndex || static_cast<int64_t>(n) <= currentEnd || m_eos;
                    });
                } catch (...) {
                    SetSourceWaitRegistration(n, false);
                    throw;
                }
            }
        }
        if (trace) waitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStarted).count();
        bool sourceWaitInterrupted = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            base = m_baseIndex;
            cacheCount = m_frames.size();
            cacheEnd = base + static_cast<int64_t>(cacheCount) - 1;
            consumerWatermark = m_sourceRetentionAnchor;
            sourceWaitInterrupted = m_interrupted || (m_evaluationGate &&
            m_evaluationGate->SourceInterruptEpoch() != m_sourceInterruptEpoch.load(std::memory_order_acquire));
            if (sourceWaitInterrupted) {
                rangeError = "AviSynth source wait interrupted by flush/stop or stale graph generation.";
            } else if (n < 0 || n < base) {
                DirectMotionTraySetSourceFrameExpired(true);
                rangeError = "directmotion AviSynth source cannot supply expired frame " + std::to_string(n) +
                    " (cached range " + std::to_string(base) + ".." + std::to_string(cacheEnd) + ").";
            } else if (static_cast<int64_t>(n) <= cacheEnd) {
                source = m_frames[static_cast<size_t>(static_cast<int64_t>(n) - base)];
            } else if (m_eos && !m_frames.empty()) {
                source = m_frames.back();
                terminalDuplicate = true;
            } else {
                rangeError = "AviSynth source wait ended without the requested frame " + std::to_string(n) + ".";
            }
        }
        if (sourceWaitEntered) {
            SetSourceWaitRegistration(n, false);
            std::lock_guard<std::mutex> cacheLock(m_mutex);
            (void)TrimLocked();
        }
        if (trace && sourceWaitEntered) {
            DirectMotionLog("TRACE", "AviSynth/SourceWait", "WAIT_END requested=" +
                std::to_string(n) + ", waitMs=" + std::to_string(waitMs) +
                ", interrupted=" + std::to_string(sourceWaitInterrupted ? 1 : 0) +
                ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                ", threadId=" + std::to_string(GetCurrentThreadId()));
        }
        if (!rangeError.empty()) {
            if (trace || rangeError.find("interrupted") == std::string::npos) {
                DirectMotionLog("ERROR", "AviSynth/InputClip",
                    "GetFrame failed: requested=" + std::to_string(n) +
                    ", cacheRange=" + std::to_string(base) + ".." + std::to_string(cacheEnd) +
                    ", cacheFrames=" + std::to_string(cacheCount) +
                    ", cacheCapacityFrames=" + std::to_string(m_cacheCapacity) +
                    ", cacheRetentionLimitFrames=" + std::to_string(m_maxRetainedFrames) +
                    ", cacheHistorySlackFrames=" + std::to_string(m_historySlackFrames) +
                    ", sourceRetentionAnchor=" + std::to_string(consumerWatermark) +
                    ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                    ", waitMs=" + std::to_string(waitMs) +
                    ", threadId=" + std::to_string(GetCurrentThreadId()) + ", reason=" + rangeError);
            }
            env->ThrowError("%s", rangeError.c_str());
            return PVideoFrame();
        }
        const auto sharedReturnStarted = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        PVideoFrame result = source;
        const double sharedReturnMs = trace ? std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sharedReturnStarted).count() : 0.0;
        if (SetActiveSourceRequest(n, false)) activeSourceRequestGuard.owner = nullptr;
        const double totalMs = trace ? std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - requestStarted).count() : 0.0;
        if (trace) {
            DirectMotionLog("TRACE", "AviSynth/InputClip",
                "GetFrame requested=" + std::to_string(n) + ", cacheRange=" +
                std::to_string(base) + ".." + std::to_string(cacheEnd) +
                ", cacheFrames=" + std::to_string(cacheCount) +
                ", cacheCapacityFrames=" + std::to_string(m_cacheCapacity) +
                ", cacheRetentionLimitFrames=" + std::to_string(m_maxRetainedFrames) +
                ", cacheHistorySlackFrames=" + std::to_string(m_historySlackFrames) +
                ", sourceRetentionAnchor=" + std::to_string(consumerWatermark) +
                ", generation=" + std::to_string(m_generation.load(std::memory_order_acquire)) +
                ", waitMs=" + std::to_string(waitMs) +
                ", sharedFrameReturnMs=" + std::to_string(sharedReturnMs) +
                ", totalMs=" + std::to_string(totalMs) +
                ", activeGetFrameCalls=" + std::to_string(m_activeGetFrames.load(std::memory_order_acquire)) +
                ", peakConcurrentGetFrames=" + std::to_string(m_peakConcurrentGetFrames.load(std::memory_order_acquire)) +
                ", prefetchThreadsInformed=" + std::to_string(m_prefetchThreadsInformed.load(std::memory_order_acquire)) +
                ", terminalDuplicate=" + std::to_string(terminalDuplicate ? 1 : 0) +
                ", threadId=" + std::to_string(GetCurrentThreadId()));
        } else if (waitMs >= 25.0 || totalMs >= 50.0) {
            DirectMotionLog("WARN", "AviSynth/Performance",
                "Slow source request: frame=" + std::to_string(n) +
                ", waitMs=" + std::to_string(waitMs) + ", totalMs=" + std::to_string(totalMs) +
                ", activeGetFrameCalls=" + std::to_string(m_activeGetFrames.load(std::memory_order_acquire)) +
                ", peakConcurrentGetFrames=" + std::to_string(m_peakConcurrentGetFrames.load(std::memory_order_acquire)));
        }
        return result;
    }

    const VideoInfo& __stdcall GetVideoInfo() override { return m_vi; }
    bool __stdcall GetParity(int /*n*/) override { return false; }
    void __stdcall GetAudio(void* /*buf*/, __int64 /*start*/, __int64 /*count*/, IScriptEnvironment* /*env*/) override {}
    int __stdcall SetCacheHints(int cachehints, int frame_range) override {
        if (cachehints == CACHE_GET_MTMODE) {
            return MT_NICE_FILTER;
        }
        if (cachehints == kAviSynthCacheInformNumThreadsHintV12) {
            const int informed = (std::max)(1, frame_range);
            m_prefetchThreadsInformed.store(informed, std::memory_order_release);
            DirectMotionLog("INFO", "AviSynth/InputClip",
                "Prefetch informed source bridge about scheduler threads=" + std::to_string(informed) + ".");
        }
        return 0;
    }

private:
    bool IsSourceGenerationInterrupted() const noexcept {
        return m_evaluationGate &&
            m_evaluationGate->SourceInterruptEpoch() != m_sourceInterruptEpoch.load(std::memory_order_acquire);
    }

    int64_t OldestActiveSourceRequest() noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_waitersMutex);
            return m_activeSourceFrames.empty() ? -1 : *m_activeSourceFrames.begin();
        } catch (...) {
            return -1;
        }
    }

    bool SetActiveSourceRequest(int64_t requestedFrame, bool active) noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_waitersMutex);
                if (active) {
                    m_activeSourceFrames.insert(requestedFrame);
                } else {
                    const auto it = m_activeSourceFrames.find(requestedFrame);
                    if (it != m_activeSourceFrames.end()) m_activeSourceFrames.erase(it);
                }
            }
            m_cacheSpaceChanged.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    void PublishCacheRangeLocked() noexcept {
        m_diagCacheBase.store(m_baseIndex, std::memory_order_release);
        m_diagCacheEnd.store(m_baseIndex + static_cast<int64_t>(m_frames.size()) - 1,
                             std::memory_order_release);
    }

    void PublishSourceWaitInfoLocked() noexcept {
        try {
            const int count = static_cast<int>(m_waitingSourceFrames.size());
            const long long lowest = count > 0 ? *m_waitingSourceFrames.begin() : -1;
            const long long highest = count > 0 ? *m_waitingSourceFrames.rbegin() : -1;
            DirectMotionTraySetSourceWaitInfo(count, lowest, highest,
                m_diagCacheBase.load(std::memory_order_acquire),
                m_diagCacheEnd.load(std::memory_order_acquire),
                m_generation.load(std::memory_order_acquire));
        } catch (...) {
        }
    }

    void PublishSourceWaitInfo() noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_waitersMutex);
            PublishSourceWaitInfoLocked();
        } catch (...) {}
    }

    void SetSourceWaitRegistration(int64_t requestedFrame, bool waiting) noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_waitersMutex);
            if (waiting) {
                m_waitingSourceFrames.insert(requestedFrame);
            } else {
                const auto it = m_waitingSourceFrames.find(requestedFrame);
                if (it != m_waitingSourceFrames.end()) m_waitingSourceFrames.erase(it);
            }
            PublishSourceWaitInfoLocked();
        } catch (...) {
        }
    }

    bool TrimLocked() {
        const int64_t oldestActiveRequest = OldestActiveSourceRequest();
        const int64_t retentionFloor = DirectMotionSourceCache::RetentionFloor(
            m_baseIndex, oldestActiveRequest, m_sourceRetentionAnchor, m_historySlackFrames);
        while (!m_frames.empty() && m_baseIndex < retentionFloor) {
            m_frames.pop_front();
            ++m_baseIndex;
        }
        PublishCacheRangeLocked();
        return m_frames.size() <= m_maxRetainedFrames;
    }
    int m_width;
    int m_height;
    VideoInfo m_vi{};
    const size_t m_cacheCapacity;
    const size_t m_maxRetainedFrames;
    const size_t m_historySlackFrames;
    std::atomic<uint64_t> m_generation;
    std::shared_ptr<AviSynthEvaluationGate> m_evaluationGate;
    std::atomic<uint64_t> m_sourceInterruptEpoch{0};
    mutable std::shared_mutex m_generationMutex;
    std::mutex m_mutex;
    std::condition_variable m_framesChanged;
    std::condition_variable m_cacheSpaceChanged;
    std::deque<PVideoFrame> m_frames;
    int64_t m_baseIndex = 0;
    int64_t m_sourceRetentionAnchor = -1;
    std::mutex m_waitersMutex;
    std::multiset<int64_t> m_waitingSourceFrames;
    std::multiset<int64_t> m_activeSourceFrames;
    std::atomic<long long> m_diagCacheBase{ 0 };
    std::atomic<long long> m_diagCacheEnd{ -1 };
    std::atomic<uint32_t> m_activeGetFrames{ 0 };
    std::atomic<uint32_t> m_peakConcurrentGetFrames{ 0 };
    std::atomic<int> m_prefetchThreadsInformed{ 0 };
    bool m_eos = false;
    bool m_interrupted = false;
};

AviSynthMvToolsEngine::~AviSynthMvToolsEngine() {
    Reset();
}

std::wstring AviSynthMvToolsEngine::GetModuleDirectory() {
    HMODULE owner = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(
        reinterpret_cast<ULONG_PTR>(&AviSynthMvToolsEngine::GetModuleDirectory));
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            address, &owner)) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    return path;
}

std::wstring AviSynthMvToolsEngine::ResolveRuntimePath() {
    const std::wstring moduleDirectory = GetModuleDirectory();
    return moduleDirectory.empty() ? std::wstring() : moduleDirectory + L"avisynth.dll";
}

std::wstring AviSynthMvToolsEngine::ResolveMvToolsPath() {
    const std::wstring directory = GetModuleDirectory();
    return directory.empty() ? std::wstring() : directory + L"mvtools2.dll";
}

std::string AviSynthMvToolsEngine::WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), bytes, nullptr, nullptr))
        return {};
    result.pop_back();
    return result;
}

std::string AviSynthMvToolsEngine::AvsErrorText(const AvisynthError& error) {
    return error.msg ? std::string(error.msg) : std::string("Unknown AviSynth exception.");
}

AviSynthRuntimeSession::~AviSynthRuntimeSession() {
    std::lock_guard<std::mutex> sessionLock(m_mutex);
    DestroyEnvironment();
}

void AviSynthRuntimeSession::DestroyEnvironment() noexcept {
    if (m_env) {
        try { m_env->DeleteScriptEnvironment(); } catch (...) {}
        m_env = nullptr;
    }
    m_avisynthInterfaceVersion = 0;
    m_ready = false;
}

bool AviSynthRuntimeSession::EnsureReady(std::string& error, bool* createdNow) {
    std::lock_guard<std::mutex> sessionLock(m_mutex);
    if (createdNow) *createdNow = false;
    if (m_ready && m_env) {
        error.clear();
        return true;
    }
    if (m_env) DestroyEnvironment();
    const std::wstring runtimePath = AviSynthMvToolsEngine::ResolveRuntimePath();
    if (runtimePath.empty()) {
        error = "Could not resolve directmotion module directory for avisynth.dll. Place avisynth.dll beside directmotion.ax.";
        return false;
    }
    {
        std::lock_guard<std::mutex> runtimeLock(g_avisynthRuntimeMutex);
        if (g_avisynthRuntimeModule) {
            if (CompareStringOrdinal(runtimePath.c_str(), -1,
                                     g_avisynthRuntimePath.c_str(), -1, TRUE) != CSTR_EQUAL) {
                error = "A different AviSynth runtime path is already active in this process. All directmotion instances must share the same adjacent avisynth.dll.";
                return false;
            }
        } else {
            g_avisynthRuntimeModule = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!g_avisynthRuntimeModule) {
                const DWORD code = GetLastError();
                error = "Could not load adjacent avisynth.dll or one of its dependencies (Win32 error " +
                        std::to_string(code) + "). External DLL search paths are disabled; keep the matching runtime beside directmotion.ax.";
                return false;
            }
            std::wstring actualRuntimePath;
            if (!ModuleMatchesRequestedPath(g_avisynthRuntimeModule, runtimePath, actualRuntimePath)) {
                FreeLibrary(g_avisynthRuntimeModule);
                g_avisynthRuntimeModule = nullptr;
                error = "Windows resolved avisynth.dll from a path other than the adjacent DLL. Expected: " +
                    AviSynthMvToolsEngine::WideToUtf8(runtimePath) + (actualRuntimePath.empty() ? std::string() : "; loaded: " + AviSynthMvToolsEngine::WideToUtf8(actualRuntimePath));
                return false;
            }
            g_avisynthRuntimePath = runtimePath;
        }
        std::wstring actualRuntimePath;
        if (!ModuleMatchesRequestedPath(g_avisynthRuntimeModule, runtimePath, actualRuntimePath)) {
            error = "The active AviSynth module is not the adjacent avisynth.dll. Expected: " +
                AviSynthMvToolsEngine::WideToUtf8(runtimePath) + (actualRuntimePath.empty() ? std::string() : "; loaded: " + AviSynthMvToolsEngine::WideToUtf8(actualRuntimePath));
            return false;
        }
        m_avisynthModule = g_avisynthRuntimeModule;
    }

    const auto createEnvironment = reinterpret_cast<CreateScriptEnvironmentFn>(
        GetProcAddress(m_avisynthModule, "CreateScriptEnvironment"));
    if (!createEnvironment) {
        error = "Loaded avisynth.dll does not export CreateScriptEnvironment.";
        return false;
    }
    auto tryCreateEnvironment = [&](int version, std::string& failure) -> IScriptEnvironment* {
        try {
            IScriptEnvironment* env = createEnvironment(version);
            if (!env) failure = "CreateScriptEnvironment returned null.";
            return env;
        } catch (const AvisynthError& ex) {
            failure = AviSynthMvToolsEngine::AvsErrorText(ex);
        } catch (const std::exception& ex) {
            failure = std::string("std::exception: ") + ex.what();
        } catch (...) {
            failure = "unrecognized exception";
        }
        return nullptr;
    };

    std::string preferredFailure;
    m_env = tryCreateEnvironment(kPreferredAviSynthInterfaceVersion, preferredFailure);
    if (m_env) {
        m_avisynthInterfaceVersion = kPreferredAviSynthInterfaceVersion;
    } else {
        DirectMotionLog("WARN", "AviSynth/Runtime",
            "CreateScriptEnvironment(V" + std::to_string(kPreferredAviSynthInterfaceVersion) +
            ") failed: " + preferredFailure + "; retrying V" +
            std::to_string(kFallbackAviSynthInterfaceVersion) + ".");
        std::string fallbackFailure;
        m_env = tryCreateEnvironment(kFallbackAviSynthInterfaceVersion, fallbackFailure);
        if (m_env) {
            m_avisynthInterfaceVersion = kFallbackAviSynthInterfaceVersion;
        } else {
            error = "CreateScriptEnvironment failed for both supported interfaces: V" +
                std::to_string(kPreferredAviSynthInterfaceVersion) + " (" + preferredFailure +
                ") and V" + std::to_string(kFallbackAviSynthInterfaceVersion) +
                " (" + fallbackFailure + "). Use an AviSynth+ runtime compatible with mvtools2.dll.";
            return false;
        }
    }
    DirectMotionLog("INFO", "AviSynth/Runtime",
        "CreateScriptEnvironment succeeded; selected AviSynth C++ interface V" +
        std::to_string(m_avisynthInterfaceVersion) + ".");

    const AVS_Linkage* runtimeLinkage = nullptr;
    try {
        runtimeLinkage = m_env->GetAVSLinkage();
    } catch (...) {
        error = "Could not obtain AviSynth C++ API linkage with GetAVSLinkage.";
        return false;
    }
    if (!runtimeLinkage) {
        error = "AviSynth returned a null C++ API linkage table (AVS_linkage).";
        return false;
    }
    {
        std::lock_guard<std::mutex> linkageLock(g_avisynthRuntimeMutex);
        if (AVS_linkage && AVS_linkage != runtimeLinkage) {
            error = "AviSynth returned a different C++ API linkage table within the same process.";
            return false;
        }
        if (!AVS_linkage) AVS_linkage = runtimeLinkage;
    }

    if (AviSynthInternalLoggingEnabled()) {
        try {
            const std::wstring logPath = AviSynthMvToolsEngine::GetModuleDirectory() +
                L"directmotion_avisynth_" + std::to_wstring(GetCurrentProcessId()) + L".log";
            const std::string logPathUtf8 = AviSynthMvToolsEngine::WideToUtf8(logPath);
            const AVSValue logArgs[] = { AVSValue(logPathUtf8.c_str()), AVSValue(3) };
            (void)m_env->Invoke("SetLogParams", AVSValue(logArgs, 2));
            const AVSValue markerArgs[] = {
                AVSValue("directmotion diagnostics: AviSynth SetLogParams active"),
                AVSValue(3)
            };
            (void)m_env->Invoke("LogMsg", AVSValue(markerArgs, 2));
            DirectMotionLog("INFO", "AviSynth/Diagnostics",
                "AviSynth internal log enabled at " + logPathUtf8 + ", level=LOG_INFO(3).");
        } catch (const AvisynthError& ex) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "AviSynth SetLogParams/LogMsg setup failed; continuing without relying on the AviSynth-owned log: " +
                AviSynthMvToolsEngine::AvsErrorText(ex));
        } catch (...) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "AviSynth SetLogParams/LogMsg setup failed with an unknown exception; continuing without relying on the AviSynth-owned log.");
        }
    }

    if (AviSynthGraphAnalysisEnabled()) {
        try {
            (void)m_env->Invoke("SetGraphAnalysis", AVSValue(true));
            DirectMotionLog("INFO", "AviSynth/Diagnostics",
                "AviSynth SetGraphAnalysis(true) enabled before graph construction.");
        } catch (const AvisynthError& ex) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "SetGraphAnalysis was unavailable or failed; continuing without graph instrumentation: " +
                AviSynthMvToolsEngine::AvsErrorText(ex));
        } catch (...) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "SetGraphAnalysis failed with an unknown exception; continuing without graph instrumentation.");
        }
    }

    const std::wstring pluginPath = AviSynthMvToolsEngine::ResolveMvToolsPath();
    if (pluginPath.empty() || GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "mvtools2.dll was not found beside directmotion.ax. DLL search paths and environment overrides are disabled.";
        return false;
    }
    {
        std::lock_guard<std::mutex> runtimeLock(g_avisynthRuntimeMutex);
        if (g_mvtoolsRuntimeModule) {
            if (CompareStringOrdinal(pluginPath.c_str(), -1,
                                     g_mvtoolsRuntimePath.c_str(), -1, TRUE) != CSTR_EQUAL) {
                error = "A different MVTools2 DLL path is already active. Every directmotion instance must use the adjacent mvtools2.dll.";
                return false;
            }
        } else {
            g_mvtoolsRuntimeModule = LoadLibraryExW(pluginPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!g_mvtoolsRuntimeModule) {
                const DWORD code = GetLastError();
                error = "Could not load the adjacent mvtools2.dll or one of its dependencies (Win32 error " +
                    std::to_string(code) + "). Keep avisynth.dll, mvtools2.dll and their required dependencies beside directmotion.ax.";
                return false;
            }
            std::wstring actualPluginPath;
            if (!ModuleMatchesRequestedPath(g_mvtoolsRuntimeModule, pluginPath, actualPluginPath)) {
                FreeLibrary(g_mvtoolsRuntimeModule);
                g_mvtoolsRuntimeModule = nullptr;
                error = "Windows resolved mvtools2.dll from a path other than the adjacent DLL. Expected: " +
                    AviSynthMvToolsEngine::WideToUtf8(pluginPath) + (actualPluginPath.empty() ? std::string() : "; loaded: " + AviSynthMvToolsEngine::WideToUtf8(actualPluginPath));
                return false;
            }
            g_mvtoolsRuntimePath = pluginPath;
        }
        std::wstring actualPluginPath;
        if (!ModuleMatchesRequestedPath(g_mvtoolsRuntimeModule, pluginPath, actualPluginPath)) {
            error = "The active MVTools module is not the adjacent mvtools2.dll. Expected: " +
                AviSynthMvToolsEngine::WideToUtf8(pluginPath) + (actualPluginPath.empty() ? std::string() : "; loaded: " + AviSynthMvToolsEngine::WideToUtf8(actualPluginPath));
            return false;
        }
    }
    const bool hasInit3 = GetProcAddress(g_mvtoolsRuntimeModule, "AvisynthPluginInit3") != nullptr;
    const bool hasInit2 = GetProcAddress(g_mvtoolsRuntimeModule, "AvisynthPluginInit2") != nullptr;
    const bool hasInit = GetProcAddress(g_mvtoolsRuntimeModule, "AvisynthPluginInit") != nullptr;
    const bool hasCInit = GetProcAddress(g_mvtoolsRuntimeModule, "avisynth_c_plugin_init") != nullptr;
    try {
        SetLastError(ERROR_SUCCESS);
        if (hasInit3) {
            using AvisynthPluginInit3Fn = const char* (__stdcall*)(IScriptEnvironment*, const AVS_Linkage*);
            auto init3 = reinterpret_cast<AvisynthPluginInit3Fn>(
                GetProcAddress(g_mvtoolsRuntimeModule, "AvisynthPluginInit3"));
            if (!init3) {
                error = "mvtools2.dll advertised AvisynthPluginInit3 but GetProcAddress returned null.";
                return false;
            }
            const char* pluginDescription = init3(m_env, runtimeLinkage);
            DirectMotionLog("INFO", "AviSynth/MVTools",
                "Adjacent mvtools2.dll registered via AvisynthPluginInit3; path=" +
                AviSynthMvToolsEngine::WideToUtf8(g_mvtoolsRuntimePath) + ", exports{Init3=" +
                std::to_string(hasInit3) + ", Init2=" + std::to_string(hasInit2) +
                ", Init=" + std::to_string(hasInit) + ", CInit=" + std::to_string(hasCInit) +
                "}, description=" + std::string(pluginDescription ? pluginDescription : "<null>") + ".");
        } else {
            if (!m_env->FunctionExists("LoadPlugin")) {
                error = "This AviSynth+ environment does not expose LoadPlugin and mvtools2.dll has no Init3 export.";
                return false;
            }
            const std::string pluginUtf8 = AviSynthMvToolsEngine::WideToUtf8(pluginPath);
            const AVSValue pluginArgs[] = { AVSValue(pluginUtf8.c_str()), AVSValue(true) };
            const char* pluginArgNames[] = { "filename", "utf8" };
            (void)m_env->Invoke("LoadPlugin", AVSValue(pluginArgs, 2), pluginArgNames);
            DirectMotionLog("INFO", "AviSynth/MVTools",
                "AviSynth LoadPlugin returned successfully for legacy plugin " + AviSynthMvToolsEngine::WideToUtf8(pluginPath) + ".");
        }
    } catch (const AvisynthError& ex) {
        error = "AviSynth/MVTools plugin initialization failed: " + AviSynthMvToolsEngine::AvsErrorText(ex);
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (const std::exception& ex) {
        error = std::string("AviSynth/MVTools plugin initialization raised std::exception: ") + ex.what();
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (const std::string& ex) {
        error = "AviSynth/MVTools plugin initialization raised std::string: " + ex;
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (const char* ex) {
        error = std::string("AviSynth/MVTools plugin initialization raised a C-string exception: ") +
            (ex ? ex : "<null>");
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (int ex) {
        error = "AviSynth/MVTools plugin initialization raised integer exception " + std::to_string(ex) + ".";
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (unsigned int ex) {
        error = "AviSynth/MVTools plugin initialization raised unsigned integer exception " + std::to_string(ex) + ".";
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    } catch (...) {
        const DWORD win32Error = GetLastError();
        const std::string route = hasInit3 ? "direct AvisynthPluginInit3 call" : "legacy AviSynth LoadPlugin call";
        error = "mvtools2.dll threw an unrecognized C++ exception during " + route +
            "; Win32 last-error=" + std::to_string(win32Error) + ", path=" + AviSynthMvToolsEngine::WideToUtf8(pluginPath) +
            ". This confirms failure inside plugin initialization or its ABI boundary, before graph construction.";
        DirectMotionLog("ERROR", "AviSynth/MVTools", error);
        return false;
    }
    for (const char* required : {"MSuper", "MAnalyse", "MRecalculate", "MBlockFps"}) {
        if (!m_env->FunctionExists(required)) {
            error = std::string("mvtools2.dll loaded, but AviSynth function '") + required +
                    "' is not registered. Check plugin/runtime bitness and compatibility.";
            return false;
        }
    }
    m_ready = true;
    if (createdNow) *createdNow = true;
    DirectMotionLog("INFO", "AviSynth/RuntimeSession",
        "Persistent AviSynth environment is ready; MVTools functions registered once for this filter instance.");
    return true;
}

bool AviSynthMvToolsEngine::BuildGraph(const std::shared_ptr<AviSynthEvaluationGate>& evaluationGate, std::string& error) {
    const long double virtualOutputFrames = static_cast<long double>(kSyntheticClipFrameCount) *
        static_cast<long double>(m_targetRate.num) * static_cast<long double>(m_sourceRate.den) /
        (static_cast<long double>(m_targetRate.den) * static_cast<long double>(m_sourceRate.num));
    if (!std::isfinite(static_cast<double>(virtualOutputFrames)) ||
        virtualOutputFrames > static_cast<long double>((std::numeric_limits<int>::max)() - 16)) {
        error = "The configured source/target FPS ratio would overflow the fixed AviSynth virtual clip length.";
        return false;
    }
    std::string graphStage = "AviSynth+ frame scheduler configuration";
    int prefetchThreads = 1;
    int prefetchFrames = 1;
    int prefetchSourceLookaheadFrames = 0;
    size_t sourceCacheFrames = 0;
    size_t sourceCacheHistorySlackFrames = 0;
    DWORD physicalCores = 0;
    const DWORD logicalProcessors = QueryLogicalProcessorCount();
    const bool physicalTopologyAvailable = QueryPhysicalCoreCount(physicalCores);
    if (!physicalTopologyAvailable) {
        physicalCores = (std::max)(static_cast<DWORD>(1), (logicalProcessors + static_cast<DWORD>(1)) / static_cast<DWORD>(2));
    }
    int cpuWorkerBudget = static_cast<int>((std::min)(logicalProcessors, static_cast<DWORD>(32)));
    switch (m_cpuProfile) {
    case kCpuProfilePhysical:
        cpuWorkerBudget = static_cast<int>((std::min)(physicalCores, static_cast<DWORD>(32)));
        break;
    case kCpuProfileHalfLogical:
        cpuWorkerBudget = static_cast<int>((std::max)(static_cast<DWORD>(1), (logicalProcessors + static_cast<DWORD>(1)) / static_cast<DWORD>(2)));
        cpuWorkerBudget = (std::min)(cpuWorkerBudget, 32);
        break;
    case kCpuProfileCustom:
        cpuWorkerBudget = std::clamp(m_customCpuWorkers, 1, 32);
        break;
    default:
        cpuWorkerBudget = static_cast<int>((std::min)(logicalProcessors, static_cast<DWORD>(32)));
        break;
    }
    try {
        const uint64_t pixelCount = static_cast<uint64_t>(m_width) * static_cast<uint64_t>(m_height);
        const long double frameScale = std::sqrt(
            static_cast<long double>(pixelCount) / static_cast<long double>(kReferenceFramePixels));
        const long double boundedScale = (std::max)(1.0L, (std::min)(2.0L, frameScale));
        const size_t resolutionScaledCacheBytes = static_cast<size_t>(
            static_cast<long double>(kBaseSourceCacheBudgetBytes) * boundedScale);

        ULONGLONG installedMemoryKiB = 0;
        if (!GetPhysicallyInstalledSystemMemory(&installedMemoryKiB) || installedMemoryKiB == 0) {
            installedMemoryKiB = 8ull * 1024ull * 1024ull;
        }
        const size_t installedMemoryBytes = static_cast<size_t>(installedMemoryKiB * 1024ull);
        const size_t totalWorkingSetBudgetBytes = (std::max)(kMinWorkingSetBudgetBytes,
            (std::min)(kMaxWorkingSetBudgetBytes, installedMemoryBytes / 3u));
        const size_t sourceCacheBudgetBytes = (std::max)(kMinSourceCacheBudgetBytes,
            (std::min)(kMaxSourceCacheBudgetBytes,
                (std::min)(resolutionScaledCacheBytes, totalWorkingSetBudgetBytes / 3u)));
        const int requestedAviSynthMemoryMaxMiB = static_cast<int>((std::max)(static_cast<size_t>(256u),
            (std::min)(static_cast<size_t>(kAviSynthMemoryMaxMiB),
                totalWorkingSetBudgetBytes / (1024u * 1024u))));
        const int actualAviSynthMemoryMaxMiB = m_env->SetMemoryMax(requestedAviSynthMemoryMaxMiB);
        if (actualAviSynthMemoryMaxMiB <= 0) {
            error = "AviSynth+ SetMemoryMax did not return a valid internal frame-cache limit.";
            return false;
        }

        const size_t prefetchFrameCostEquivalents = static_cast<size_t>((std::max)(5.0L,
            (std::min)(8.0L, 3.0L + std::ceil(frameScale) + 2.0L)));
        const int schedulerThreadBudget = (std::min)(cpuWorkerBudget, 32);
        const int automaticPrefetchFrames = (std::min)(64, schedulerThreadBudget * 2);
        const int requestedPrefetchFrames = m_requestedPrefetchFrames == 0
            ? automaticPrefetchFrames : m_requestedPrefetchFrames;
        prefetchFrames = std::clamp(requestedPrefetchFrames, 1, 64);
        prefetchThreads = schedulerThreadBudget;

        const long double outputPerSource =
            (static_cast<long double>(m_targetRate.num) * m_sourceRate.den) /
            (static_cast<long double>(m_targetRate.den) * m_sourceRate.num);
        if (!(outputPerSource > 0.0L) || !std::isfinite(static_cast<double>(outputPerSource))) {
            error = "Invalid source/target FPS ratio while configuring AviSynth+ Prefetch.";
            return false;
        }
        const size_t frameBytes = static_cast<size_t>(m_width) * m_height * 3u / 2u;
        if (frameBytes == 0) {
            error = "Invalid frame size while configuring the AviSynth+ source cache.";
            return false;
        }
        const size_t sourceCacheMemoryFrames = sourceCacheBudgetBytes / frameBytes;
        const size_t sourceCacheRetentionFrames = DirectMotionSourceCache::MaxRetainedFrames(
            sourceCacheMemoryFrames, kSourceCacheHardCapFrames);
        const size_t totalWorkingMemoryFrames = totalWorkingSetBudgetBytes / frameBytes;
        auto requiredSourceFrames = [&](int outputFrames) -> size_t {
            const long double needed = static_cast<long double>(outputFrames) / outputPerSource;
            return static_cast<size_t>(std::ceil(needed)) +
                kMotionLookaheadFrames + kSourceCacheSafetyFrames;
        };
        auto plannedCacheFrames = [&](int outputFrames) -> size_t {
            return requiredSourceFrames(outputFrames) + kSourceCacheHistoryMarginFrames;
        };
        auto estimatedWorkingSetFrames = [&](int outputFrames) -> size_t {
            const size_t prefetchCost = static_cast<size_t>(outputFrames) * prefetchFrameCostEquivalents;
            if (sourceCacheRetentionFrames > (std::numeric_limits<size_t>::max)() - prefetchCost)
                return (std::numeric_limits<size_t>::max)();
            return sourceCacheRetentionFrames + prefetchCost;
        };
        while (prefetchFrames > 1 &&
               (plannedCacheFrames(prefetchFrames) > sourceCacheRetentionFrames ||
                estimatedWorkingSetFrames(prefetchFrames) > totalWorkingMemoryFrames)) {
            --prefetchFrames;
        }
        sourceCacheFrames = plannedCacheFrames(prefetchFrames);
        if (sourceCacheRetentionFrames == 0 || sourceCacheFrames > sourceCacheRetentionFrames ||
            estimatedWorkingSetFrames(prefetchFrames) > totalWorkingMemoryFrames) {
            error = "Not enough memory/cache budget to build a safe AviSynth+ Prefetch window, even with a one-frame read-ahead window.";
            return false;
        }
        m_prefetchThreads = prefetchThreads;
        m_prefetchFrames = prefetchFrames;
        prefetchSourceLookaheadFrames = static_cast<int>(std::ceil(
            static_cast<long double>(prefetchFrames) / outputPerSource));
        sourceCacheHistorySlackFrames = DirectMotionSourceCache::HistorySlackFrames(
            static_cast<size_t>(prefetchSourceLookaheadFrames),
            static_cast<size_t>(kMotionLookaheadFrames),
            kSourceCacheSafetyFrames, kSourceCacheHistoryMarginFrames);
        const int prefetchSourceLeadFrames = static_cast<int>(std::ceil(
            static_cast<long double>(prefetchFrames) / outputPerSource));
        m_bootstrapInputFramesRequired =
            DirectMotionBootstrapEvaluation::RequiredSourceFramesForFirstOutput(kMotionLookaheadFrames);

        if (!m_env->FunctionExists("SetFilterMTMode") || !m_env->FunctionExists("Prefetch") ||
            !m_env->FunctionExists("Trim")) {
            error = "The loaded AviSynth+ runtime does not expose SetFilterMTMode, Prefetch and Trim; frame-parallel offset bootstrap is required by this backend.";
            return false;
        }
        const AVSValue mtModeArgs[] = { AVSValue("DEFAULT_MT_MODE"), AVSValue(2) };
        graphStage = "SetFilterMTMode(DEFAULT_MT_MODE, 2)";
        (void)m_env->Invoke("SetFilterMTMode", AVSValue(mtModeArgs, 2));

        DirectMotionLog("INFO", "AviSynth/Scheduler",
            "Frame-parallel scheduler planned: SetFilterMTMode(DEFAULT_MT_MODE,2), Prefetch threads=" +
            std::to_string(prefetchThreads) + ", framesRequested=" + std::to_string(requestedPrefetchFrames) +
            ", framesEffective=" + std::to_string(prefetchFrames) +
            ", framesMode=" + std::string(m_requestedPrefetchFrames == 0 ? "Auto(2xWorkers)" : "Manual") +
            ", cpuProfile=" + CpuProfileName(m_cpuProfile) +
            ", cpuWorkerBudget=" + std::to_string(cpuWorkerBudget) +
            ", logicalProcessors=" + std::to_string(logicalProcessors) +
            ", physicalCores=" + std::to_string(physicalCores) +
            ", physicalTopologyDetected=" + std::to_string(physicalTopologyAvailable ? 1 : 0) +
            ", prefetchFrameCostEquivalent=" + std::to_string(prefetchFrameCostEquivalents) +
            ", prefetchWindowPolicy=profile-independent-memory-bounded" +
            ", sourceCacheFrames=" + std::to_string(sourceCacheFrames) +
            ", sourceCacheRetentionLimitFrames=" + std::to_string(sourceCacheRetentionFrames) +
            ", sourceCacheHistorySlackFrames=" + std::to_string(sourceCacheHistorySlackFrames) +
            ", sourceLookaheadFrames=" + std::to_string(prefetchSourceLookaheadFrames) +
            ", prefetchSourceLeadFrames=" + std::to_string(prefetchSourceLeadFrames) +
            ", bootstrapInputFrames=" + std::to_string(m_bootstrapInputFramesRequired) +
            ", bootstrapContext=" + std::string(m_resumeWarmStart ? "after-strict-pause-run" : "initial-or-other-reset") +
            ", bootstrapPolicy=raw-direct-first-uncovered-output" +
            ", bootstrapEvaluation=raw-MBlockFps-direct-no-Prefetch-GetFrame" +
            ", prefetchActivation=lazy-after-fallback-watermark-seal-frame-zero-is-first-successor" +
            ", prefetchReadAheadWaitMode=processing-worker-asynchronous" +
            ", effectiveOutputPerSource=" + std::to_string(static_cast<double>(outputPerSource)) +
            ", cacheSourceFramesForPrefetchWindow=" + std::to_string(
                static_cast<size_t>(std::ceil(static_cast<long double>(prefetchFrames) / outputPerSource))) +
            ", frameScale=" + std::to_string(static_cast<double>(boundedScale)) +
            ", frameBytesMiB=" + std::to_string(static_cast<double>(frameBytes) / (1024.0 * 1024.0)) +
            ", installedMemoryMiB=" + std::to_string(installedMemoryBytes / (1024u * 1024u)) +
            ", totalWorkingSetBudgetMiB=" + std::to_string(totalWorkingSetBudgetBytes / (1024u * 1024u)) +
            ", sourceCacheBudgetMiB=" + std::to_string(sourceCacheBudgetBytes / (1024u * 1024u)) +
            ", estimatedWorkingSetMiB=" + std::to_string(estimatedWorkingSetFrames(prefetchFrames) * frameBytes / (1024u * 1024u)) +
            ", delivery=AsyncProcessingWorker->AsyncDeliveryWorker; Receive publishes to the live source cache.");

        DirectMotionLog("INFO", "AviSynth/Memory",
            "SetMemoryMax requestedMiB=" + std::to_string(requestedAviSynthMemoryMaxMiB) +
            ", actualMiB=" + std::to_string(actualAviSynthMemoryMaxMiB) +
            ", resolutionPixels=" + std::to_string(pixelCount) +
            ", frameScale=" + std::to_string(static_cast<double>(boundedScale)) +
            ", installedMemoryMiB=" + std::to_string(installedMemoryBytes / (1024u * 1024u)) +
            "; source-ring retention has its own frame-count/byte budget inside the shared AviSynth frame-buffer pool; "
            "SetMemoryMax does not cap every MVTools/plugin temporary allocation, so the weighted estimate retains headroom.");

        graphStage = "source clip creation";
        const uint64_t sourceCacheGeneration = g_sourceCacheGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        DirectMotionTraySetSourceFrameExpired(false);
        DirectMotionTraySetSourceCacheProducerWaiting(false);
        m_input = new InputClip(m_width, m_height, m_sourceRate, sourceCacheFrames,
            sourceCacheRetentionFrames, sourceCacheHistorySlackFrames,
            sourceCacheGeneration, evaluationGate);
        m_sourceClip = std::make_unique<PClip>(m_input);
        DirectMotionLog("INFO", "AviSynth/SourceCache",
            "Created synchronous live source window generation=" + std::to_string(sourceCacheGeneration) +
            ", capacityFrames=" + std::to_string(sourceCacheFrames) +
            ", retentionLimitFrames=" + std::to_string(sourceCacheRetentionFrames) +
            ", historySlackFrames=" + std::to_string(sourceCacheHistorySlackFrames) +
            ", backing=AviSynth-PVideoFrame-shared-immutable" +
            ", per-worker-source-frame-copy=disabled" +
            ", motionLookaheadFrames=" + std::to_string(kMotionLookaheadFrames) +
            "; Receive publishes each source surface once and pulls all currently safe output frames before returning.");

        graphStage = "MSuper";
        AVSValue superResult;
        if (m_options.advanced) {
            const AVSValue superArgs[] = {
                AVSValue(*m_sourceClip), AVSValue(0), AVSValue(0), AVSValue(m_options.superPel),
                AVSValue(m_options.superLevels), AVSValue(m_options.analyseChroma), AVSValue(m_options.superSharp),
                AVSValue(m_options.superRFilter), AVSValue(false)
            };
            const char* superNames[] = {
                nullptr, "hpad", "vpad", "pel", "levels", "chroma", "sharp", "rfilter", "mt"
            };
            superResult = m_env->Invoke("MSuper", AVSValue(superArgs, 9), superNames);
        } else {
            const AVSValue superArgs[] = {
                AVSValue(*m_sourceClip), AVSValue(0), AVSValue(0), AVSValue(1), AVSValue(false)
            };
            const char* superNames[] = { nullptr, "hpad", "vpad", "pel", "mt" };
            superResult = m_env->Invoke("MSuper", AVSValue(superArgs, 5), superNames);
        }
        if (!superResult.IsClip()) {
            error = "MSuper did not return an AviSynth clip.";
            return false;
        }
        m_superClip = std::make_unique<PClip>(superResult.AsClip());

        const int profileAnalyseBlockSize = 32;
        const int blockSize = m_options.advanced ? m_options.analyseBlockSize :
            ((m_requestedGrid == 1) ? 4 : (m_requestedGrid == 2) ? 8 :
             (m_requestedGrid == 4) ? 16 : (m_requestedGrid == 8) ? 32 : profileAnalyseBlockSize);
        const int search = m_options.advanced ? m_options.analyseSearch :
            ((m_performance == 1) ? 3 : ((m_performance == 3) ? 0 : 4));
        const int searchParam = m_options.advanced ? m_options.analyseSearchParam :
            ((m_performance == 1) ? 3 : ((m_performance == 3) ? 1 : 2));
        const int badRange = m_options.advanced ? m_options.analyseBadRange :
            ((m_performance == 1) ? -24 : (m_performance == 2) ? 24 : 0);
        const bool analyseChroma = m_options.advanced ? m_options.analyseChroma : false;
        const bool analyseTrueMotion = m_options.advanced ? m_options.analyseTrueMotion : true;
        const int analyseOverlap = m_options.advanced ? m_options.analyseOverlap : 0;
        const auto& analyseContract =
            DirectMotionAviSynth::GetMAnalyseInvocationContract(false);
        const auto& analyseAdvancedContract =
            DirectMotionAviSynth::GetMAnalyseInvocationContract(true);
        auto invokeMAnalyse = [&](bool backward) -> AVSValue {
            if (!m_options.advanced) {
                const AVSValue args[] = {
                    AVSValue(*m_superClip), AVSValue(false), AVSValue(backward),
                    AVSValue(blockSize), AVSValue(blockSize), AVSValue(searchParam),
                    AVSValue(0), AVSValue(search), AVSValue(badRange),
                    AVSValue(DirectMotionAviSynth::kMAnalyseInternalThreadingEnabled),
                    AVSValue(DirectMotionAviSynth::kMAnalyseTemporalPredictorsEnabled)
                };
                try {
                    return m_env->Invoke("MAnalyse",
                        AVSValue(args, static_cast<int>(analyseContract.argumentCount)),
                        analyseContract.argumentNames);
                } catch (const IScriptEnvironment::NotFound&) {
                    error = "MVTools2 rejected the required standard MAnalyse(mt=false) signature. "
                        "DirectMotion will not retry without mt because that would restore MVTools' "
                        "default mt=true and could activate its private AVSTP scheduler alongside "
                        "AviSynth+ Prefetch. Install a compatible mvtools2.dll that accepts the "
                        "documented mt argument.";
                    DirectMotionLog("ERROR", "AviSynth/MVTools", error);
                    return AVSValue();
                }
            }

            const AVSValue args[] = {
                AVSValue(*m_superClip), AVSValue(m_options.analyseChroma), AVSValue(backward),
                AVSValue(blockSize), AVSValue(blockSize), AVSValue(searchParam),
                AVSValue(0), AVSValue(search), AVSValue(badRange),
                AVSValue(analyseOverlap), AVSValue(analyseTrueMotion),
                AVSValue(DirectMotionAviSynth::kMAnalyseInternalThreadingEnabled),
                AVSValue(DirectMotionAviSynth::kMAnalyseTemporalPredictorsEnabled)
            };
            try {
                return m_env->Invoke("MAnalyse",
                    AVSValue(args, static_cast<int>(analyseAdvancedContract.argumentCount)),
                    analyseAdvancedContract.argumentNames);
            } catch (const IScriptEnvironment::NotFound&) {
                error = "MVTools2 rejected the required advanced MAnalyse(mt=false) signature. "
                    "DirectMotion will not retry without mt because that would restore MVTools' "
                    "default mt=true and could activate its private AVSTP scheduler alongside "
                    "AviSynth+ Prefetch. Install a compatible mvtools2.dll that accepts the "
                    "documented mt argument.";
                DirectMotionLog("ERROR", "AviSynth/MVTools", error);
                return AVSValue();
            }
        };
        graphStage = "backward MAnalyse";
        AVSValue backwardResult = invokeMAnalyse(true);
        if (!backwardResult.IsClip()) {
            if (error.empty()) error = "Backward MAnalyse did not return an AviSynth clip.";
            return false;
        }

        graphStage = "forward MAnalyse";
        AVSValue forwardResult = invokeMAnalyse(false);
        if (!forwardResult.IsClip()) {
            if (error.empty()) error = "Forward MAnalyse did not return an AviSynth clip.";
            return false;
        }

        const char* recalculateNames[] = {
            nullptr, nullptr, "blksize", "blksizev", "searchparam", "search", "chroma"
        };
        const char* recalculateAdvancedNames[] = {
            nullptr, nullptr, "blksize", "blksizev", "searchparam", "search", "chroma",
            "overlap", "truemotion"
        };
        const bool quickFastRefinement = !m_options.advanced &&
            (m_performance == 3 || m_quickRecalcMode == 1);
        const int recalcBlock = m_options.advanced ? m_options.recalcBlockSize :
            (quickFastRefinement ? 16 : 8);
        const int recalcSearch = m_options.advanced ? m_options.recalcSearch :
            (quickFastRefinement ? 0 : 3);
        const int recalcSearchParam = m_options.advanced ? m_options.recalcSearchParam :
            (quickFastRefinement ? 1 : 0);
        const int recalcOverlap = m_options.advanced ? m_options.recalcOverlap : 0;
        const bool recalcChroma = m_options.advanced ? m_options.analyseChroma : false;
        const bool recalcTrueMotion = m_options.advanced ? m_options.analyseTrueMotion : true;
        const AVSValue recalculateBackwardArgs[] = {
            AVSValue(*m_superClip), AVSValue(backwardResult.AsClip()),
            AVSValue(recalcBlock), AVSValue(recalcBlock), AVSValue(recalcSearchParam),
            AVSValue(recalcSearch), AVSValue(recalcChroma), AVSValue(recalcOverlap),
            AVSValue(recalcTrueMotion)
        };
        graphStage = "backward MRecalculate";
        AVSValue recalculateBackwardResult = m_options.advanced
            ? m_env->Invoke("MRecalculate", AVSValue(recalculateBackwardArgs, 9), recalculateAdvancedNames)
            : m_env->Invoke("MRecalculate", AVSValue(recalculateBackwardArgs, 7), recalculateNames);
        if (!recalculateBackwardResult.IsClip()) {
            error = "Backward MRecalculate did not return an AviSynth clip.";
            return false;
        }

        const AVSValue recalculateForwardArgs[] = {
            AVSValue(*m_superClip), AVSValue(forwardResult.AsClip()),
            AVSValue(recalcBlock), AVSValue(recalcBlock), AVSValue(recalcSearchParam),
            AVSValue(recalcSearch), AVSValue(recalcChroma), AVSValue(recalcOverlap),
            AVSValue(recalcTrueMotion)
        };
        graphStage = "forward MRecalculate";
        AVSValue recalculateForwardResult = m_options.advanced
            ? m_env->Invoke("MRecalculate", AVSValue(recalculateForwardArgs, 9), recalculateAdvancedNames)
            : m_env->Invoke("MRecalculate", AVSValue(recalculateForwardArgs, 7), recalculateNames);
        if (!recalculateForwardResult.IsClip()) {
            error = "Forward MRecalculate did not return an AviSynth clip.";
            return false;
        }
        m_mvBackwardClip = std::make_unique<PClip>(recalculateBackwardResult.AsClip());
        m_mvForwardClip = std::make_unique<PClip>(recalculateForwardResult.AsClip());

        const int64_t rawNum = static_cast<int64_t>(m_targetRate.num);
        const int64_t rawDen = static_cast<int64_t>(m_targetRate.den);
        const int64_t divisor = std::gcd(rawNum, rawDen);
        const int64_t fpsNum = rawNum / divisor;
        const int64_t fpsDen = rawDen / divisor;
        if (fpsNum <= 0 || fpsDen <= 0 ||
            fpsNum > (std::numeric_limits<int>::max)() ||
            fpsDen > (std::numeric_limits<int>::max)()) {
            error = "The absolute output frame rate cannot be represented by MBlockFps num/den.";
            return false;
        }
        AVSValue fpsResult;
        graphStage = "MBlockFps (explicit source+super+backward+forward clips)";
        if (m_options.advanced) {
            const AVSValue fpsArgs[] = {
                AVSValue(*m_sourceClip), AVSValue(*m_superClip),
                AVSValue(*m_mvBackwardClip), AVSValue(*m_mvForwardClip),
                AVSValue(static_cast<int>(fpsNum)), AVSValue(static_cast<int>(fpsDen)),
                AVSValue(m_options.blockFpsMode), AVSValue(m_options.blockFpsMaskScale),
                AVSValue(m_options.blockFpsBlend), AVSValue(m_options.thSCD1), AVSValue(m_options.thSCD2)
            };
            const char* fpsNames[] = {
                nullptr, nullptr, nullptr, nullptr, "num", "den", "mode", "ml", "blend", "thSCD1", "thSCD2"
            };
            fpsResult = m_env->Invoke("MBlockFps", AVSValue(fpsArgs, 11), fpsNames);
        } else {
            const AVSValue fpsArgs[] = {
                AVSValue(*m_sourceClip), AVSValue(*m_superClip),
                AVSValue(*m_mvBackwardClip), AVSValue(*m_mvForwardClip),
                AVSValue(static_cast<int>(fpsNum)), AVSValue(static_cast<int>(fpsDen)), AVSValue(2)
            };
            const char* fpsNames[] = {
                nullptr, nullptr, nullptr, nullptr, "num", "den", "mode"
            };
            fpsResult = m_env->Invoke("MBlockFps", AVSValue(fpsArgs, 7), fpsNames);
        }
        if (!fpsResult.IsClip()) {
            error = "MBlockFps did not return an AviSynth clip.";
            return false;
        }
        const VideoInfo& rawOutputVi = fpsResult.AsClip()->GetVideoInfo();
        const FrucRational expectedOutputRate = FrucNormalizeRational(fpsNum, fpsDen);
        const FrucRational actualOutputRate = FrucNormalizeRational(
            static_cast<int64_t>(rawOutputVi.fps_numerator),
            static_cast<int64_t>(rawOutputVi.fps_denominator));
        if (actualOutputRate.num != expectedOutputRate.num ||
            actualOutputRate.den != expectedOutputRate.den) {
            error = "MBlockFps returned " + std::to_string(actualOutputRate.num) + "/" +
                std::to_string(actualOutputRate.den) + " FPS instead of the requested rational " +
                std::to_string(expectedOutputRate.num) + "/" + std::to_string(expectedOutputRate.den) + ".";
            return false;
        }
        if (rawOutputVi.width != m_width || rawOutputVi.height != m_height || !rawOutputVi.IsYV12()) {
            error = "Raw MBlockFps returned an unexpected frame size or pixel format; expected same-size YV12.";
            return false;
        }
        m_rawFlowClip = std::make_unique<PClip>(fpsResult.AsClip());
        DirectMotionLog("INFO", "AviSynth/Pipeline",
            "Asynchronous AviSynth evaluation enabled: the first requested MBlockFps output is evaluated directly from the selected raw segment-local index; "
            "Prefetch is then created over Trim(raw, firstOutput + 1, 0), so its frame zero is the first successor and no covered prefix or bootstrap frame is scheduled again. "
            "Receive publishes normalized source surfaces into the shared InputClip cache; ProcessingWorker owns MBlockFps GetFrame requests; "
            "DeliveryWorker owns allocator acquisition/conversion/Deliver. sourceCacheFrames=" +
            std::to_string(sourceCacheFrames) + ", motionLookaheadFrames=" +
            std::to_string(kMotionLookaheadFrames) + ", source generation=" +
            std::to_string(sourceCacheGeneration) + ".");
        DirectMotionLog("INFO", "AviSynth/Scheduler",
            "Frame-parallel scheduler policy stored for frontier-relative startup: SetFilterMTMode(DEFAULT_MT_MODE,2), Prefetch threads=" +
            std::to_string(prefetchThreads) + ", framesRequested=" + std::to_string(requestedPrefetchFrames) +
            ", framesEffective=" + std::to_string(prefetchFrames) +
            ", framesMode=" + std::string(m_requestedPrefetchFrames == 0 ? "Auto(2xWorkers)" : "Manual") +
            ", sourceCacheFrames=" + std::to_string(sourceCacheFrames) +
            ", sourceLookaheadFrames=" + std::to_string(prefetchSourceLookaheadFrames) +
            ", bootstrap=first-uncovered-raw-MBlockFps-output, Prefetch=Trimmed-frontier-wrapper-created-at-bootstrap, delivery=AsyncProcessingWorker->AsyncDeliveryWorker, MVTools MSuper/MAnalyse mt=false (private AVSTP scheduling disabled).");
        DirectMotionLog("INFO", "AviSynth/MVTools",
            "Graph configured: profile=" + std::string(m_options.advanced ? "Advanced" :
                (m_performance == 1 ? "Quality" : (m_performance == 2 ? "Balanced" : "Fast"))) +
            ", advanced=" + std::to_string(m_options.advanced ? 1 : 0) +
            ", MAnalyseGridOverride=" + std::string(m_options.advanced ? "advanced" :
                (m_requestedGrid == 0 ? "profile-default" : "enabled")) +
            ", quickRecalcOverride=" + std::to_string(m_quickRecalcMode) +
            ", MSuper(pel=" + std::to_string(m_options.advanced ? m_options.superPel : 1) +
            ",sharp=" + std::to_string(m_options.advanced ? m_options.superSharp : 2) +
            ",rfilter=" + std::to_string(m_options.advanced ? m_options.superRFilter : 2) +
            ",levels=" + std::to_string(m_options.advanced ? m_options.superLevels : 0) +
            ",chroma=" + std::string(m_options.advanced ? (m_options.analyseChroma ? "1" : "0") : "default(true)") +
            ",hpad=0,vpad=0,mt=false) -> MAnalyse(" +
            std::to_string(blockSize) + "x" + std::to_string(blockSize) +
            ",search=" + std::to_string(search) + ",searchparam=" +
            std::to_string(searchParam) + ",badrange=" + std::to_string(badRange) +
            ",badSAD=10000(disabled)" +
            ",overlap=" + std::to_string(analyseOverlap) + ",chroma=" +
            std::to_string(analyseChroma ? 1 : 0) + ",truemotion=" +
            std::to_string(analyseTrueMotion ? 1 : 0) + ",mt=false) -> MRecalculate(" +
            std::to_string(recalcBlock) + "x" + std::to_string(recalcBlock) +
            ",search=" + std::to_string(recalcSearch) + ",searchparam=" +
            std::to_string(recalcSearchParam) + ",overlap=" + std::to_string(recalcOverlap) +
            ",chroma=" + std::to_string(recalcChroma ? 1 : 0) + ",truemotion=" +
            std::to_string(recalcTrueMotion ? 1 : 0) + ") -> MBlockFps(mode=" +
            std::to_string(m_options.advanced ? m_options.blockFpsMode : 2) +
            ",ml=" + std::to_string(m_options.advanced ? m_options.blockFpsMaskScale : 100.0) +
            ",blend=" + std::to_string(m_options.advanced ? m_options.blockFpsBlend : true) +
            ",thSCD1=" + std::to_string(m_options.advanced ? m_options.thSCD1 : 400) +
            ",thSCD2=" + std::to_string(m_options.advanced ? m_options.thSCD2 : 130) +
            ",num=" + std::to_string(fpsNum) + ",den=" + std::to_string(fpsDen) + ").");
        return true;
    } catch (const IScriptEnvironment::NotFound&) {
        error = "AviSynth Invoke could not match the arguments for graph stage '" + graphStage +
                "' to a registered function signature. Check function name, parameter names, and argument count.";
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    } catch (const AvisynthError& ex) {
        error = "Building graph stage '" + graphStage + "' failed: " + AvsErrorText(ex);
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    } catch (const std::exception& ex) {
        error = "Building graph stage '" + graphStage + "' raised std::exception: " + ex.what();
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    } catch (const std::string& ex) {
        error = "Building graph stage '" + graphStage + "' raised std::string: " + ex;
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    } catch (const char* ex) {
        error = "Building graph stage '" + graphStage + "' raised C-string exception: " +
                std::string(ex ? ex : "<null>");
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    } catch (...) {
        error = "Building graph stage '" + graphStage + "' failed with an unknown exception.";
        DirectMotionLog("ERROR", "AviSynth/Graph", error);
        return false;
    }
}

bool AviSynthMvToolsEngine::Initialize(int width, int height,
                                        FrucRational sourceRate, FrucRational targetRate,
                                        int performance, int grid, int cpuProfile, int customCpuWorkers,
                                        int quickRecalcMode, int requestedPrefetchFrames,
                                        const AviSynthMvToolsOptions& options,
                                        bool resumeWarmStart,
                                        const std::shared_ptr<AviSynthRuntimeSession>& runtimeSession,
                                        const std::shared_ptr<AviSynthEvaluationGate>& evaluationGate,
                                        std::string& error) {
    Reset();
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        error = "AviSynth MVTools bridge requires positive, even-sized 8-bit 4:2:0 frames.";
        return false;
    }
    if (!IsValidRate(sourceRate) || !IsValidRate(targetRate)) {
        error = "Invalid source or target frame rate for AviSynth MVTools.";
        return false;
    }
    if (static_cast<uint64_t>(sourceRate.num) > (std::numeric_limits<unsigned>::max)() ||
        static_cast<uint64_t>(sourceRate.den) > (std::numeric_limits<unsigned>::max)()) {
        error = "The source frame rate exceeds AviSynth's VideoInfo representation.";
        return false;
    }

    m_width = width;
    m_height = height;
    m_sourceRate = sourceRate;
    m_targetRate = targetRate;
    m_outputTimeBase = FrucNormalizeRational(targetRate.den, targetRate.num);
    m_performance = std::clamp(performance, 1, 3);
    m_requestedGrid = grid;
    m_cpuProfile = (cpuProfile >= kCpuProfileAuto && cpuProfile <= kCpuProfileCustom)
        ? cpuProfile : kCpuProfileAuto;
    m_customCpuWorkers = std::clamp(customCpuWorkers, 1, 32);
    m_quickRecalcMode = std::clamp(quickRecalcMode, 0, 1);
    m_resumeWarmStart = resumeWarmStart;
    const int validPrefetchValues[] = { 0, 8, 12, 16, 24, 32, 48, 64 };
    m_requestedPrefetchFrames = 0;
    for (const int value : validPrefetchValues) {
        if (requestedPrefetchFrames == value) { m_requestedPrefetchFrames = value; break; }
    }
    m_options = options;
    m_runtimeSession = runtimeSession;
    if (!m_runtimeSession) {
        error = "DirectMotion did not provide its persistent AviSynth runtime session.";
        Reset();
        return false;
    }
    const auto runtimeSessionStarted = std::chrono::steady_clock::now();
    bool runtimeSessionCreated = false;
    if (!m_runtimeSession->EnsureReady(error, &runtimeSessionCreated)) {
        Reset();
        return false;
    }
    m_env = m_runtimeSession->Environment();
    if (!m_env) {
        error = "The persistent AviSynth runtime session reported ready without an environment.";
        Reset();
        return false;
    }
    const double runtimeSessionMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - runtimeSessionStarted).count();
    const auto graphBuildStarted = std::chrono::steady_clock::now();
    if (!BuildGraph(evaluationGate, error)) {
        const double graphBuildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - graphBuildStarted).count();
        DirectMotionLog("WARN", "AviSynth/GraphTiming",
            "Graph construction failed; runtimeSession=" + std::string(runtimeSessionCreated ? "created" : "reused") +
            ", runtimeSessionMs=" + std::to_string(runtimeSessionMs) +
            ", graphBuildMs=" + std::to_string(graphBuildMs) + ".");
        Reset();
        return false;
    }
    const double graphBuildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - graphBuildStarted).count();
    DirectMotionLog("INFO", "AviSynth/GraphTiming",
        "Graph construction completed; runtimeSession=" + std::string(runtimeSessionCreated ? "created" : "reused") +
        ", runtimeSessionMs=" + std::to_string(runtimeSessionMs) +
        ", graphBuildMs=" + std::to_string(graphBuildMs) + ".");
    m_initialized = true;
    if (AviSynthGraphAnalysisEnabled()) {
        try {
            const std::wstring graphPath = GetModuleDirectory() +
                L"directmotion_avisynth_graph_" + std::to_wstring(GetCurrentProcessId()) + L".dot";
            const std::string graphPathUtf8 = WideToUtf8(graphPath);
            const AVSValue graphArgs[] = {
                AVSValue(*m_rawFlowClip), AVSValue(graphPathUtf8.c_str()), AVSValue(2)
            };
            (void)m_env->Invoke("DumpFilterGraph", AVSValue(graphArgs, 3));
            DirectMotionLog("INFO", "AviSynth/Diagnostics",
                "Raw MBlockFps graph dumped at " + graphPathUtf8 + "; frontier-relative Prefetch is created only when the seek watermark is sealed.");
        } catch (const AvisynthError& ex) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "DumpFilterGraph was unavailable or failed; continuing playback without graph dump: " +
                AvsErrorText(ex));
        } catch (...) {
            DirectMotionLog("WARN", "AviSynth/Diagnostics",
                "DumpFilterGraph failed with an unknown exception; continuing without graph dump.");
        }
    }
    DirectMotionLog("INFO", "AviSynth/MVTools",
        "Graph ready; sourceFPS=" + std::to_string(static_cast<double>(sourceRate.num) / sourceRate.den) +
        ", targetFPS=" + std::to_string(static_cast<double>(targetRate.num) / targetRate.den) +
        ", dimensions=" + std::to_string(width) + "x" + std::to_string(height) +
        ". CPU profile=" + CpuProfileName(m_cpuProfile) +
        "; selected CPU profile caps total scheduler threads and does not pin threads to specific logical processors.");
    return true;
}

bool AviSynthMvToolsEngine::BuildPrefetchFromOutputStart(
    int64_t startOutputIndex, std::string& error) {
    if (startOutputIndex < 0 || startOutputIndex >= (std::numeric_limits<int>::max)() - 1) {
        error = "Seek-frontier output index leaves no supported successor frame for the Prefetch scheduler.";
        return false;
    }
    if (!m_env || !m_rawFlowClip || !*m_rawFlowClip ||
        m_prefetchThreads < 1 || m_prefetchFrames < 1) {
        error = "Cannot build frontier-relative Prefetch without a valid raw MBlockFps clip and scheduler plan.";
        return false;
    }
    if (m_flowClip) {
        error = "Frontier-relative Prefetch was already constructed for this engine generation.";
        return false;
    }

    try {
        const AVSValue trimArgs[] = {
            AVSValue(*m_rawFlowClip),
            AVSValue(static_cast<int>(startOutputIndex + 1)),
            AVSValue(0)
        };
        AVSValue trimResult = m_env->Invoke("Trim", AVSValue(trimArgs, 3));
        if (!trimResult.IsClip()) {
            error = "AviSynth Trim did not return a clip for the selected seek frontier.";
            return false;
        }
        const VideoInfo& trimmedVi = trimResult.AsClip()->GetVideoInfo();
        const VideoInfo& rawVi = (*m_rawFlowClip)->GetVideoInfo();
        if (trimmedVi.width != m_width || trimmedVi.height != m_height || !trimmedVi.IsYV12() ||
            trimmedVi.fps_numerator != rawVi.fps_numerator ||
            trimmedVi.fps_denominator != rawVi.fps_denominator ||
            trimmedVi.num_frames <= 0) {
            error = "Trimmed MBlockFps frontier clip changed dimensions, pixel format, cadence or has no frames.";
            return false;
        }

        const AVSValue prefetchArgs[] = {
            AVSValue(trimResult.AsClip()), AVSValue(m_prefetchThreads), AVSValue(m_prefetchFrames)
        };
        AVSValue prefetchResult = m_env->Invoke("Prefetch", AVSValue(prefetchArgs, 3));
        if (!prefetchResult.IsClip()) {
            error = "AviSynth+ Prefetch did not return a frontier-relative clip.";
            return false;
        }
        const VideoInfo& outputVi = prefetchResult.AsClip()->GetVideoInfo();
        if (outputVi.width != m_width || outputVi.height != m_height || !outputVi.IsYV12() ||
            outputVi.fps_numerator != rawVi.fps_numerator ||
            outputVi.fps_denominator != rawVi.fps_denominator || outputVi.num_frames <= 0) {
            error = "Frontier-relative Prefetch changed the negotiated MBlockFps dimensions, format, cadence or frame range.";
            return false;
        }
        m_flowClip = std::make_unique<PClip>(prefetchResult.AsClip());
        m_outputStartIndex = startOutputIndex;
        m_firstPrefetchOutputIndex = startOutputIndex + 1;
        DirectMotionLog("INFO", "AviSynth/SeekFrontier",
            "Created Prefetch over Trim(raw MBlockFps, firstPrefetchOutputIndex, 0); Prefetch-relative frame zero is the first output after the direct frontier bootstrap. "
            "outputStartIndex=" + std::to_string(startOutputIndex) +
            ", firstPrefetchOutputIndex=" + std::to_string(m_firstPrefetchOutputIndex) +
            ", prefetchThreads=" + std::to_string(m_prefetchThreads) +
            ", prefetchFrames=" + std::to_string(m_prefetchFrames) +
            ", trimmedFrames=" + std::to_string(trimmedVi.num_frames) + ".");
        return true;
    } catch (const IScriptEnvironment::NotFound&) {
        error = "AviSynth could not resolve Trim or Prefetch while constructing the seek-frontier scheduler.";
    } catch (const AvisynthError& ex) {
        error = "AviSynth failed to construct Prefetch from the selected Trim frontier: " + AvsErrorText(ex);
    } catch (const std::exception& ex) {
        error = std::string("Frontier-relative Prefetch construction raised std::exception: ") + ex.what();
    } catch (...) {
        error = "Frontier-relative Prefetch construction failed with an unknown exception.";
    }
    DirectMotionLog("ERROR", "AviSynth/SeekFrontier", error);
    return false;
}

bool AviSynthMvToolsEngine::CanEmitOutputIndex(int64_t outputIndex) const noexcept {
    if (!IsInitialized() || outputIndex < 0 || outputIndex >= (std::numeric_limits<int>::max)())
        return false;
    if (m_flushed.load(std::memory_order_acquire))
        return outputIndex < m_expectedOutputCount.load(std::memory_order_acquire);

    return DirectMotionSeekFrontier::HasSourceLookaheadForOutputIndex(
        outputIndex, m_inputFrameCount.load(std::memory_order_acquire),
        kMotionLookaheadFrames, m_sourceRate, m_targetRate);
}

bool AviSynthMvToolsEngine::PrimeSourceCacheForOutputIndex(
    int64_t outputIndex, std::string& error) {
    if (!IsInitialized() || !m_input) {
        error = "Cannot prime source-cache retention without an initialized AviSynth input clip.";
        return false;
    }
    if (outputIndex < 0 || outputIndex >= (std::numeric_limits<int>::max)()) {
        error = "Seek-frontier output index is outside the supported source-cache anchor range.";
        return false;
    }
    if (m_sourceRetentionPrimedForOutputIndex == outputIndex) return true;

    int64_t sourceFrameAnchor = -1;
    if (!DirectMotionSeekFrontier::TrySourceFrameAnchorForOutputIndex(
            outputIndex, m_sourceRate, m_targetRate, sourceFrameAnchor)) {
        error = "Could not map the selected MBlockFps frontier to a bounded source-cache retention anchor.";
        return false;
    }

    m_input->AdvanceConsumerSourceRetentionAnchor(sourceFrameAnchor);
    m_sourceRetentionPrimedForOutputIndex = outputIndex;
    DirectMotionLog("INFO", "AviSynth/SeekFrontier",
        "Primed the bounded source-cache history window for direct frontier evaluation; source frames before the retained history floor are no longer needed by the selected output. "
        "outputIndex=" + std::to_string(outputIndex) +
        ", sourceFrameAnchor=" + std::to_string(sourceFrameAnchor) + ".");
    return true;
}

bool AviSynthMvToolsEngine::EmitFrame(int outputIndex,
                                       std::vector<OutputFrame>& output,
                                       std::string& error,
                                       bool forceRawMBlockFps) {
    const bool trace = TraceFrameRequestsEnabled();
    const auto noTime = std::chrono::steady_clock::time_point{};
    const auto totalStarted = trace ? std::chrono::steady_clock::now() : noTime;
    const bool usePrefetch = !forceRawMBlockFps &&
        DirectMotionBootstrapEvaluation::UsesPrefetchForOutput(outputIndex, m_firstPrefetchOutputIndex);
    const int clipFrameIndex = usePrefetch
        ? static_cast<int>(DirectMotionBootstrapEvaluation::PrefetchRelativeIndex(outputIndex, m_firstPrefetchOutputIndex))
        : outputIndex;
    if (trace) {
        DirectMotionLog("TRACE", "AviSynth/OutputFrame",
            "BEGIN MBlockFps.GetFrame outputIndex=" + std::to_string(outputIndex) +
            ", clipFrameIndex=" + std::to_string(clipFrameIndex) +
            ", evaluationPath=" + std::string(usePrefetch ? "frontier-relative-Prefetch" : "raw-direct-bootstrap") +
            ", outputStartIndex=" + std::to_string(m_outputStartIndex) +
            "; if no matching END appears, evaluation did not return to directmotion.");
    }
    try {
        PClip* selectedClip = usePrefetch ? m_flowClip.get() : m_rawFlowClip.get();
        if (!selectedClip || !*selectedClip) {
            error = usePrefetch
                ? "Frontier-relative Prefetch output clip is unavailable for a non-bootstrap frame."
                : "Raw MBlockFps output clip is unavailable for the selected bootstrap frame.";
            return false;
        }
        const auto graphStarted = trace ? std::chrono::steady_clock::now() : noTime;
        PVideoFrame frame = (*selectedClip)->GetFrame(clipFrameIndex, m_env);
        double graphGetFrameMs = 0.0;
        if (trace) {
            graphGetFrameMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - graphStarted).count();
        }
        const auto conversionStarted = trace ? std::chrono::steady_clock::now() : noTime;
        const size_t yBytes = static_cast<size_t>(m_width) * m_height;
        const size_t chromaBytes = yBytes / 4;
        OutputFrame out;
        out.pts = outputIndex;
        out.outputLineSizeY = m_width;
        out.outputLineSizeUV = m_width;
        out.nv12.resize(yBytes + 2 * chromaBytes);
        uint8_t* dstY = out.nv12.data();
        uint8_t* dstUV = out.nv12.data() + yBytes;
        const BYTE* srcY = frame->GetReadPtr(PLANAR_Y);
        const BYTE* srcU = frame->GetReadPtr(PLANAR_U);
        const BYTE* srcV = frame->GetReadPtr(PLANAR_V);
        CopyPlane(dstY, m_width, srcY, frame->GetPitch(PLANAR_Y), m_width, m_height);
        const int chromaWidth = m_width / 2;
        for (int y = 0; y < m_height / 2; ++y) {
            const uint8_t* u = srcU + static_cast<size_t>(y) * frame->GetPitch(PLANAR_U);
            const uint8_t* v = srcV + static_cast<size_t>(y) * frame->GetPitch(PLANAR_V);
            uint8_t* uv = dstUV + static_cast<size_t>(y) * m_width;
            InterleaveChromaRow(uv, u, v, chromaWidth);
        }
        output.emplace_back(std::move(out));
        if (m_input) {
            const long double sourceProgress =
                (static_cast<long double>(outputIndex) + 1.0L) *
                static_cast<long double>(m_sourceRate.num) *
                static_cast<long double>(m_targetRate.den) /
                (static_cast<long double>(m_sourceRate.den) *
                 static_cast<long double>(m_targetRate.num));
            if (std::isfinite(static_cast<double>(sourceProgress)) && sourceProgress >= 0.0L &&
                sourceProgress <= static_cast<long double>((std::numeric_limits<int64_t>::max)())) {
                m_input->AdvanceConsumerSourceRetentionAnchor(static_cast<int64_t>(std::floor(sourceProgress)));
            }
        }
        if (trace) {
            const double conversionMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - conversionStarted).count();
            const double totalMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - totalStarted).count();
            DirectMotionLog("TRACE", "AviSynth/OutputFrame",
                "END outputIndex=" + std::to_string(outputIndex) +
                ", graphGetFrameMs=" + std::to_string(graphGetFrameMs) +
                ", planarToNv12Ms=" + std::to_string(conversionMs) +
                ", totalMs=" + std::to_string(totalMs));
        }
        return true;
    } catch (const AvisynthError& ex) {
        error = "AviSynth failed while retrieving MBlockFps frame " +
                std::to_string(outputIndex) + ": " + AvsErrorText(ex);
        if (trace) DirectMotionLog("ERROR", "AviSynth/OutputFrame",
            "FAILED outputIndex=" + std::to_string(outputIndex) +
            ", elapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - totalStarted).count()) +
            ", detail=" + error);
        return false;
    } catch (const std::bad_alloc&) {
        error = "Out of memory while copying an AviSynth output frame.";
        if (trace) DirectMotionLog("ERROR", "AviSynth/OutputFrame",
            "FAILED outputIndex=" + std::to_string(outputIndex) + ", reason=bad_alloc");
        return false;
    } catch (...) {
        error = "Unknown exception while retrieving an AviSynth output frame.";
        if (trace) DirectMotionLog("ERROR", "AviSynth/OutputFrame",
            "FAILED outputIndex=" + std::to_string(outputIndex) + ", reason=unknown-exception");
        return false;
    }
}

DirectMotionSourceCache::PushResult AviSynthMvToolsEngine::PushSourceNv12(
    const uint8_t* data, int sourceStride, int64_t inputPts, std::string& error,
    void* pressureContext, SourceCachePressureCallback pressureCallback) {
    if (!IsInitialized() || m_flushed.load(std::memory_order_acquire)) {
        error = "AviSynth MVTools source is not active or has already been finalized.";
        return DirectMotionSourceCache::PushResult::Failed;
    }
    const int64_t expected = m_inputFrameCount.load(std::memory_order_acquire);
    if (inputPts != expected) {
        error = "AviSynth bridge expects sequential input frame indices; got " +
                std::to_string(inputPts) + ", expected " + std::to_string(expected) + ".";
        return DirectMotionSourceCache::PushResult::Failed;
    }
    if (expected >= kSyntheticClipFrameCount - 2) {
        error = "AviSynth bridge's configured virtual clip length has been exhausted.";
        return DirectMotionSourceCache::PushResult::Failed;
    }
    if (!m_input) {
        error = "AviSynth input bridge is unavailable.";
        return DirectMotionSourceCache::PushResult::Failed;
    }
    const DirectMotionSourceCache::PushResult result = m_input->PushNv12(
        data, sourceStride, m_env, error, pressureContext, pressureCallback);
    if (result != DirectMotionSourceCache::PushResult::Published) return result;
    m_inputFrameCount.store(expected + 1, std::memory_order_release);
    return DirectMotionSourceCache::PushResult::Published;
}

bool AviSynthMvToolsEngine::TryRebindInitialSourceGeneration(
    uint64_t expectedPreviousEpoch, uint64_t newSourceInterruptEpoch,
    bool pipelineGenerationMatches, bool noFilterInputPublished,
    bool noFilterOutputEmitted, std::string& error) {
    if (!m_initialized || !m_input || !m_rawFlowClip || !m_sourceClip) {
        error = "Initial source-generation rebinding requires a fully initialized AviSynth graph and InputClip.";
        return false;
    }

    const bool noEngineInputAccepted =
        m_inputFrameCount.load(std::memory_order_acquire) == 0;
    const bool noEngineOutputEmitted =
        m_expectedOutputCount.load(std::memory_order_acquire) == 0 &&
        m_nextOutputIndex == 0 && !m_bootstrapEmitted &&
        m_outputStartIndex == 0 && m_sourceRetentionPrimedForOutputIndex == -1 &&
        !m_flowClip && !m_flushed.load(std::memory_order_acquire);
    if (!noEngineInputAccepted || !noEngineOutputEmitted) {
        error = "Initial source-generation rebinding rejected: the AviSynth engine has already accepted source input, advanced output state, activated Prefetch, or finalized the source. inputFrames=" +
            std::to_string(m_inputFrameCount.load(std::memory_order_acquire)) +
            ", expectedOutputs=" + std::to_string(m_expectedOutputCount.load(std::memory_order_acquire)) +
            ", nextOutput=" + std::to_string(m_nextOutputIndex) +
            ", bootstrapEmitted=" + std::to_string(m_bootstrapEmitted ? 1 : 0) +
            ", prefetchActive=" + std::to_string(m_flowClip ? 1 : 0) +
            ", flushed=" + std::to_string(m_flushed.load(std::memory_order_acquire) ? 1 : 0) + ".";
        return false;
    }

    return m_input->TryRebindPristineInitialGeneration(
        expectedPreviousEpoch, newSourceInterruptEpoch,
        pipelineGenerationMatches, noFilterInputPublished, noFilterOutputEmitted,
        noEngineInputAccepted, noEngineOutputEmitted, error);
}

bool AviSynthMvToolsEngine::EmitBootstrapFrame(
    int64_t startOutputIndex, OutputFrame& output, std::string& error) {
    if (!IsInitialized()) {
        error = "AviSynth MVTools graph is not initialized for frontier bootstrap.";
        return false;
    }
    if (m_bootstrapEmitted || m_nextOutputIndex != 0 ||
        startOutputIndex < 0 || startOutputIndex >= (std::numeric_limits<int>::max)() ||
        !CanEmitOutputIndex(startOutputIndex)) {
        error = "Seek-frontier bootstrap was requested twice, outside the supported range, or before its MVTools source lookahead was available.";
        return false;
    }
    m_outputStartIndex = startOutputIndex;
    if (!BuildPrefetchFromOutputStart(startOutputIndex, error)) {
        m_outputStartIndex = 0;
        m_firstPrefetchOutputIndex = 1;
        return false;
    }
    std::vector<OutputFrame> frames;
    if (!EmitFrame(static_cast<int>(startOutputIndex), frames, error, true)) return false;
    if (frames.size() != 1) {
        error = "Raw MBlockFps seek-frontier bootstrap did not return exactly one frame.";
        return false;
    }
    output = std::move(frames.front());
    m_nextOutputIndex = startOutputIndex + 1;
    m_bootstrapEmitted = true;
    DirectMotionLog("INFO", "AviSynth/SeekFrontier",
        "Directly evaluated the first uncovered MBlockFps output and advanced the engine cursor to its successor. "
        "outputStartIndex=" + std::to_string(startOutputIndex) +
        ", nextOutputIndex=" + std::to_string(m_nextOutputIndex) + ".");
    return true;
}

bool AviSynthMvToolsEngine::EmitNextAvailable(OutputFrame& output, bool& available,
                                               bool& finished, std::string& error) {
    available = false;
    finished = false;
    if (!IsInitialized()) {
        error = "AviSynth MVTools graph is not initialized.";
        return false;
    }
    if (!m_bootstrapEmitted) {
        const int64_t initialIndex = 0;
        if (m_flushed.load(std::memory_order_acquire) &&
            initialIndex >= m_expectedOutputCount.load(std::memory_order_acquire)) {
            finished = true;
            return true;
        }
        if (!CanEmitOutputIndex(initialIndex)) return true;
        if (!EmitBootstrapFrame(initialIndex, output, error)) return false;
        available = true;
        return true;
    }
    const bool finalized = m_flushed.load(std::memory_order_acquire);
    const int64_t expectedOutputCount = m_expectedOutputCount.load(std::memory_order_acquire);
    if (finalized && m_nextOutputIndex >= expectedOutputCount) {
        finished = true;
        return true;
    }
    if (!finalized) {
        const int64_t newestInputIndex = m_inputFrameCount.load(std::memory_order_acquire) - 1;
        const int64_t safeInputIndex = newestInputIndex - kMotionLookaheadFrames;
        if (safeInputIndex < 0) return true;
        const int64_t lhs = m_nextOutputIndex * static_cast<int64_t>(m_sourceRate.num) * m_targetRate.den;
        const int64_t rhs = safeInputIndex * m_sourceRate.den * m_targetRate.num;
        if (lhs > rhs) return true;
    }
    if (m_nextOutputIndex < 0 || m_nextOutputIndex >= (std::numeric_limits<int>::max)()) {
        error = "AviSynth output frame index exceeded the supported range.";
        return false;
    }
    std::vector<OutputFrame> frames;
    if (!EmitFrame(static_cast<int>(m_nextOutputIndex), frames, error)) return false;
    if (frames.size() != 1) {
        error = "AviSynth output request did not return exactly one frame.";
        return false;
    }
    output = std::move(frames.front());
    ++m_nextOutputIndex;
    available = true;
    return true;
}

bool AviSynthMvToolsEngine::FinalizeSource(std::string& error) {
    if (!IsInitialized()) {
        error = "Cannot finalize an uninitialized AviSynth graph.";
        return false;
    }
    if (m_flushed.load(std::memory_order_acquire)) return true;
    const int64_t inputFrames = m_inputFrameCount.load(std::memory_order_acquire);
    if (inputFrames == 0) {
        m_expectedOutputCount.store(0, std::memory_order_release);
        m_flushed.store(true, std::memory_order_release);
        return true;
    }
    if (!m_input || !m_input->CompleteEndOfStream(error)) return false;
    const long double scaledCount = static_cast<long double>(inputFrames) *
        static_cast<long double>(m_targetRate.num) * m_sourceRate.den /
        (static_cast<long double>(m_targetRate.den) * m_sourceRate.num);
    if (!std::isfinite(static_cast<double>(scaledCount)) || scaledCount < 0.0L ||
        scaledCount > static_cast<long double>((std::numeric_limits<int>::max)())) {
        error = "AviSynth output frame count exceeds the supported index range.";
        return false;
    }
    m_expectedOutputCount.store(static_cast<int64_t>(std::floor(scaledCount + 0.5L)), std::memory_order_release);
    m_flushed.store(true, std::memory_order_release);
    return true;
}

void AviSynthMvToolsEngine::InterruptSourceWaits() noexcept {
    if (m_input) m_input->InterruptSourceWaits();
}


void AviSynthMvToolsEngine::Reset() noexcept {
    if (m_input) m_input->InterruptSourceWaits();
    m_flowClip.reset();
    m_rawFlowClip.reset();
    m_mvForwardClip.reset();
    m_mvBackwardClip.reset();
    m_superClip.reset();
    m_sourceClip.reset();
    m_input = nullptr;
    m_env = nullptr;
    m_runtimeSession.reset();
    m_width = 0;
    m_height = 0;
    m_sourceRate = FrucRational{24, 1};
    m_targetRate = FrucRational{48, 1};
    m_outputTimeBase = FrucRational{1, 48};
    m_bootstrapInputFramesRequired = DirectMotionBootstrapEvaluation::RequiredSourceFramesForFirstOutput(kMotionLookaheadFrames);
    m_prefetchThreads = 1;
    m_prefetchFrames = 1;
    m_outputStartIndex = 0;
    m_firstPrefetchOutputIndex = 1;
    m_sourceRetentionPrimedForOutputIndex = -1;
    m_bootstrapEmitted = false;
    m_resumeWarmStart = false;
    m_requestedPrefetchFrames = 0;
    m_inputFrameCount.store(0, std::memory_order_release);
    m_nextOutputIndex = 0;
    m_expectedOutputCount.store(0, std::memory_order_release);
    m_initialized = false;
    m_flushed.store(false, std::memory_order_release);
}
