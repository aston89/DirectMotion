#include <windows.h>
#include <streams.h>
#include <initguid.h>
#include <dvdmedia.h>
#include <evcode.h>
#include <mfidl.h>
#include <evr.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <limits>
#include <string>
#include <utility>

#include "uids.h"
#include "resource.h"
#include "directmotion.h"
#include "directmotionSettings.h"
#include "RuntimeDiagnostics.h"
#include "PipelineStallDiagnostics.h"
#include "DeliveryLatencyDiagnostics.h"
#include "ProcessingContinuation.h"
#include "ProcessingQuiescence.h"
#include "ProcessingGenerationPolicy.h"
#include "SampleTimePolicy.h"
#include "EngineBootstrapCommitPolicy.h"
#include "SeekFrontierPolicy.h"
#include "InitialSegmentGraphPolicy.h"
#include "InitialSegmentStagingPolicy.h"
#include <vector>


namespace {

constexpr REFERENCE_TIME kReferenceTimeUnitsPerSecond = 10000000;
// A timestamp jump larger than this is a timeline discontinuity, not frame stepping.
// Use stream timestamps (100 ns units), not the difference between frame indices.
constexpr REFERENCE_TIME kSeekTimestampJumpThreshold = 2 * kReferenceTimeUnitsPerSecond;
constexpr unsigned long long kStepAuditMaxFlushStacks = 16;

bool EnvironmentFlagEnabled(const wchar_t* name) noexcept {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(name, value,
        static_cast<DWORD>(_countof(value)));
    return length == 1 && value[0] == L'1';
}

// The normal diagnostic switch runs a bounded frame-step audit only. Full per-frame
// allocator/AviSynth traces remain available under a separate, intentionally noisy flag.
bool DirectShowTraceEnabled() noexcept {
    static const bool enabled = EnvironmentFlagEnabled(L"DIRECTMOTION_VERBOSE_TRACE");
    return enabled;
}

bool DirectShowFullTraceEnabled() noexcept {
    static const bool enabled = EnvironmentFlagEnabled(L"DIRECTMOTION_FULL_TRACE");
    return enabled;
}

uint64_t MonotonicTickNanoseconds() noexcept {
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return ticks > 0 ? static_cast<uint64_t>(ticks) : 0;
}

// A bounded, allocation-free timing accumulator for diagnostic hot paths.
// The timer is inert when file logging is disabled; when enabled it performs
// only monotonic-clock reads and relaxed diagnostic accounting on scope exit.
class ScopedDiagnosticTimer final {
public:
    ScopedDiagnosticTimer(bool enabled,
        std::atomic<uint64_t>& samples,
        std::atomic<uint64_t>& totalNs,
        std::atomic<uint64_t>& lastNs,
        std::atomic<uint64_t>& maxNs) noexcept
        : m_enabled(enabled), m_samples(samples), m_totalNs(totalNs),
          m_lastNs(lastNs), m_maxNs(maxNs),
          m_startedNs(enabled ? MonotonicTickNanoseconds() : 0) {}

    ScopedDiagnosticTimer(const ScopedDiagnosticTimer&) = delete;
    ScopedDiagnosticTimer& operator=(const ScopedDiagnosticTimer&) = delete;

    ~ScopedDiagnosticTimer() noexcept {
        if (!m_enabled) return;
        const uint64_t endedNs = MonotonicTickNanoseconds();
        const uint64_t elapsedNs = endedNs >= m_startedNs ? endedNs - m_startedNs : 0;
        m_lastNs.store(elapsedNs, std::memory_order_relaxed);
        m_totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
        uint64_t observedMax = m_maxNs.load(std::memory_order_relaxed);
        while (elapsedNs > observedMax &&
            !m_maxNs.compare_exchange_weak(observedMax, elapsedNs,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        // Publish the completed sample after its associated duration values so
        // telemetry can acquire the count before reading the aggregate snapshot.
        m_samples.fetch_add(1, std::memory_order_release);
    }

private:
    const bool m_enabled;
    std::atomic<uint64_t>& m_samples;
    std::atomic<uint64_t>& m_totalNs;
    std::atomic<uint64_t>& m_lastNs;
    std::atomic<uint64_t>& m_maxNs;
    const uint64_t m_startedNs;
};

bool ConsumeTraceBudget(std::atomic<int>& remaining) noexcept {
    int value = remaining.load(std::memory_order_acquire);
    while (value > 0) {
        if (remaining.compare_exchange_weak(value, value - 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
    return false;
}

std::atomic<unsigned long long> g_transitionSequence{ 0 };
std::atomic<unsigned long long> g_flushSequence{ 0 };
std::atomic<unsigned long long> g_segmentSequence{ 0 };
std::atomic<unsigned long long> g_allocatorSequence{ 0 };
std::atomic<unsigned long long> g_stepAuditFlushStackCount{ 0 };

// EVR exposes its display-control interface through IMFGetService. Keep the
// service GUID local so this module does not acquire a new Media Foundation
// import-library dependency merely to query the renderer's video HWND.
constexpr GUID kMrVideoRenderService = {
    0x1092a86c, 0xab1a, 0x459a, { 0xa3, 0x36, 0x83, 0x1f, 0xbc, 0x4d, 0x11, 0xff }
};

struct ProcessWindowSearch {
    DWORD processId = 0;
    HWND bestWindow = nullptr;
    long long bestArea = 0;
};

BOOL CALLBACK FindLargestProcessWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ProcessWindowSearch*>(parameter);
    if (!search || !IsWindowVisible(window)) return TRUE;
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    const long long width = (std::max)(0L, rect.right - rect.left);
    const long long height = (std::max)(0L, rect.bottom - rect.top);
    const long long area = width * height;
    if (area > search->bestArea) {
        search->bestArea = area;
        search->bestWindow = window;
    }
    return TRUE;
}

bool TryGetEvrVideoWindow(IBaseFilter* filter, HWND& videoWindow) {
    videoWindow = nullptr;
    if (!filter) return false;
    IMFGetService* serviceProvider = nullptr;
    HRESULT hr = filter->QueryInterface(__uuidof(IMFGetService),
        reinterpret_cast<void**>(&serviceProvider));
    if (FAILED(hr) || !serviceProvider) return false;

    IMFVideoDisplayControl* displayControl = nullptr;
    hr = serviceProvider->GetService(kMrVideoRenderService, __uuidof(IMFVideoDisplayControl),
        reinterpret_cast<void**>(&displayControl));
    serviceProvider->Release();
    if (FAILED(hr) || !displayControl) return false;

    hr = displayControl->GetVideoWindow(&videoWindow);
    displayControl->Release();
    if (FAILED(hr) || !videoWindow || !IsWindow(videoWindow)) {
        videoWindow = nullptr;
        return false;
    }
    return true;
}

bool TryGetRendererVideoWindow(IPin* outputPin, IFilterGraph* graph,
                               HWND& videoWindow, std::string& source) {
    videoWindow = nullptr;
    source.clear();

    // Prefer the actual connected downstream filter when DirectShow has already
    // completed that part of graph construction.
    if (outputPin) {
        IPin* downstreamPin = nullptr;
        if (SUCCEEDED(outputPin->ConnectedTo(&downstreamPin)) && downstreamPin) {
            PIN_INFO pinInfo{};
            if (SUCCEEDED(downstreamPin->QueryPinInfo(&pinInfo)) && pinInfo.pFilter) {
                const bool found = TryGetEvrVideoWindow(pinInfo.pFilter, videoWindow);
                pinInfo.pFilter->Release();
                if (found) {
                    source = "connected EVR renderer window";
                    downstreamPin->Release();
                    return true;
                }
            }
            downstreamPin->Release();
        }
    }

    // During initial GetMediaType negotiation the output pin may not yet be
    // connected. The renderer is normally already present in the graph, so query
    // its EVR service before falling back to the foreground/primary monitor.
    if (graph) {
        IEnumFilters* enumerator = nullptr;
        if (SUCCEEDED(graph->EnumFilters(&enumerator)) && enumerator) {
            IBaseFilter* filter = nullptr;
            ULONG fetched = 0;
            while (enumerator->Next(1, &filter, &fetched) == S_OK && filter) {
                const bool found = TryGetEvrVideoWindow(filter, videoWindow);
                filter->Release();
                filter = nullptr;
                if (found) {
                    source = "EVR renderer window discovered in filter graph";
                    enumerator->Release();
                    return true;
                }
            }
            enumerator->Release();
        }
    }

    // Some EVR presenters do not expose their HWND until after renderer setup.
    // Prefer a foreground window belonging to this process; never accidentally
    // select an unrelated application's monitor just because it is topmost.
    HWND foreground = GetForegroundWindow();
    if (foreground && IsWindow(foreground) && IsWindowVisible(foreground)) {
        DWORD foregroundProcess = 0;
        (void)GetWindowThreadProcessId(foreground, &foregroundProcess);
        if (foregroundProcess == GetCurrentProcessId()) {
            videoWindow = foreground;
            source = "foreground process-window fallback (renderer HWND unavailable)";
            return true;
        }
    }

    // The player may be visible but temporarily not foregrounded. In that case,
    // use the largest visible top-level window owned by this process as a better
    // approximation than an unrelated foreground application.
    ProcessWindowSearch search{};
    search.processId = GetCurrentProcessId();
    (void)EnumWindows(FindLargestProcessWindow, reinterpret_cast<LPARAM>(&search));
    if (search.bestWindow && IsWindow(search.bestWindow)) {
        videoWindow = search.bestWindow;
        source = "largest visible process-window fallback (renderer HWND unavailable)";
        return true;
    }
    source = "primary-monitor fallback (renderer HWND unavailable)";
    return false;
}

bool QueryExactMonitorRefresh(HMONITOR monitor, FrucRational& refresh,
                              std::string& description) {
    refresh = FrucRational{ 0, 1 };
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo)))
        return false;

    // QueryDisplayConfig preserves fractional modes (for example 60000/1001),
    // unlike DEVMODE.dmDisplayFrequency which is typically rounded to an integer.
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS || pathCount == 0) break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes((std::max)(1u, modeCount));
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
            &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) break;
        paths.resize(pathCount);

        for (const auto& path : paths) {
            if (!path.targetInfo.targetAvailable ||
                path.targetInfo.refreshRate.Numerator == 0 ||
                path.targetInfo.refreshRate.Denominator == 0) continue;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) continue;
            if (_wcsicmp(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) continue;

            refresh = FrucNormalizeRational(
                static_cast<int64_t>(path.targetInfo.refreshRate.Numerator),
                static_cast<int64_t>(path.targetInfo.refreshRate.Denominator));
            // Reject invalid/sentinel-like display modes before they can become
            // an output FPS or overflow AviSynth VideoInfo's unsigned fields.
            if (refresh.num <= 0 || refresh.den <= 0 ||
                refresh.num < 20 * refresh.den || refresh.num > 1000 * refresh.den ||
                refresh.num > (std::numeric_limits<int>::max)() ||
                refresh.den > (std::numeric_limits<int>::max)()) {
                refresh = FrucRational{ 0, 1 };
                continue;
            }
            char deviceName[96]{};
            const int written = WideCharToMultiByte(CP_UTF8, 0, monitorInfo.szDevice, -1,
                deviceName, static_cast<int>(_countof(deviceName)), nullptr, nullptr);
            description = "QueryDisplayConfig " + std::to_string(refresh.num) + "/" +
                std::to_string(refresh.den) + " Hz on " +
                (written > 1 ? std::string(deviceName) : std::string("selected monitor"));
            return true;
        }
        break;
    }

    // Last-resort integer fallback only. This intentionally comes after
    // QueryDisplayConfig so common 59.94/119.88 Hz modes keep their exact ratio.
    DEVMODEW displayMode{};
    displayMode.dmSize = sizeof(displayMode);
    if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode) &&
        displayMode.dmDisplayFrequency >= 20 && displayMode.dmDisplayFrequency <= 1000) {
        refresh = FrucRational{ static_cast<int64_t>(displayMode.dmDisplayFrequency), 1 };
        description = "DEVMODE integer fallback " + std::to_string(displayMode.dmDisplayFrequency) +
            " Hz (fractional refresh unavailable)";
        return true;
    }
    return false;
}

FrucRational ResolveActiveDisplayRefresh(IPin* outputPin, IFilterGraph* graph,
                                         std::string& description) noexcept {
    try {
        HWND videoWindow = nullptr;
        std::string windowSource;
        const bool rendererWindowFound = TryGetRendererVideoWindow(outputPin, graph,
            videoWindow, windowSource);
        const HMONITOR monitor = MonitorFromWindow(videoWindow, MONITOR_DEFAULTTOPRIMARY);
        FrucRational refresh{ 0, 1 };
        std::string refreshDescription;
        if (!QueryExactMonitorRefresh(monitor, refresh, refreshDescription)) {
            description = windowSource + "; no active refresh rate could be queried";
            return FrucRational{ 0, 1 };
        }
        description = windowSource + "; " + refreshDescription;
        if (!rendererWindowFound && windowSource.find("primary-monitor") != std::string::npos)
            description += "; using Windows primary display as fallback";
        return refresh;
    } catch (...) {
        description = "display refresh detection failed with an internal exception";
        return FrucRational{ 0, 1 };
    }
}

std::string RateModeName(int mode) {
    switch (mode) {
    case kRateMode2x: return "2x";
    case kRateMode25x: return "2.5x";
    case kRateMode3x: return "3x";
    case kRateMode4x: return "4x";
    case kRateModeAuto: return "Auto/display-refresh";
    default: return "invalid";
    }
}

std::string WidePathToUtf8(const wchar_t* path) noexcept {
    if (!path || !*path) return "<module-path-unavailable>";
    try {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return "<module-path-conversion-failed>";
        std::string result(static_cast<size_t>(needed), '\0');
        const int written = WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), needed, nullptr, nullptr);
        if (written <= 1) return "<module-path-conversion-failed>";
        result.resize(static_cast<size_t>(written - 1));
        return result;
    } catch (...) {
        return "<module-path-conversion-exception>";
    }
}

// Capture the caller above a DirectShow transition callback. This is a best-effort
// module/offset trace, not a symbolic stack trace (no PDB dependency).
std::string CaptureTransitionCallerStack() noexcept {
    try {
        void* frames[10]{};
        const USHORT count = CaptureStackBackTrace(3, static_cast<DWORD>(_countof(frames)), frames, nullptr);
        std::ostringstream out;
        for (USHORT i = 0; i < count; ++i) {
            if (i != 0) out << " | ";
            const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(frames[i], &info, sizeof(info)) == sizeof(info) && info.AllocationBase) {
                wchar_t modulePath[32768]{};
                const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase),
                    modulePath, static_cast<DWORD>(_countof(modulePath)));
                out << "frame" << i << '=';
                if (length > 0 && length < _countof(modulePath)) out << WidePathToUtf8(modulePath);
                else out << "<module-path-unavailable>";
                out << "+0x" << std::hex << (address - reinterpret_cast<std::uintptr_t>(info.AllocationBase)) << std::dec;
            } else {
                out << "frame" << i << "=0x" << std::hex << address << std::dec;
            }
        }
        if (count == 0) out << "<no-frames-captured>";
        return out.str();
    } catch (...) {
        return "<stack-capture-failed>";
    }
}

void LogTransitionCaller(const char* method, unsigned long long transitionId) noexcept {
    if (!DirectShowTraceEnabled() && !DirectShowFullTraceEnabled()) return;
    // In compact mode, NewSegment and BeginFlush callers distinguish a source/player
    // seek transition from a local pause/stop flush. Pause/Run stacks remain full-trace only.
    if (!DirectShowFullTraceEnabled() && (!method ||
        (std::strcmp(method, "NewSegment") != 0 && std::strcmp(method, "BeginFlush") != 0))) return;
    try {
        DirectMotionLog("TRACE", "DirectShow/TransitionCaller",
            std::string("method=") + method +
            ", transitionId=" + std::to_string(transitionId) +
            ", threadId=" + std::to_string(GetCurrentThreadId()) +
            ", stack(skip=3,module+offset; symbols unavailable)=" + CaptureTransitionCallerStack());
    } catch (...) {
        // Never let optional stack diagnostics affect graph state transitions.
    }
}

// A deliberately sparse fingerprint for diagnosing static input/output content.
// It reads only 128 bytes spread across luma and chroma, never scans full frames.
uint64_t SparseNv12Fingerprint(const uint8_t* nv12, int width, int height) noexcept {
    if (!nv12 || width < 2 || height < 2 || (width & 1) || (height & 1)) return 0;
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](uint8_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<uint8_t>(width));
    mix(static_cast<uint8_t>(width >> 8));
    mix(static_cast<uint8_t>(height));
    mix(static_cast<uint8_t>(height >> 8));

    const size_t yBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (int row = 0; row < 8; ++row) {
        const int y = std::min(height - 1, ((2 * row + 1) * height) / 16);
        for (int col = 0; col < 8; ++col) {
            const int x = std::min(width - 1, ((2 * col + 1) * width) / 16);
            mix(nv12[static_cast<size_t>(y) * width + x]);
        }
    }
    const int chromaHeight = height / 2;
    for (int row = 0; row < 4; ++row) {
        const int y = std::min(chromaHeight - 1, ((2 * row + 1) * chromaHeight) / 8);
        for (int col = 0; col < 8; ++col) {
            int x = (((2 * col + 1) * width) / 16) & ~1;
            if (x > width - 2) x = width - 2;
            const size_t offset = yBytes + static_cast<size_t>(y) * width + x;
            mix(nv12[offset]);
            mix(nv12[offset + 1]);
        }
    }
    return hash;
}



// Used by the early per-sample media-type audit; definition follows below.
std::string GuidText(REFGUID guid);

std::wstring Utf8ToWide(const std::string& value) noexcept {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}

// The FOURCC media-subtype GUID layout used by DirectShow.
const GUID kSubtypeNV12 = MEDIASUBTYPE_NV12;
const GUID kSubtypeNV21 = { MAKEFOURCC('N','V','2','1'), 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

enum : int {
    kLayoutNV12 = 0,
    kLayoutNV21 = 7
};

WCHAR g_inputPinName[] = L"Input";
WCHAR g_outputPinName[] = L"Output";
WCHAR g_filterName[] = L"directmotion";

// Keep pin negotiation restricted to the two supported 8-bit semi-planar formats.
// NV12/NV21 are preserved at the DirectShow boundary; internal processing is normalized to NV12/YV12.
const AMOVIESETUP_MEDIATYPE g_pinTypes[] = {
    { &MEDIATYPE_Video, &kSubtypeNV12 },
    { &MEDIATYPE_Video, &kSubtypeNV21 }
};

const AMOVIESETUP_PIN g_pins[] = {
    { g_inputPinName, FALSE, FALSE, FALSE, FALSE, &CLSID_NULL, nullptr, _countof(g_pinTypes), g_pinTypes },
    { g_outputPinName, FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, _countof(g_pinTypes), g_pinTypes }
};

const AMOVIESETUP_FILTER g_filterSetup = {
    &CLSID_directmotion,
    g_filterName,
    MERIT_DO_NOT_USE,
    2,
    g_pins,
    CLSID_LegacyAmFilterCategory
};

// An address inside this image identifies the module containing the filter code,
// even when PotPlayer has loaded a copy that differs from the registered path.
int g_modulePathAnchor = 0;

std::wstring GetLoadedModulePath() {
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&g_modulePathAnchor), &module) || !module)
        return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, &path[0], static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size())) return {};
    path.resize(length);
    return path;
}

bool WriteNv12PreviewBmp(const uint8_t* nv12, int width, int height,
                         std::wstring& path, std::string& error) {
    if (!nv12 || width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        error = "Invalid NV12 payload for preview BMP.";
        return false;
    }
    try {
        const std::wstring modulePath = GetLoadedModulePath();
        const size_t separator = modulePath.find_last_of(L"\\/");
        if (modulePath.empty() || separator == std::wstring::npos) {
            error = "Could not resolve the loaded directmotion module path; Win32 error=" + std::to_string(GetLastError());
            return false;
        }
        path.assign(modulePath, 0, separator + 1);
        path += L"directmotion_first_input_audit.bmp";

        const size_t yBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
        const size_t expectedBytes = yBytes + yBytes / 2;
        const size_t rowBytes = static_cast<size_t>(width) * 3;
        const size_t bmpStride = (rowBytes + 3u) & ~size_t(3u);
        const size_t imageBytes = bmpStride * static_cast<size_t>(height);
        const size_t pixelOffset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        if (expectedBytes > static_cast<size_t>((std::numeric_limits<DWORD>::max)()) ||
            imageBytes > static_cast<size_t>((std::numeric_limits<DWORD>::max)()) - pixelOffset) {
            error = "NV12 preview BMP size overflows the Windows bitmap header.";
            return false;
        }
        std::vector<uint8_t> bmp(pixelOffset + imageBytes, 0);
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfSize = static_cast<DWORD>(bmp.size());
        fileHeader.bfOffBits = static_cast<DWORD>(pixelOffset);
        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(BITMAPINFOHEADER);
        infoHeader.biWidth = width;
        infoHeader.biHeight = -height; // Top-down BMP: first input row stays the first displayed row.
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 24;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = static_cast<DWORD>(imageBytes);
        std::memcpy(bmp.data(), &fileHeader, sizeof(fileHeader));
        std::memcpy(bmp.data() + sizeof(fileHeader), &infoHeader, sizeof(infoHeader));

        const uint8_t* uvBase = nv12 + yBytes;
        uint8_t* pixelBase = bmp.data() + pixelOffset;
        for (int y = 0; y < height; ++y) {
            const uint8_t* yRow = nv12 + static_cast<size_t>(y) * width;
            const uint8_t* uvRow = uvBase + static_cast<size_t>(y / 2) * width;
            uint8_t* outRow = pixelBase + static_cast<size_t>(y) * bmpStride;
            for (int x = 0; x < width; ++x) {
                const int yy = (std::max)(0, static_cast<int>(yRow[x]) - 16);
                const int uvIndex = x & ~1;
                const int u = static_cast<int>(uvRow[uvIndex]) - 128;
                const int v = static_cast<int>(uvRow[uvIndex + 1]) - 128;
                const int r = (std::clamp)((298 * yy + 409 * v + 128) >> 8, 0, 255);
                const int g = (std::clamp)((298 * yy - 100 * u - 208 * v + 128) >> 8, 0, 255);
                const int b = (std::clamp)((298 * yy + 516 * u + 128) >> 8, 0, 255);
                outRow[static_cast<size_t>(x) * 3 + 0] = static_cast<uint8_t>(b);
                outRow[static_cast<size_t>(x) * 3 + 1] = static_cast<uint8_t>(g);
                outRow[static_cast<size_t>(x) * 3 + 2] = static_cast<uint8_t>(r);
            }
        }

        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "CreateFileW for preview BMP failed, Win32 error=" + std::to_string(GetLastError());
            return false;
        }
        size_t offset = 0;
        bool ok = true;
        while (offset < bmp.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(bmp.size() - offset,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file, bmp.data() + offset, chunk, &written, nullptr) || written != chunk || written == 0) {
                error = "WriteFile for preview BMP failed, Win32 error=" + std::to_string(GetLastError());
                ok = false;
                break;
            }
            offset += written;
        }
        CloseHandle(file);
        return ok;
    } catch (const std::exception& e) {
        error = std::string("Preview BMP allocation/conversion failed: ") + e.what();
        return false;
    } catch (...) {
        error = "Preview BMP allocation/conversion failed with an unknown exception.";
        return false;
    }
}

bool GetVideoInfo(const CMediaType& mt, VIDEOINFOHEADER*& vi, VIDEOINFOHEADER2*& vi2) {
    vi = nullptr;
    vi2 = nullptr;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        vi = reinterpret_cast<VIDEOINFOHEADER*>(mt.pbFormat);
        return true;
    }
    if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        vi2 = reinterpret_cast<VIDEOINFOHEADER2*>(mt.pbFormat);
        return true;
    }
    return false;
}

REFERENCE_TIME GetAvgTimePerFrame(const CMediaType& mt) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return 0;
    return vi2 ? vi2->AvgTimePerFrame : vi->AvgTimePerFrame;
}

void SetAvgTimePerFrame(CMediaType& mt, REFERENCE_TIME value) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return;
    if (vi2) vi2->AvgTimePerFrame = value;
    else vi->AvgTimePerFrame = value;
}

bool GetSurfaceSize(const CMediaType& mt, int& width, int& height) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return false;
    const BITMAPINFOHEADER& bmi = vi2 ? vi2->bmiHeader : vi->bmiHeader;
    if (bmi.biWidth <= 0 || bmi.biHeight == 0 || bmi.biHeight == (std::numeric_limits<LONG>::min)())
        return false;
    width = bmi.biWidth;
    height = std::abs(bmi.biHeight);
    return width > 0 && height > 0;
}

bool GetVisibleRect(const CMediaType& mt, RECT& visible) {
    int surfaceWidth = 0, surfaceHeight = 0;
    if (!GetSurfaceSize(mt, surfaceWidth, surfaceHeight)) return false;
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return false;
    const RECT target = vi2 ? vi2->rcTarget : vi->rcTarget;
    const bool emptyTarget = target.right <= target.left || target.bottom <= target.top;

    // The filter reads via rcTarget; rcSource is the producer's upstream crop.
    visible = emptyTarget ? RECT{ 0, 0, surfaceWidth, surfaceHeight } : target;
    return visible.left >= 0 && visible.top >= 0 &&
        visible.right <= surfaceWidth && visible.bottom <= surfaceHeight &&
        visible.right > visible.left && visible.bottom > visible.top;
}

bool InputMediaTypeRequiresScaling(const CMediaType& mt, int visibleWidth, int visibleHeight) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    int surfaceWidth = 0, surfaceHeight = 0;
    if (!GetVideoInfo(mt, vi, vi2) || !GetSurfaceSize(mt, surfaceWidth, surfaceHeight)) return true;
    const RECT source = vi2 ? vi2->rcSource : vi->rcSource;
    const RECT target = vi2 ? vi2->rcTarget : vi->rcTarget;
    const bool emptySource = source.right <= source.left || source.bottom <= source.top;
    const bool emptyTarget = target.right <= target.left || target.bottom <= target.top;
    const RECT effectiveTarget = emptyTarget
        ? RECT{ 0, 0, surfaceWidth, surfaceHeight } : target;
    if (effectiveTarget.left < 0 || effectiveTarget.top < 0 ||
        effectiveTarget.right > surfaceWidth || effectiveTarget.bottom > surfaceHeight)
        return true;
    const int64_t targetWidth = static_cast<int64_t>(effectiveTarget.right) - effectiveTarget.left;
    const int64_t targetHeight = static_cast<int64_t>(effectiveTarget.bottom) - effectiveTarget.top;
    if (targetWidth != visibleWidth || targetHeight != visibleHeight) return true;
    if (emptySource) {
        // Without an explicit rcSource, the producer's full source maps to rcTarget.
        return false;
    }
    if (source.left < 0 || source.top < 0 ||
        source.right > surfaceWidth || source.bottom > surfaceHeight) return true;
    const int64_t sourceWidth = static_cast<int64_t>(source.right) - source.left;
    const int64_t sourceHeight = static_cast<int64_t>(source.bottom) - source.top;
    // No spatial scaling is supported: an explicit source crop must match the target extent.
    return sourceWidth != targetWidth || sourceHeight != targetHeight;
}

// Report visible picture dimensions, not the allocator's padded surface pitch.
bool GetFrameSize(const CMediaType& mt, int& width, int& height) {
    RECT visible{};
    if (!GetVisibleRect(mt, visible)) return false;
    width = visible.right - visible.left;
    height = visible.bottom - visible.top;
    return width > 0 && height > 0;
}

int GetImageSize(const CMediaType& mt) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return 0;
    return static_cast<int>(vi2 ? vi2->bmiHeader.biSizeImage : vi->bmiHeader.biSizeImage);
}

int Align4(int value) { return (value + 3) & ~3; }

int AlignNv12SurfaceHeight(int height) {
    constexpr int kSurfaceHeightAlignment = 16;
    if (height <= 0 || height > (std::numeric_limits<int>::max)() - (kSurfaceHeightAlignment - 1))
        return height;
    return (height + kSurfaceHeightAlignment - 1) & ~(kSurfaceHeightAlignment - 1);
}

// Prefer 256-pixel D3D9 pitch; GetMediaType also offers tight layouts.
int AlignNv12Stride(int width) {
    constexpr int kStrideAlignment = 256;
    if (width <= 0 || width > (std::numeric_limits<int>::max)() - (kStrideAlignment - 1))
        return width;
    return (width + kStrideAlignment - 1) & ~(kStrideAlignment - 1);
}

size_t Nv12BytesForStride(int stride, int height) {
    if (stride <= 0 || height <= 0 || (height & 1)) return 0;
    const size_t s = static_cast<size_t>(stride);
    const size_t h = static_cast<size_t>(height);
    const size_t max = (std::numeric_limits<size_t>::max)();
    if (s > max / h) return 0;
    const size_t yBytes = s * h;
    if (yBytes > max - yBytes / 2) return 0;
    return yBytes + yBytes / 2;
}

std::string VideoHeaderAudit(const CMediaType& mt) {
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mt, vi, vi2)) return "header=unavailable";
    const BITMAPINFOHEADER& bmi = vi2 ? vi2->bmiHeader : vi->bmiHeader;
    const RECT source = vi2 ? vi2->rcSource : vi->rcSource;
    const RECT target = vi2 ? vi2->rcTarget : vi->rcTarget;
    std::string out = "biWidth=" + std::to_string(bmi.biWidth) +
        ", biHeight=" + std::to_string(bmi.biHeight) +
        ", biPlanes=" + std::to_string(bmi.biPlanes) +
        ", biBitCount=" + std::to_string(bmi.biBitCount) +
        ", biCompression=" + std::to_string(bmi.biCompression) +
        ", biSizeImage=" + std::to_string(bmi.biSizeImage) +
        ", rcSource=" + std::to_string(source.left) + "," + std::to_string(source.top) + "," +
            std::to_string(source.right) + "," + std::to_string(source.bottom) +
        ", rcTarget=" + std::to_string(target.left) + "," + std::to_string(target.top) + "," +
            std::to_string(target.right) + "," + std::to_string(target.bottom) +
        ", avgTimePerFrame=" + std::to_string(GetAvgTimePerFrame(mt));
    if (vi2) {
        out += ", dwInterlaceFlags=" + std::to_string(vi2->dwInterlaceFlags) +
            ", dwCopyProtectFlags=" + std::to_string(vi2->dwCopyProtectFlags) +
            ", aspect=" + std::to_string(vi2->dwPictAspectRatioX) + ":" +
                std::to_string(vi2->dwPictAspectRatioY) +
            ", dwControlFlags=" + std::to_string(vi2->dwControlFlags) +
            ", dwReserved2=" + std::to_string(vi2->dwReserved2);
    }
    out += ", fixedSamples=" + std::to_string(mt.bFixedSizeSamples ? 1 : 0) +
        ", temporalCompression=" + std::to_string(mt.bTemporalCompression ? 1 : 0) +
        ", sampleSize=" + std::to_string(mt.lSampleSize) +
        ", cbFormat=" + std::to_string(mt.cbFormat);
    return out;
}

bool IsFourCCSubtype(const GUID& subtype, DWORD fourcc) {
    return subtype.Data1 == fourcc && subtype.Data2 == 0 && subtype.Data3 == 0x0010 &&
        subtype.Data4[0] == 0x80 && subtype.Data4[1] == 0x00 &&
        subtype.Data4[2] == 0x00 && subtype.Data4[3] == 0xAA &&
        subtype.Data4[4] == 0x00 && subtype.Data4[5] == 0x38 &&
        subtype.Data4[6] == 0x9B && subtype.Data4[7] == 0x71;
}

bool IsNv12MediaSubtype(const GUID& subtype) {
    return subtype == kSubtypeNV12 || IsFourCCSubtype(subtype, MAKEFOURCC('N','V','1','2'));
}

std::string GuidText(REFGUID guid) {
    wchar_t text[64] = {};
    if (!StringFromGUID2(guid, text, _countof(text))) return "{unknown-guid}";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "{unknown-guid}";
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr);
    result.pop_back();
    return result;
}

const char* LayoutName(int layout) {
    switch (layout) {
    case kLayoutNV12: return "NV12";
    case kLayoutNV21: return "NV21";
    default: return "unsupported";
    }
}

FrucRational RationalFromDouble(double value) {
    return FrucRationalFromDouble(value, 100000);
}

} // namespace

CFactoryTemplate g_Templates[] = {
    { L"directmotion", &CLSID_directmotion, CDirectMotion::CreateInstance, nullptr, &g_filterSetup },
    { L"directmotion Settings", &CLSID_directmotionSettings, CDirectMotionSettingsPage::CreateInstance }
};
int g_cTemplates = static_cast<int>(sizeof(g_Templates) / sizeof(g_Templates[0]));

STDAPI DllRegisterServer() { return AMovieDllRegisterServer2(TRUE); }
STDAPI DllUnregisterServer() { return AMovieDllRegisterServer2(FALSE); }
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HANDLE module, DWORD reason, LPVOID reserved) {
    return DllEntryPoint(static_cast<HINSTANCE>(module), reason, reserved);
}

CUnknown* WINAPI CDirectMotion::CreateInstance(LPUNKNOWN punk, HRESULT* phr) {
    ASSERT(phr);
    // Do not log here: the factory runs before the constructor loads LogEnabled
    // from HKCU. A process-wide logger left enabled by another object could
    // otherwise create/append directmotion.log before this instance reads its preference.
    auto* object = new CDirectMotion(NAME("directmotion"), punk, phr);
    if (!object && phr) *phr = E_OUTOFMEMORY;
    return object;
}

CDirectMotion::CDirectMotion(LPCTSTR name, LPUNKNOWN punk, HRESULT* phr)
    : CTransformFilter(name, punk, CLSID_directmotion) {
    if (phr) *phr = S_OK;
    LoadSettings();
    DirectMotionTrayAcquire();
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    const std::wstring loadedModulePath = GetLoadedModulePath();
    DirectMotionLogW("INFO", "BuildIdentity", L"build=directmotion 3.0.0 (baseline=0.2.58.28; lineage=source-generation-rebinding; parent=0.2.58-dev-initial-segment-preroll-order-independent; backend=avisynth-mvtools; runtime-dlls=module-directory-only); modulePath=" +
        (loadedModulePath.empty() ? std::wstring(L"<unresolved>") : loadedModulePath));
    LogInfo("Construct", std::string("Filter constructed. Enabled=") + (m_settings.enabled ? "1" : "0") +
        ", rateMode=" + std::to_string(m_settings.rateMode) +
        ", performance=" + std::to_string(m_settings.performance) +
        ", grid=" + std::to_string(m_settings.grid) +
        ", saveFirstInputBmp=" + std::to_string(m_settings.saveFirstInputBmp ? 1 : 0) +
        ", logEnabled=" + std::to_string(m_settings.logEnabled ? 1 : 0));
}

CDirectMotion::~CDirectMotion() {
    LogInfo("Destruct", "Filter instance being destroyed. GraphAttached=" +
        std::to_string(GetFilterGraph() ? 1 : 0) + ", InputConnected=" +
        std::to_string(m_pInput && m_pInput->IsConnected() ? 1 : 0) +
        ", OutputConnected=" + std::to_string(m_pOutput && m_pOutput->IsConnected() ? 1 : 0) +
        ", Inputs=" + std::to_string(m_inputFrameCount) +
        ", Outputs=" + std::to_string(m_outputFrameCount));
    if (m_pOutput && m_pOutput->IsConnected()) {
        // The graph manager normally stops filters before the final release. Keep a
        // best-effort teardown guard for hosts that release an active filter directly.
        (void)m_pOutput->DeliverBeginFlush();
    }
    StopSourceBootstrapWorker("filter destruction");
    StopDeliveryWorker("filter destruction");
    StopProcessingWorker("filter destruction");
    ResetEngine();
    StopDiagnosticsWorker();
    DirectMotionTrayRelease();
}

STDMETHODIMP CDirectMotion::EnumPins(IEnumPins** ppEnum) {
    // PotPlayer queries pins repeatedly during playback. Do not log normal enumeration:
    // the synchronous per-line file logger would add I/O without helping diagnose delivery.
    const HRESULT hr = CTransformFilter::EnumPins(ppEnum);
    if (FAILED(hr)) {
        LogError("EnumPins", "Base EnumPins failed, HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
    }
    return hr;
}

STDMETHODIMP CDirectMotion::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
    const std::string graphName = name ? std::string("provided") : std::string("null");
    LogInfo("JoinFilterGraph", std::string(graph ? "Attaching to graph" : "Detaching from graph") +
        "; filterName=" + graphName + "; graphPtr=" +
        std::to_string(reinterpret_cast<ULONG_PTR>(graph)));
    const HRESULT hr = CTransformFilter::JoinFilterGraph(graph, name);
    LogInfo("JoinFilterGraph", "Base JoinFilterGraph returned HRESULT=" +
        std::to_string(static_cast<unsigned long>(hr)));
    return hr;
}

int CDirectMotion::GetPinCount() {
    return CTransformFilter::GetPinCount();
}

CBasePin* CDirectMotion::GetPin(int index) {
    return CTransformFilter::GetPin(index);
}

STDMETHODIMP CDirectMotion::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IDirectMotionSettings) {
        return GetInterface(static_cast<IDirectMotionSettings*>(this), ppv);
    }
    if (riid == IID_ISpecifyPropertyPages) {
        return GetInterface(static_cast<ISpecifyPropertyPages*>(this), ppv);
    }
    return CTransformFilter::NonDelegatingQueryInterface(riid, ppv);
}

HRESULT CDirectMotion::CheckInputType(const CMediaType* mtIn) {
    if (!mtIn) return E_POINTER;
    const bool isNv12 = IsNv12MediaSubtype(mtIn->subtype);
    const bool isNv21 = (mtIn->subtype == kSubtypeNV21 ||
        IsFourCCSubtype(mtIn->subtype, MAKEFOURCC('N','V','2','1')));
    const bool supportedSubtype = isNv12 || isNv21;
    const int layout = isNv21 ? kLayoutNV21 : kLayoutNV12;
    int width = 0, height = 0, surfaceWidth = 0, surfaceHeight = 0;
    RECT visible{};
    const bool validSize = GetFrameSize(*mtIn, width, height) &&
        GetSurfaceSize(*mtIn, surfaceWidth, surfaceHeight) && GetVisibleRect(*mtIn, visible) &&
        width >= 16 && height >= 16 && surfaceWidth >= visible.right && surfaceHeight >= visible.bottom &&
        !InputMediaTypeRequiresScaling(*mtIn, width, height) &&
        ((visible.left | visible.top | width | height | surfaceWidth | surfaceHeight) & 1) == 0;
    const bool supportedFormat = mtIn->formattype == FORMAT_VideoInfo || mtIn->formattype == FORMAT_VideoInfo2;
    const bool dimensionsOk = validSize && ((width & 1) == 0 && (height & 1) == 0);
    const bool accepted = mtIn->majortype == MEDIATYPE_Video && supportedSubtype && supportedFormat && dimensionsOk;
    std::string message = std::string(accepted ? "ACCEPT " : "REJECT ") +
        "subtype=" + GuidText(mtIn->subtype) + ", layout=" + (supportedSubtype ? LayoutName(layout) : "unsupported; NV12/NV21 only") +
        ", formattype=" + GuidText(mtIn->formattype) + ", size=" +
        std::to_string(width) + "x" + std::to_string(height) +
        ", majorVideo=" + std::to_string(mtIn->majortype == MEDIATYPE_Video ? 1 : 0) +
        ", supportedHeader=" + std::to_string(supportedFormat ? 1 : 0);
    if (accepted) {
        if (!m_loggedAcceptedInputType.exchange(true))
            DirectMotionLog("INFO", "CheckInputType", message);
        return S_OK;
    }
    DirectMotionLog("WARN", "CheckInputType", message);
    return VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT CDirectMotion::CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut) {
    if (!mtIn || !mtOut) return E_POINTER;
    if (FAILED(CheckInputType(mtIn)) || mtOut->majortype != MEDIATYPE_Video ||
        mtOut->subtype != mtIn->subtype) {
        DirectMotionLog("WARN", "CheckTransform", "Rejected output type because major type/subtype differs. input=" +
            GuidText(mtIn->subtype) + "; output=" + GuidText(mtOut->subtype) +
            "; inputFormat=" + GuidText(mtIn->formattype) +
            "; outputFormat=" + GuidText(mtOut->formattype));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    int inW = 0, inH = 0;
    int outSurfaceWidth = 0, outSurfaceHeight = 0;
    if (!GetFrameSize(*mtIn, inW, inH) ||
        !GetSurfaceSize(*mtOut, outSurfaceWidth, outSurfaceHeight)) {
        DirectMotionLog("WARN", "CheckTransform", "Rejected output type because its visible picture or physical surface cannot be parsed. input{" +
            VideoHeaderAudit(*mtIn) + "}; output{" + VideoHeaderAudit(*mtOut) + "}");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    VIDEOINFOHEADER* outVi = nullptr;
    VIDEOINFOHEADER2* outVi2 = nullptr;
    if (!GetVideoInfo(*mtOut, outVi, outVi2) || (!outVi && !outVi2))
        return VFW_E_TYPE_NOT_ACCEPTED;
    const RECT rawSource = outVi2 ? outVi2->rcSource : outVi->rcSource;
    const RECT rawTarget = outVi2 ? outVi2->rcTarget : outVi->rcTarget;
    const auto emptyRect = [](const RECT& r) {
        return r.right <= r.left || r.bottom <= r.top;
    };
    // rcTarget locates the visible picture in the physical renderer surface.
    const RECT sourceRect = emptyRect(rawSource) ? RECT{ 0, 0, inW, inH } : rawSource;
    const RECT targetRect = emptyRect(rawTarget)
        ? RECT{ 0, 0, outSurfaceWidth, outSurfaceHeight } : rawTarget;
    const int64_t targetWidth = static_cast<int64_t>(targetRect.right) - targetRect.left;
    const int64_t targetHeight = static_cast<int64_t>(targetRect.bottom) - targetRect.top;
    const int maxStride = AlignNv12Stride(inW + 1024);
    const int maxSurfaceHeight = inH + 256;
    const bool sourceMatches = sourceRect.left == 0 && sourceRect.top == 0 &&
        sourceRect.right == inW && sourceRect.bottom == inH;
    const bool targetMatches = targetRect.left >= 0 && targetRect.top >= 0 &&
        targetWidth == inW && targetHeight == inH &&
        targetRect.right <= outSurfaceWidth && targetRect.bottom <= outSurfaceHeight &&
        ((targetRect.left | targetRect.top) & 1) == 0;
    const bool surfaceFits = outSurfaceWidth >= targetRect.right &&
        outSurfaceWidth <= maxStride && (outSurfaceWidth & 1) == 0 &&
        outSurfaceHeight >= targetRect.bottom && outSurfaceHeight <= maxSurfaceHeight &&
        (outSurfaceHeight & 1) == 0;
    if (!sourceMatches || !targetMatches || !surfaceFits) {
        DirectMotionLog("WARN", "CheckTransform", "Rejected output media type. input{" +
            VideoHeaderAudit(*mtIn) + "}; output{" + VideoHeaderAudit(*mtOut) +
            "}; expectedVisible=" + std::to_string(inW) + "x" + std::to_string(inH) +
            "; outputSurface=" + std::to_string(outSurfaceWidth) + "x" + std::to_string(outSurfaceHeight) +
            "; outputTarget=" + std::to_string(targetRect.left) + "," + std::to_string(targetRect.top) + "," +
                std::to_string(targetRect.right) + "," + std::to_string(targetRect.bottom) +
            "; pitchRange=" + std::to_string(inW) + ".." + std::to_string(maxStride) +
            "; surfaceHeightRange=" + std::to_string(inH) + ".." + std::to_string(maxSurfaceHeight));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    const size_t minimumBytes = Nv12BytesForStride(outSurfaceWidth, outSurfaceHeight);
    if (minimumBytes == 0 || minimumBytes > static_cast<size_t>((std::numeric_limits<LONG>::max)()))
        return VFW_E_TYPE_NOT_ACCEPTED;

    if (!m_loggedAcceptedTransform.exchange(true)) {
        DirectMotionLog("INFO", "CheckTransform", "Accepted semi-planar output: visible=" +
            std::to_string(inW) + "x" + std::to_string(inH) +
            ", surface=" + std::to_string(outSurfaceWidth) + "x" + std::to_string(outSurfaceHeight) +
            ", pitchPixels=" + std::to_string(outSurfaceWidth) +
            ", targetRect=" + std::to_string(targetRect.left) + "," + std::to_string(targetRect.top) + "," +
                std::to_string(targetRect.right) + "," + std::to_string(targetRect.bottom) +
            ", minimumBytes=" + std::to_string(minimumBytes) +
            ", header{" + VideoHeaderAudit(*mtOut) + "}");
    }
    return S_OK;
}

HRESULT CDirectMotion::GetMediaType(int position, CMediaType* mediaType) {
    if (!mediaType) {
        LogError("GetMediaType", "Output media type requested with a null destination pointer.");
        return E_POINTER;
    }
    if (position < 0) {
        LogError("GetMediaType", "Output media type requested with invalid negative position=" + std::to_string(position));
        return E_INVALIDARG;
    }
    if (position > 3) return VFW_S_NO_MORE_ITEMS;
    if (!m_pInput || !m_pInput->IsConnected()) return VFW_E_NOT_CONNECTED;

    *mediaType = m_pInput->CurrentMediaType();
    m_sourceRate = SourceFrameRate();
    m_autoDisplayRate = FrucRational{ 0, 1 };
    m_autoDisplayRateSource.clear();
    if (m_settings.enabled && m_settings.rateMode == kRateModeAuto) {
        m_autoDisplayRate = ResolveActiveDisplayRefresh(m_pOutput, m_pGraph, m_autoDisplayRateSource);
        if (m_autoDisplayRate.num <= 0 || m_autoDisplayRate.den <= 0) {
            LogInfo("TargetFPS/Auto", "Could not identify a valid display refresh; Auto safely falls back to 2x source FPS. " +
                m_autoDisplayRateSource);
        } else {
            LogInfo("TargetFPS/Auto", "Resolved display refresh=" +
                std::to_string(m_autoDisplayRate.num) + "/" + std::to_string(m_autoDisplayRate.den) +
                " Hz; sourceFPS=" + std::to_string(static_cast<double>(m_sourceRate.num) / m_sourceRate.den) +
                "; " + m_autoDisplayRateSource);
        }
    }
    const FrucRational resolvedTargetRate = m_settings.enabled ? TargetFrameRate() : m_sourceRate;
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_targetRate = resolvedTargetRate;
    }
    if (m_settings.enabled && m_settings.rateMode == kRateModeAuto && position == 0) {
        LogInfo("TargetFPS/Auto", "Selected target=" + std::to_string(m_targetRate.num) + "/" +
            std::to_string(m_targetRate.den) + " FPS; source=" + std::to_string(m_sourceRate.num) + "/" +
            std::to_string(m_sourceRate.den) + "; display=" +
            (m_autoDisplayRate.num > 0 && m_autoDisplayRate.den > 0
                ? std::to_string(m_autoDisplayRate.num) + "/" + std::to_string(m_autoDisplayRate.den)
                : std::string("unavailable")) + "; mode=Auto.");
    }
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_targetFrameDuration = FrucFrameDuration100ns(m_targetRate);
        if (m_targetFrameDuration <= 0) m_targetFrameDuration = 1;
    }
    SetAvgTimePerFrame(*mediaType, m_targetFrameDuration);

    const int width = m_width;
    const int height = m_height;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return VFW_E_TYPE_NOT_ACCEPTED;

    const int tightStride = width;
    const int paddedStride = AlignNv12Stride(width);
    const int tightHeight = height;
    const int paddedHeight = AlignNv12SurfaceHeight(height);
    const std::pair<int, int> rawCandidates[] = {
        { paddedStride, paddedHeight },
        { tightStride, paddedHeight },
        { paddedStride, tightHeight },
        { tightStride, tightHeight }
    };
    std::vector<std::pair<int, int>> candidates;
    for (const auto& candidate : rawCandidates) {
        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
            candidates.push_back(candidate);
    }
    if (static_cast<size_t>(position) >= candidates.size()) return VFW_S_NO_MORE_ITEMS;

    const int candidateStride = candidates[static_cast<size_t>(position)].first;
    const int candidateHeight = candidates[static_cast<size_t>(position)].second;
    const size_t candidateBytes = Nv12BytesForStride(candidateStride, candidateHeight);
    if (candidateBytes == 0 || candidateBytes > static_cast<size_t>((std::numeric_limits<LONG>::max)()))
        return VFW_E_TYPE_NOT_ACCEPTED;
    const int imageSize = static_cast<int>(candidateBytes);
    LONG outputBiHeight = 0;
    DWORD outputCompression = 0;
    DWORD outputBiSizeImage = 0;
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    WORD outputBiPlanes = 0;
    WORD outputBiBitCount = 0;
    RECT outputSourceRect{};
    RECT outputTargetRect{};
    if (!GetVideoInfo(*mediaType, vi, vi2)) return VFW_E_TYPE_NOT_ACCEPTED;
    BITMAPINFOHEADER& bmi = vi2 ? vi2->bmiHeader : vi->bmiHeader;
    // Advertise physical surface dimensions separately from visible geometry.
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = candidateStride;
    bmi.biHeight = candidateHeight;
    bmi.biPlanes = 1;
    bmi.biBitCount = 12;
    bmi.biCompression = (m_inputLayout == kLayoutNV21)
        ? MAKEFOURCC('N','V','2','1') : MAKEFOURCC('N','V','1','2');
    bmi.biSizeImage = static_cast<DWORD>(candidateBytes);
    outputBiHeight = bmi.biHeight;
    outputCompression = bmi.biCompression;
    outputBiPlanes = bmi.biPlanes;
    outputBiBitCount = bmi.biBitCount;
    if (vi2) {
        vi2->rcSource = RECT{ 0, 0, width, height };
        vi2->rcTarget = RECT{ 0, 0, width, height };
        outputSourceRect = vi2->rcSource;
        outputTargetRect = vi2->rcTarget;
    } else {
        vi->rcSource = RECT{ 0, 0, width, height };
        vi->rcTarget = RECT{ 0, 0, width, height };
        outputSourceRect = vi->rcSource;
        outputTargetRect = vi->rcTarget;
    }
    mediaType->SetSampleSize(imageSize);
    mediaType->SetTemporalCompression(FALSE);
    mediaType->bFixedSizeSamples = TRUE;

    const unsigned candidateBit = 1u << static_cast<unsigned>(position);
    const unsigned priorMask = m_loggedMediaTypeCandidateMask.fetch_or(candidateBit);
    if ((priorMask & candidateBit) == 0) {
        LogInfo("GetMediaType", "Offering semi-planar output candidate=" + std::to_string(position) +
            ", visibleSize=" + std::to_string(width) + "x" + std::to_string(height) +
            ", surface=" + std::to_string(candidateStride) + "x" + std::to_string(candidateHeight) +
            ", stridePixels=" + std::to_string(candidateStride) +
            ", bufferBytes=" + std::to_string(candidateBytes) +
            ", subtype=" + GuidText(mediaType->subtype) +
            ", formattype=" + GuidText(mediaType->formattype) +
            ", cbFormat=" + std::to_string(mediaType->cbFormat) +
            ", biHeight=" + std::to_string(outputBiHeight) +
            ", biPlanes=" + std::to_string(outputBiPlanes) +
            ", biBitCount=" + std::to_string(outputBiBitCount) +
            ", biCompression=" + std::to_string(outputCompression) +
            ", biSizeImage=" + std::to_string(bmi.biSizeImage) +
            ", rcSource=" + std::to_string(outputSourceRect.left) + "," + std::to_string(outputSourceRect.top) + "," +
                std::to_string(outputSourceRect.right) + "," + std::to_string(outputSourceRect.bottom) +
            ", rcTarget=" + std::to_string(outputTargetRect.left) + "," + std::to_string(outputTargetRect.top) + "," +
                std::to_string(outputTargetRect.right) + "," + std::to_string(outputTargetRect.bottom) +
            ", bFixedSizeSamples=" + std::to_string(mediaType->bFixedSizeSamples ? 1 : 0) +
            ", sampleSize=" + std::to_string(mediaType->lSampleSize) +
            ", sourceFPS=" + std::to_string(static_cast<double>(m_sourceRate.num) / m_sourceRate.den) +
            ", outputFPS=" + std::to_string(static_cast<double>(m_targetRate.num) / m_targetRate.den) +
            ", header{" + VideoHeaderAudit(*mediaType) + "}");
    }
    return S_OK;
}

HRESULT CDirectMotion::SetMediaType(PIN_DIRECTION direction, const CMediaType* mediaType) {
    HRESULT hr = CTransformFilter::SetMediaType(direction, mediaType);
    if (FAILED(hr) || !mediaType) return hr;
    if (direction == PINDIR_INPUT) {
        if (!ParseInputMediaType(*mediaType)) {
            LogError("SetMediaType/Input", "Input media type passed SetMediaType but could not be parsed.");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        ResetEngine();
        LogInfo("SetMediaType/Input", "Accepted " + std::string(LayoutName(m_inputLayout)) + " input: " + std::to_string(m_width) + "x" +
            std::to_string(m_height) + ", stride=" + std::to_string(m_inputStride) +
            ", imageBytes=" + std::to_string(m_inputBufferBytes) +
            ", sourceFPS=" + std::to_string(static_cast<double>(m_sourceRate.num) / m_sourceRate.den));
    } else if (direction == PINDIR_OUTPUT) {
        int outputSurfaceWidth = 0, outputSurfaceHeight = 0;
        VIDEOINFOHEADER* outputVi = nullptr;
        VIDEOINFOHEADER2* outputVi2 = nullptr;
        if (!GetSurfaceSize(*mediaType, outputSurfaceWidth, outputSurfaceHeight) ||
            !GetVideoInfo(*mediaType, outputVi, outputVi2) || (!outputVi && !outputVi2)) {
            LogError("SetMediaType/Output", "Accepted output media type has no usable semi-planar surface header. header{" +
                VideoHeaderAudit(*mediaType) + "}");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const RECT rawSource = outputVi2 ? outputVi2->rcSource : outputVi->rcSource;
        const RECT rawTarget = outputVi2 ? outputVi2->rcTarget : outputVi->rcTarget;
        const auto emptyRect = [](const RECT& r) { return r.right <= r.left || r.bottom <= r.top; };
        const RECT sourceRect = emptyRect(rawSource) ? RECT{ 0, 0, m_width, m_height } : rawSource;
        const RECT targetRect = emptyRect(rawTarget)
            ? RECT{ 0, 0, outputSurfaceWidth, outputSurfaceHeight } : rawTarget;
        const int64_t targetWidth = static_cast<int64_t>(targetRect.right) - targetRect.left;
        const int64_t targetHeight = static_cast<int64_t>(targetRect.bottom) - targetRect.top;
        const int maxStride = AlignNv12Stride(m_width + 1024);
        const int maxSurfaceHeight = m_height + 256;
        if (sourceRect.left != 0 || sourceRect.top != 0 ||
            sourceRect.right != m_width || sourceRect.bottom != m_height ||
            targetRect.left < 0 || targetRect.top < 0 ||
            targetWidth != m_width || targetHeight != m_height ||
            targetRect.right > outputSurfaceWidth || targetRect.bottom > outputSurfaceHeight ||
            ((targetRect.left | targetRect.top) & 1) != 0 ||
            outputSurfaceWidth < targetRect.right || outputSurfaceWidth > maxStride ||
            (outputSurfaceWidth & 1) != 0 ||
            outputSurfaceHeight < targetRect.bottom || outputSurfaceHeight > maxSurfaceHeight ||
            (outputSurfaceHeight & 1) != 0) {
            LogError("SetMediaType/Output", "Accepted output type has incompatible picture geometry or unsupported padded NV12 surface. header{" +
                VideoHeaderAudit(*mediaType) + "}");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const size_t chosenBytes = Nv12BytesForStride(outputSurfaceWidth, outputSurfaceHeight);
        if (chosenBytes == 0 || chosenBytes > static_cast<size_t>((std::numeric_limits<LONG>::max)()))
            return VFW_E_TYPE_NOT_ACCEPTED;
        m_outputStride = outputSurfaceWidth;
        m_outputSurfaceHeight = outputSurfaceHeight;
        m_outputTargetLeft = targetRect.left;
        m_outputTargetTop = targetRect.top;
        m_outputBufferBytes = chosenBytes;
        const REFERENCE_TIME avg = GetAvgTimePerFrame(*mediaType);
        if (avg > 0) {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_targetFrameDuration = avg;
        }
        LogInfo("SetMediaType/Output", "Output media type accepted: visible=" + std::to_string(m_width) +
            "x" + std::to_string(m_height) + ", surface=" + std::to_string(outputSurfaceWidth) +
            "x" + std::to_string(outputSurfaceHeight) + ", outputPitchPixels=" + std::to_string(m_outputStride) +
            ", targetRect=" + std::to_string(targetRect.left) + "," + std::to_string(targetRect.top) + "," +
                std::to_string(targetRect.right) + "," + std::to_string(targetRect.bottom) +
            ", outputBufferBytes=" + std::to_string(m_outputBufferBytes) +
            ", frameDuration100ns=" + std::to_string(m_targetFrameDuration) +
            ", header{" + VideoHeaderAudit(*mediaType) + "}");
    }
    return S_OK;
}

HRESULT CDirectMotion::CompleteConnect(PIN_DIRECTION direction, IPin* receivePin) {
    LogInfo("CompleteConnect", std::string("Attempting ") +
        (direction == PINDIR_INPUT ? "input" : "output") + " pin connection.");
    const HRESULT hr = CTransformFilter::CompleteConnect(direction, receivePin);
    if (FAILED(hr)) {
        LogError("CompleteConnect", std::string(direction == PINDIR_INPUT ? "Input" : "Output") +
            " pin connection failed, HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
    } else {
        std::string typeDescription = "media type not yet available";
        if (direction == PINDIR_INPUT && m_pInput && m_pInput->IsConnected()) {
            const CMediaType& mt = m_pInput->CurrentMediaType();
            int w = 0, h = 0;
            GetFrameSize(mt, w, h);
            const int layout = IsNv12MediaSubtype(mt.subtype) ? kLayoutNV12 : -1;
            typeDescription = std::string("subtype=") + GuidText(mt.subtype) + ", format=" + LayoutName(layout) +
                ", size=" + std::to_string(w) + "x" + std::to_string(h);
        } else if (direction == PINDIR_OUTPUT && m_pOutput && m_pOutput->IsConnected()) {
            const CMediaType& mt = m_pOutput->CurrentMediaType();
            int w = 0, h = 0; GetFrameSize(mt, w, h);
            typeDescription = "subtype=" + GuidText(mt.subtype) + ", size=" + std::to_string(w) + "x" + std::to_string(h);
        }
        LogInfo("CompleteConnect", std::string(direction == PINDIR_INPUT ? "Input" : "Output") +
            " pin connected successfully: " + typeDescription);
    }
    return hr;
}

HRESULT CDirectMotion::BreakConnect(PIN_DIRECTION direction) {
    LogInfo("BreakConnect", std::string(direction == PINDIR_INPUT ? "Input" : "Output") + " pin disconnect requested.");
    const HRESULT hr = CTransformFilter::BreakConnect(direction);
    if (FAILED(hr)) LogError("BreakConnect", "Base-class disconnect failed, HRESULT=" +
        std::to_string(static_cast<unsigned long>(hr)));
    else LogInfo("BreakConnect", "Pin disconnected successfully.");
    return hr;
}

void CDirectMotion::SetEvaluationEnabled(bool enabled, const char* reason) noexcept {
    try {
        if (!m_sourceEvaluationGate) return;
        const auto state = m_sourceEvaluationGate->SetEnabled(enabled);
        if (!state.changed) return;
        DirectMotionLog("INFO", "AviSynth/EvaluationGate",
            std::string(enabled ? "OPEN" : "CLOSED") +
            ", reason=" + (reason ? reason : "unspecified") +
            ", blockedRequestsTotal=" + std::to_string(state.blockedRequests) +
            ", requestsCurrentlyWaiting=" + std::to_string(state.waitingRequests) +
            "; synchronous AviSynth source evaluation " +
            (enabled ? "is permitted." : "is gated."));
    } catch (...) {
        // Diagnostics are best-effort; the shared gate itself must remain usable.
    }
}

uint64_t CDirectMotion::InterruptPendingSourceRequests(const char* reason) noexcept {
    try {
        if (!m_sourceEvaluationGate) return 0;
        const uint64_t epoch = m_sourceEvaluationGate->InterruptSourceRequests();
        DirectMotionLog("INFO", "AviSynth/SourceWait",
            std::string("Published cancellation before waiting for the DirectShow receive lock; epoch=") +
            std::to_string(epoch) + ", reason=" + (reason ? reason : "unspecified") + ".");
        return epoch;
    } catch (...) {
        // Lifecycle cancellation must remain best-effort and must never throw into COM.
        return 0;
    }
}

HRESULT CDirectMotion::WaitForReceiveAdmission(IMediaSample* sample) {
    if (!sample) return E_POINTER;

    // Match CBaseInputPin::Receive's stream-id extraction without touching its
    // shared m_SampleProps before the receive lock is acquired.
    IMediaSample2* sample2 = nullptr;
    const HRESULT queryHr = sample->QueryInterface(IID_IMediaSample2,
        reinterpret_cast<void**>(&sample2));
    if (SUCCEEDED(queryHr) && sample2) {
        AM_SAMPLE2_PROPERTIES properties{};
        properties.cbData = sizeof(properties);
        const HRESULT propertiesHr = sample2->GetProperties(sizeof(properties),
            reinterpret_cast<BYTE*>(&properties));
        sample2->Release();
        if (FAILED(propertiesHr)) return propertiesHr;
        if (properties.dwStreamId != AM_STREAM_MEDIA) return S_OK;
    }

    const auto waitForBootstrapCapacity = [this]() -> HRESULT {
        const size_t frameBytes = NormalizedSourceFrameBytes();
        if (frameBytes == 0) return S_OK;
        const auto waitResult = m_sourceBootstrapQueue.WaitForAdmission(frameBytes, [this]() noexcept {
            return m_sourceBootstrapWorkerStop.load(std::memory_order_acquire) ||
                m_runtimeFailed.load(std::memory_order_acquire) || IsGraphTransitioning();
        });
        if (waitResult == DirectMotionSourceBootstrap::AdmissionResult::Ready) return S_OK;
        if (waitResult == DirectMotionSourceBootstrap::AdmissionResult::TooLarge) {
            LogError("Receive/SourceBootstrapBackpressure", "The normalized source frame exceeds the bounded bootstrap staging byte budget.");
            return E_OUTOFMEMORY;
        }
        if (m_runtimeFailed.load(std::memory_order_acquire)) return E_FAIL;
        if (m_stopRequested.load(std::memory_order_acquire)) return VFW_E_WRONG_STATE;
        LogInfo("Receive/SourceBootstrapBackpressure",
            "Cancelled a source-stage admission wait because the staging generation or graph lifecycle changed.");
        return S_FALSE;
    };

    // Cold-start DirectShow preroll is not a user-requested paused-frame step.
    // Keep admitting source samples until AviSynth/MVTools can populate its
    // minimum bootstrap lookahead; a strict pause hold still takes precedence.
    if (DirectMotionInitialPrerollPolicy::AllowColdStartPreroll(
            m_initialPrerollActive.load(std::memory_order_acquire),
            m_runActive.load(std::memory_order_acquire),
            m_pauseHoldActive.load(std::memory_order_acquire))) {
        return waitForBootstrapCapacity();
    }

    // Capture the lifecycle token before checking the one-shot marker. If Run or
    // a cancellation races with the check, WaitForChange observes the new epoch
    // immediately instead of missing a notification and sleeping indefinitely.
    const PauseReceiveBackpressure::Token token = m_pauseReceiveBackpressure.Capture();
    const PauseFrameGate::Epoch pauseEpoch = m_pauseFrameGate.CurrentEpoch();
    const bool runningToPausedHold = m_pauseHoldActive.load(std::memory_order_acquire);
    const bool pausedFrameAlreadyPresented = !m_runActive.load(std::memory_order_acquire) &&
        m_pauseFrameGate.WasPresented(pauseEpoch);
    if (!runningToPausedHold && !pausedFrameAlreadyPresented)
        return waitForBootstrapCapacity();

    const uint32_t waitersBefore = m_pauseReceiveBackpressure.WaitingCount();
    LogInfo("Receive/PauseBackpressure", std::string("WAIT for Run; reason=") +
        (runningToPausedHold ? "strict Running-to-Paused hold" : "one-shot paused-frame presentation") +
        ", pauseEpoch=" +
        std::to_string(pauseEpoch) + ", runEpoch=" + std::to_string(token.runEpoch) +
        ", cancelEpoch=" + std::to_string(token.cancelEpoch) +
        ", existingWaiters=" + std::to_string(waitersBefore) +
        "; m_csReceive is not held while upstream is backpressured.");
    const auto waitStarted = std::chrono::steady_clock::now();
    const PauseReceiveBackpressure::WaitResult waitResult =
        m_pauseReceiveBackpressure.WaitForChange(token);
    const double waitElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - waitStarted).count();

    if (waitResult == PauseReceiveBackpressure::WaitResult::Failed) {
        LogError("Receive/PauseBackpressure", "The pause/resume condition-variable wait failed; aborting the pending source sample.");
        return E_FAIL;
    }
    if (waitResult == PauseReceiveBackpressure::WaitResult::Cancelled) {
        if (m_runtimeFailed.load(std::memory_order_acquire)) return E_FAIL;
        if (m_stopRequested.load(std::memory_order_acquire)) return VFW_E_WRONG_STATE;
        LogInfo("Receive/PauseBackpressure", "CANCELLED by Stop/Flush/seek/runtime lifecycle epoch; pending source sample was not admitted. waitMs=" +
            std::to_string(waitElapsedMs));
        return S_FALSE;
    }

    LogInfo("Receive/PauseBackpressure", "RESUMED by Run epoch=" +
        std::to_string(m_pauseReceiveBackpressure.RunEpoch()) +
        "; the exact pending upstream sample is now eligible for normal processing. waitMs=" +
        std::to_string(waitElapsedMs));
    return waitForBootstrapCapacity();
}

bool CDirectMotion::ShouldWaitForReceiveAdmission(IMediaSample* sample) {
    UNREFERENCED_PARAMETER(sample);
    const AM_SAMPLE2_PROPERTIES* properties = m_pInput ? m_pInput->SampleProps() : nullptr;
    if (properties && properties->dwStreamId != AM_STREAM_MEDIA) return false;
    const bool coldStartPreroll = DirectMotionInitialPrerollPolicy::AllowColdStartPreroll(
        m_initialPrerollActive.load(std::memory_order_acquire),
        m_runActive.load(std::memory_order_acquire),
        m_pauseHoldActive.load(std::memory_order_acquire));
    const bool pauseBackpressure = !coldStartPreroll &&
        (m_pauseHoldActive.load(std::memory_order_acquire) ||
            (!m_runActive.load(std::memory_order_acquire) &&
                m_pauseFrameGate.WasPresented(m_pauseFrameGate.CurrentEpoch())));
    if (pauseBackpressure) return true;
    return m_sourceBootstrapQueue.ShouldBackpressure(NormalizedSourceFrameBytes());
}


STDMETHODIMP CDirectMotion::Pause() {
    const unsigned long long transitionId = ++g_transitionSequence;
    const auto started = std::chrono::steady_clock::now();
    FILTER_STATE previousState = State_Stopped;
    const HRESULT stateHr = GetState(0, &previousState);
    const bool runningToPaused = previousState == State_Running;
    if (previousState == State_Stopped) {
        // Initial DirectShow preroll is a graph-startup operation, not a manual
        // paused-frame step. Let source samples reach the AviSynth/MVTools cache
        // until Run (or Stop) so the three-frame bootstrap can complete.
        m_initialPrerollActive.store(true, std::memory_order_release);
    } else if (runningToPaused) {
        m_initialPrerollActive.store(false, std::memory_order_release);
    }
    LogInfo("Pause", std::string("ENTER transitionId=") + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", GetStateHr=" + std::to_string(static_cast<unsigned long>(stateHr)) +
        ", previousState=" + (previousState == State_Running ? "Running" :
            previousState == State_Paused ? "Paused" : "Stopped") +
        ", runningToPaused=" + std::to_string(runningToPaused ? 1 : 0) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    LogTransitionCaller("Pause", transitionId);

    // StartStreaming has already created both pipeline workers so the processing
    // worker can synthesize the initial paused preroll and the delivery worker can
    // submit it downstream. This marker remains diagnostic only and is not a Receive gate.
    m_pauseCallInProgress.store(true, std::memory_order_release);
    DirectMotionTraySetGraphRunning(false);

    // Pause must publish cancellation and flush downstream BEFORE taking
    // m_deliveryMutex. DeliverInterpolated owns that mutex while GetDeliveryBuffer
    // waits for a renderer sample; waiting for the mutex first would prevent the
    // very BeginFlush needed to release the allocator wait.
    bool localPauseFlushStarted = false;
    if (runningToPaused) {
        // Close Receive admission immediately at the Running -> Paused boundary.
        // The renderer already owns the last valid output frame; do not deliver the
        // next decoded source sample as a visible paused-frame step.
        m_pauseHoldActive.store(true, std::memory_order_release);
        const PauseFrameGate::Epoch pauseFrameEpoch = m_pauseFrameGate.Invalidate();
        m_runActive.store(false, std::memory_order_release);
        // Publish the monotonic epoch before the interruption flag. Any worker that
        // observes pauseTransition=true with acquire semantics must also observe the
        // new epoch, so a concurrently completed GetFrame cannot be dropped in the
        // narrow publication window between the two atomics.
        m_strictPauseEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_pauseTransition.store(true, std::memory_order_release);
        DirectMotionTraySetStatus(DirectMotionStatus::Standby);
        LogInfo("Pause/PausedHold", "Enabled strict frame hold for Running-to-Paused epoch=" +
            std::to_string(pauseFrameEpoch) + "; the renderer keeps its last delivered image and the next source Receive waits upstream for Run.");
        bool terminalEosActiveAtPauseEntry = false;
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            terminalEosActiveAtPauseEntry = m_processingEosRequested && !m_processingEosCompleted;
        }
        if (terminalEosActiveAtPauseEntry) {
            LogInfo("Pause/AsyncPipeline", "An ordered terminal EOS drain is already active; preserving its source requests while flushing only the downstream allocator wait.");
        }

        const bool externalFlushActive = m_flushTransition.Active();
        const bool stopAlreadyFlushing = m_stopRequested.load(std::memory_order_acquire);
        if (!externalFlushActive && !stopAlreadyFlushing && m_pOutput && m_pOutput->IsConnected()) {
            const auto flushStarted = std::chrono::steady_clock::now();
            LogInfo("Pause/LocalFlush", "DeliverBeginFlush BEGIN before delivery fence; releasing any in-flight GetDeliveryBuffer/Deliver call.");
            const HRESULT beginFlushHr = m_pOutput->DeliverBeginFlush();
            const double flushElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - flushStarted).count();
            LogInfo("Pause/LocalFlush", "DeliverBeginFlush END, HRESULT=" +
                std::to_string(static_cast<unsigned long>(beginFlushHr)) +
                ", elapsedMs=" + std::to_string(flushElapsedMs));
            if (FAILED(beginFlushHr)) {
                // The base filter is still Running at this point. Roll back our
                // transition markers rather than entering a Pause state whose active
                // downstream delivery could not be quiesced safely.
                m_pauseCallInProgress.store(false, std::memory_order_release);
                m_pauseTransition.store(false, std::memory_order_release);
                m_runActive.store(true, std::memory_order_release);
                m_pauseHoldActive.store(false, std::memory_order_release);
                m_pauseReceiveBackpressure.NotifyRun();
                NotifyDeliveryWorker();
                DirectMotionTraySetGraphRunning(true);
                SetEvaluationEnabled(true, "Pause local downstream BeginFlush failed; restoring the running state.");
                NotifyProcessingWorker();
                LogError("Pause/LocalFlush", "Could not release the active downstream delivery before Pause; HRESULT=" +
                    std::to_string(static_cast<unsigned long>(beginFlushHr)) + ". Pause was not committed.");
                return beginFlushHr;
            }
            localPauseFlushStarted = true;
        } else if (externalFlushActive || stopAlreadyFlushing) {
            LogInfo("Pause/LocalFlush", "Skipped a nested local downstream BeginFlush because an external Flush/Stop already owns the transition.");
        }

        // Do not interrupt AviSynth source requests on a strict Running -> Paused
        // hold. The live source cache may already contain several seconds of decoded
        // samples ahead of the last displayed frame. Interrupting and rebuilding it
        // on Run would discard that history and re-anchor timestamps to the next
        // upstream sample, producing a visible jump and a future-PTS renderer stall.
        // A GetFrame waiting for its next source sample is allowed to sleep until Run
        // releases the exact pending Receive. The delivery worker is independently
        // interrupted by the local downstream flush below and retains its packet.
    }

    LogInfo("Pause/Base", "CTransformFilter::Pause ENTER, transitionId=" +
        std::to_string(transitionId) + ", threadId=" + std::to_string(GetCurrentThreadId()));
    const auto baseStarted = std::chrono::steady_clock::now();
    const HRESULT hr = CTransformFilter::Pause();
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    LogInfo("Pause/Base", "CTransformFilter::Pause EXIT, transitionId=" +
        std::to_string(transitionId) + ", HRESULT=" +
        std::to_string(static_cast<unsigned long>(hr)) +
        ", elapsedMs=" + std::to_string(baseElapsedMs));
    if (FAILED(hr)) {
        HRESULT endFlushHr = S_OK;
        if (localPauseFlushStarted && m_pOutput && m_pOutput->IsConnected()) {
            endFlushHr = m_pOutput->DeliverEndFlush();
            LogInfo("Pause/LocalFlush", "Paired DeliverEndFlush after base Pause failure, HRESULT=" +
                std::to_string(static_cast<unsigned long>(endFlushHr)));
        }
        if (runningToPaused) {
            m_pauseTransition.store(false, std::memory_order_release);
            m_runActive.store(true, std::memory_order_release);
            m_pauseHoldActive.store(false, std::memory_order_release);
            m_pauseReceiveBackpressure.NotifyRun();
            NotifyDeliveryWorker();
            DirectMotionTraySetGraphRunning(true);
            SetEvaluationEnabled(true, "DirectShow Pause failed; restoring the running state.");
            NotifyProcessingWorker();
        } else if (previousState == State_Stopped) {
            m_pauseTransition.store(false, std::memory_order_release);
            m_runActive.store(false, std::memory_order_release);
            m_initialPrerollActive.store(false, std::memory_order_release);
        }
        m_pauseCallInProgress.store(false, std::memory_order_release);
        if (FAILED(endFlushHr)) {
            LogError("Pause/LocalFlush", "Could not pair the local downstream EndFlush after base Pause failed; HRESULT=" +
                std::to_string(static_cast<unsigned long>(endFlushHr)));
        }
    } else if (previousState == State_Stopped) {
        m_runActive.store(false, std::memory_order_release);
        m_pauseTransition.store(false, std::memory_order_release);
        m_pauseCallInProgress.store(false, std::memory_order_release);
    } else if (runningToPaused) {
        bool terminalEosActive = false;
        AsyncPipelineState::Generation oldGeneration = 0;
        AsyncPipelineState::Generation newGeneration = 0;
        const auto fenceStarted = std::chrono::steady_clock::now();
        LogInfo("Pause/DeliveryFence", "WAIT_BEGIN acquiring delivery fence after downstream flush; localFlushOwned=" +
            std::to_string(localPauseFlushStarted ? 1 : 0));
        {
            // Downstream BeginFlush above releases a worker blocked in GetDeliveryBuffer
            // or Deliver. Only now is it safe to acquire the fence and retire the old
            // output timeline. Terminal EOS retains its generation so ordered EOS
            // completion can continue as non-visible preroll.
            std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
            {
                std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
                terminalEosActive = m_processingEosRequested && !m_processingEosCompleted;
            }
            if (!terminalEosActive) {
                // This is a presentation hold, not a source discontinuity. Keep the
                // same FIFO generation, pending output packets, AviSynth Prefetch state
                // and rolling source cache. DeliveryWorkerMain parks/retries its active
                // packet across the local BeginFlush/EndFlush pair; Run then resumes it.
                oldGeneration = m_asyncPipeline.CurrentGeneration();
                newGeneration = oldGeneration;
            }
            // Keep the delivery fence while reopening downstream. The worker cannot
            // acquire a fresh allocator sample until the local flush is fully paired.
            if (localPauseFlushStarted && m_pOutput && m_pOutput->IsConnected()) {
                const auto endFlushStarted = std::chrono::steady_clock::now();
                LogInfo("Pause/LocalFlush", "DeliverEndFlush BEGIN after delivery fence; terminalEosActive=" +
                    std::to_string(terminalEosActive ? 1 : 0));
                const HRESULT endFlushHr = m_pOutput->DeliverEndFlush();
                const double endFlushElapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - endFlushStarted).count();
                LogInfo("Pause/LocalFlush", "DeliverEndFlush END, HRESULT=" +
                    std::to_string(static_cast<unsigned long>(endFlushHr)) +
                    ", elapsedMs=" + std::to_string(endFlushElapsedMs));
                if (FAILED(endFlushHr)) {
                    LogError("Pause/LocalFlush", "Could not reopen downstream after the local Pause flush; HRESULT=" +
                        std::to_string(static_cast<unsigned long>(endFlushHr)));
                    // Continue the state bookkeeping below, but report the downstream
                    // transition failure to the graph manager.
                    m_pauseCallInProgress.store(false, std::memory_order_release);
                    LogInfo("Pause/DeliveryFence", "WAIT_END fence acquired; elapsedMs=" +
                        std::to_string(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - fenceStarted).count()));
                    LogInfo("Pause", "EXIT transitionId=" + std::to_string(transitionId) +
                        ", threadId=" + std::to_string(GetCurrentThreadId()) +
                        ", HRESULT=" + std::to_string(static_cast<unsigned long>(endFlushHr)) +
                        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
                        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started).count()) +
                        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0));
                    return endFlushHr;
                }
            }
        }
        LogInfo("Pause/DeliveryFence", "WAIT_END fence acquired; elapsedMs=" +
            std::to_string(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - fenceStarted).count()));
        m_pauseCallInProgress.store(false, std::memory_order_release);
        if (terminalEosActive) {
            // Keep this generation alive: terminal output may still be queued. The
            // delivery worker marks final samples as preroll while paused, and the
            // ordered drain barrier lets EndOfStream follow them without display.
            LogInfo("Pause/AsyncPipeline", "Pause overlapped an active EOS drain; downstream allocator wait was flushed and its generation retained so remaining terminal frames can complete as non-visible preroll before downstream EOS.");
        } else {
            // Keep the running timeline and graph intact. The only interrupted work
            // is the currently active downstream allocator/Receive call; its packet
            // and the remainder of the FIFO are preserved for ordered delivery on Run.
            LogInfo("Pause/AsyncPipeline", "Preserved the running output timeline and AviSynth source cache across strict Pause. generation=" +
                std::to_string(oldGeneration) + ", generationAfterPause=" +
                std::to_string(newGeneration) +
                "; queued frames and source history will resume in order without rebuilding the graph.");
        }
    }

    // Also covers an idempotent Pause call when the filter was already Paused.
    m_pauseCallInProgress.store(false, std::memory_order_release);
    LogInfo("Pause", "EXIT transitionId=" + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0));
    return hr;
}

STDMETHODIMP CDirectMotion::Stop() {
    const unsigned long long transitionId = ++g_transitionSequence;
    const auto started = std::chrono::steady_clock::now();
    const bool externalFlushAlreadyActive = m_flushTransition.Active();
    bool localStopFlushStarted = false;
    // Invalidate a paused-frame allocator wait before publishing shutdown.
    m_pauseFrameGate.Invalidate();
    m_initialPrerollActive.store(false, std::memory_order_release);
    // Publish transition flags before interrupting downstream, so any Receive/Deliver
    // unwinding is recognized as shutdown rather than a backend error.
    m_runActive.store(false, std::memory_order_release);
    m_pauseHoldActive.store(false, std::memory_order_release);
    DirectMotionTraySetGraphRunning(false);
    m_stopRequested.store(true, std::memory_order_release);
    NotifyDeliveryWorker();
    m_pauseReceiveBackpressure.Cancel();
    InterruptPendingSourceRequests("Stop");
    LogInfo("Stop", "ENTER transitionId=" + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(externalFlushAlreadyActive ? 1 : 0));
    LogTransitionCaller("Stop", transitionId);
    if (!externalFlushAlreadyActive && m_pOutput && m_pOutput->IsConnected()) {
        LogInfo("Stop/Flush", "DeliverBeginFlush BEGIN; releasing any in-flight delivery-worker allocator/Deliver call.");
        const HRESULT flushHr = m_pOutput->DeliverBeginFlush();
        localStopFlushStarted = SUCCEEDED(flushHr);
        LogInfo("Stop/Flush", "DeliverBeginFlush END, HRESULT=" +
            std::to_string(static_cast<unsigned long>(flushHr)) +
            ", localFlushOwned=" + std::to_string(localStopFlushStarted ? 1 : 0));
    }
    const auto baseStarted = std::chrono::steady_clock::now();
    const HRESULT hr = CTransformFilter::Stop();
    m_pauseCallInProgress.store(false, std::memory_order_release);
    m_pauseTransition.store(false, std::memory_order_release);
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    LogInfo("Stop/Base", "CTransformFilter::Stop returned, transitionId=" +
        std::to_string(transitionId) + ", HRESULT=" +
        std::to_string(static_cast<unsigned long>(hr)) + ", elapsedMs=" + std::to_string(baseElapsedMs));
    if (localStopFlushStarted && m_pOutput && m_pOutput->IsConnected()) {
        const HRESULT endFlushHr = m_pOutput->DeliverEndFlush();
        LogInfo("Stop/Flush", "DeliverEndFlush paired with local Stop flush, HRESULT=" +
            std::to_string(static_cast<unsigned long>(endFlushHr)));
    }
    LogInfo("Stop", "EXIT transitionId=" + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()));
    return hr;
}

HRESULT CDirectMotion::Run(REFERENCE_TIME start) {
    const unsigned long long transitionId = ++g_transitionSequence;
    const auto started = std::chrono::steady_clock::now();
    LogInfo("Run", "ENTER transitionId=" + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", start100ns=" + std::to_string(start) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    LogTransitionCaller("Run", transitionId);
    // Never resume DirectShow's base graph after the interpolation runtime has failed:
    // without this guard audio could resume while the video output remains frozen.
    if (m_runtimeFailed.load(std::memory_order_acquire)) {
        LogError("Run", "Rejected Run because the interpolation runtime previously failed; no pass-through fallback is available.");
        return E_FAIL;
    }
    const bool resumedAfterInitialPreroll = m_initialPrerollActive.load(
        std::memory_order_acquire);
    bool pendingPausedFrameRequest = false;
    const bool resumedAfterPausedFrameStep = m_pauseFrameGate.AdvanceForRun(&pendingPausedFrameRequest);
    const bool resumedAfterStrictPauseHold = m_pauseHoldActive.load(std::memory_order_acquire);
    if (pendingPausedFrameRequest && !m_stopRequested.load(std::memory_order_acquire) &&
        !m_flushTransition.Active() && m_pOutput && m_pOutput->IsConnected()) {
        // A paused Receive may still own m_deliveryMutex while blocked in the
        // downstream allocator or renderer. Run invalidates its epoch above; flush
        // now to wake that call before taking the same delivery fence. Otherwise the
        // obsolete Receive can remain blocked until an unrelated future Pause/Stop.
        const auto flushStarted = std::chrono::steady_clock::now();
        LogInfo("Run/PausedFrameFlush", "DeliverBeginFlush BEGIN to cancel an outstanding paused-frame allocator/Deliver request before resuming.");
        const HRESULT beginFlushHr = m_pOutput->DeliverBeginFlush();
        LogInfo("Run/PausedFrameFlush", "DeliverBeginFlush END, HRESULT=" +
            std::to_string(static_cast<unsigned long>(beginFlushHr)) +
            ", elapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - flushStarted).count()));
        if (FAILED(beginFlushHr)) {
            m_pauseTransition.store(true, std::memory_order_release);
            m_runActive.store(false, std::memory_order_release);
            DirectMotionTraySetGraphRunning(false);
            LogError("Run/PausedFrameFlush", "Could not cancel the superseded paused-frame allocator request; refusing to resume with an in-flight stale Receive. HRESULT=" +
                std::to_string(static_cast<unsigned long>(beginFlushHr)));
            return beginFlushHr;
        }
        HRESULT endFlushHr = S_OK;
        {
            std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
            LogInfo("Run/PausedFrameFlush", "Delivery fence acquired after cancelling the stale paused-frame request; pairing DeliverEndFlush before Run.");
            endFlushHr = m_pOutput->DeliverEndFlush();
        }
        if (FAILED(endFlushHr)) {
            LogError("Run/PausedFrameFlush", "Could not pair the local DeliverEndFlush after cancelling the stale paused-frame request; refusing to resume. HRESULT=" +
                std::to_string(static_cast<unsigned long>(endFlushHr)));
            FailRuntime("Run/PausedFrameFlush", "Downstream EndFlush failed after cancelling an obsolete paused-frame allocator request.", endFlushHr);
            return endFlushHr;
        }
        LogInfo("Run/PausedFrameFlush", "DeliverEndFlush completed; the stale paused-frame Receive has been fenced out before resuming.");
    }
    m_pauseCallInProgress.store(false, std::memory_order_release);
    m_pauseTransition.store(false, std::memory_order_release);
    if (m_processingNeedsReset.exchange(false, std::memory_order_acq_rel)) {
        LogInfo("Run/AsyncProcessing", "Retiring a graph generation interrupted by paused allocator decommit before accepting new running input.");
        ResetEngine(resumedAfterStrictPauseHold);
        if (resumedAfterStrictPauseHold) {
            LogInfo("Run/WarmStart", "Prepared a resume warm-start graph. The next engine will enter Prefetch after MVTools' minimum source lookahead instead of staging the full Prefetch window before requesting the first output.");
        }
    }
    if (resumedAfterPausedFrameStep) {
        LogInfo("Run/PausedFrameStep",
            "Resuming after direct presentation of a paused decoded frame; the fresh interpolation generation will anchor output PTS to the next timed source sample.");
    }
    if (resumedAfterStrictPauseHold) {
        LogInfo("Run/PauseHold", "Resuming from strict Running-to-Paused hold; no new source image was presented while paused, and the exact pending upstream Receive is being released.");
    }
    if (resumedAfterInitialPreroll) {
        LogInfo("Run/InitialPreroll", "Leaving cold-start preroll mode; startup source samples were admitted to AviSynth/MVTools instead of being consumed by the paused-frame one-shot.");
    }
    // Publish Running before clearing the cold-start exception. The policy helper
    // gives runActive precedence, so no concurrent Receive can briefly fall into
    // the manual paused-frame one-shot between these two lifecycle stores.
    m_runActive.store(true, std::memory_order_release);
    m_initialPrerollActive.store(false, std::memory_order_release);
    // For a strict resume, keep the delivery worker parked until the base filter has
    // accepted Run. The upstream Receive may resume synchronously during base Run,
    // but ProcessingWorker remains suspended by the old pause hold; it is notified
    // again only after the base Run succeeds and the hold is cleared.
    if (!resumedAfterStrictPauseHold)
        m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.NotifyRun();
    NotifyProcessingWorker();
    DirectMotionTraySetGraphRunning(true);
    SetEvaluationEnabled(true, "DirectShow Run");
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    const auto baseStarted = std::chrono::steady_clock::now();
    // Publish Running before calling the base class: it can resume upstream delivery
    // synchronously before CTransformFilter::Run returns.
    const HRESULT hr = CTransformFilter::Run(start);
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    HRESULT resultHr = hr;
    if (FAILED(hr)) {
        // A failed base Run returns a strict resume to the same paused hold. Advance
        // the epoch before publishing pauseTransition so a GetFrame/Deliver call that
        // overlapped the failed Run is retained for the next successful Run.
        if (resumedAfterStrictPauseHold) {
            m_strictPauseEpoch.fetch_add(1, std::memory_order_acq_rel);
            m_pauseHoldActive.store(true, std::memory_order_release);
        }
        m_pauseTransition.store(true, std::memory_order_release);
        // Restore the appropriate paused admission policy before publishing
        // runActive=false. Otherwise a concurrent Receive can briefly observe
        // neither cold-start admission nor the strict pause-hold marker.
        if (resumedAfterInitialPreroll && !m_stopRequested.load(std::memory_order_acquire))
            m_initialPrerollActive.store(true, std::memory_order_release);
        m_runActive.store(false, std::memory_order_release);
        DirectMotionTraySetGraphRunning(false);
        SetEvaluationEnabled(true, "DirectShow Run failed; the graph remains stopped from playback delivery");
    } else {
        m_pauseCallInProgress.store(false, std::memory_order_release);
        m_pauseTransition.store(false, std::memory_order_release);
        m_runActive.store(true, std::memory_order_release);
        if (resumedAfterStrictPauseHold) {
            m_pauseHoldActive.store(false, std::memory_order_release);
            NotifyDeliveryWorker();
            // ProcessingWorker may have returned after observing the strict hold.
            // Wake it only after the base filter has successfully accepted Run.
            NotifyProcessingWorker();
        }
        SetEvaluationEnabled(true, "DirectShow Run succeeded");
    }
    LogInfo("Run", "EXIT transitionId=" + std::to_string(transitionId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(resultHr)) +
        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()));
    return resultHr;
}

bool CDirectMotion::ParseInputMediaType(const CMediaType& mediaType) {
    const bool isNv12 = IsNv12MediaSubtype(mediaType.subtype);
    const bool isNv21 = (mediaType.subtype == kSubtypeNV21 ||
        IsFourCCSubtype(mediaType.subtype, MAKEFOURCC('N','V','2','1')));
    const int layout = isNv21 ? kLayoutNV21 : kLayoutNV12;
    if ((!isNv12 && !isNv21) || mediaType.majortype != MEDIATYPE_Video ||
        (mediaType.formattype != FORMAT_VideoInfo && mediaType.formattype != FORMAT_VideoInfo2)) return false;

    int surfaceWidth = 0, surfaceHeight = 0;
    RECT visible{};
    if (!GetSurfaceSize(mediaType, surfaceWidth, surfaceHeight) ||
        !GetVisibleRect(mediaType, visible)) return false;
    const int width = visible.right - visible.left;
    const int height = visible.bottom - visible.top;
    if (width < 16 || height < 16 || ((visible.left | visible.top | width | height) & 1) != 0 ||
        surfaceWidth < visible.right || surfaceHeight < visible.bottom ||
        InputMediaTypeRequiresScaling(mediaType, width, height)) return false;

    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(mediaType, vi, vi2)) return false;
    const BITMAPINFOHEADER& bmi = vi2 ? vi2->bmiHeader : vi->bmiHeader;
    const bool interlacedTwoFieldsPerSample = vi2 &&
        (vi2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0 &&
        (vi2->dwInterlaceFlags & AMINTERLACE_1FieldPerSample) == 0;
    m_inputInterlacedTwoFieldsPerSample.store(interlacedTwoFieldsPerSample,
        std::memory_order_release);
    m_interpolatedTypeSpecificFlags.store(0, std::memory_order_release);
    // DirectShow YUV is top-down regardless of biHeight's sign. Decoders should
    // offer positive YUV heights, but compatible filters must accept either sign.
    // GetSurfaceSize() has already normalized the magnitude without changing row order.
    int inputStride = surfaceWidth;
    const int imageSize = GetImageSize(mediaType);
    const size_t surfaceRows = static_cast<size_t>(surfaceHeight) * 3u / 2u;
    if (imageSize > 0 && surfaceRows > 0 &&
        static_cast<size_t>(imageSize) % surfaceRows == 0) {
        const size_t inferred = static_cast<size_t>(imageSize) / surfaceRows;
        if (inferred >= static_cast<size_t>(surfaceWidth) &&
            inferred <= static_cast<size_t>(surfaceWidth) + 512u &&
            inferred <= static_cast<size_t>((std::numeric_limits<int>::max)())) {
            inputStride = static_cast<int>(inferred);
        }
    }
    if ((inputStride & 1) != 0 || inputStride < visible.right) return false;
    const size_t requiredInputBytes = Nv12BytesForStride(inputStride, surfaceHeight);
    if (requiredInputBytes == 0 ||
        requiredInputBytes > static_cast<size_t>((std::numeric_limits<LONG>::max)())) return false;

    m_width = width;
    m_height = height;
    m_inputSurfaceHeight = surfaceHeight;
    m_inputCropLeft = visible.left;
    m_inputCropTop = visible.top;
    m_inputLayout = layout;
    m_inputBottomUp = false;
    m_inputStride = inputStride;
    m_chromaStride = inputStride;
    // Only the minimum addressable semi-planar surface is required. A larger
    // biSizeImage may include trailing padding; it must not move the UV plane.
    m_inputBufferBytes = requiredInputBytes;
    m_outputStride = width;
    m_outputSurfaceHeight = height;
    m_outputTargetLeft = 0;
    m_outputTargetTop = 0;
    m_outputBufferBytes = Nv12BytesForStride(m_outputStride, m_outputSurfaceHeight);
    m_sourceRate = SourceFrameRate();

    const RECT rcTarget = vi2 ? vi2->rcTarget : vi->rcTarget;
    DirectMotionLog("INFO", "ParseInputMediaType", "semi-planar direct path (NV12/NV21 normalized into tightly packed visible NV12): layout=" +
        std::string(LayoutName(layout)) + ", visible=" + std::to_string(width) + "x" + std::to_string(height) +
        ", surface=" + std::to_string(surfaceWidth) + "x" + std::to_string(surfaceHeight) +
        ", crop=" + std::to_string(visible.left) + "," + std::to_string(visible.top) +
        ", stridePixels=" + std::to_string(m_inputStride) +
        ", requiredSurfaceBytes=" + std::to_string(m_inputBufferBytes) +
        ", interlacedTwoFieldsPerSample=" + std::to_string(interlacedTwoFieldsPerSample ? 1 : 0) +
        ", biSizeImage=" + std::to_string(imageSize) +
        ", rcTarget=" + std::to_string(rcTarget.left) + "," + std::to_string(rcTarget.top) + "," +
            std::to_string(rcTarget.right) + "," + std::to_string(rcTarget.bottom) +
        ", frameDuration100ns=" + std::to_string(GetAvgTimePerFrame(mediaType)) +
        "; surface pitch and visible picture dimensions are tracked independently.");
    return true;
}

HRESULT CDirectMotion::ApplyDynamicInputSampleType(IMediaSample* sample, const char* site) {
    if (!sample) return E_POINTER;
    if (!m_pInput || !m_pInput->IsConnected()) return VFW_E_NOT_CONNECTED;
    AM_MEDIA_TYPE* rawType = nullptr;
    const HRESULT getHr = sample->GetMediaType(&rawType);
    if (getHr == S_FALSE || (SUCCEEDED(getHr) && !rawType)) return S_OK;
    if (FAILED(getHr) || !rawType) {
        if (rawType) DeleteMediaType(rawType);
        LogError(site, "Input sample GetMediaType failed, HRESULT=" +
            std::to_string(static_cast<unsigned long>(getHr)));
        return FAILED(getHr) ? getHr : E_FAIL;
    }

    HRESULT copyHr = S_OK;
    CMediaType dynamicType(*rawType, &copyHr);
    DeleteMediaType(rawType);
    if (FAILED(copyHr)) return copyHr;
    int visibleWidth = 0, visibleHeight = 0, surfaceWidth = 0, surfaceHeight = 0;
    RECT visible{};
    const bool valid = SUCCEEDED(CheckInputType(&dynamicType)) &&
        GetFrameSize(dynamicType, visibleWidth, visibleHeight) &&
        GetSurfaceSize(dynamicType, surfaceWidth, surfaceHeight) &&
        GetVisibleRect(dynamicType, visible);
    if (!valid) {
        LogError(site, "Dynamic input media type is not a valid supported NV12/NV21 surface. " +
            VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (dynamicType.subtype != m_pInput->CurrentMediaType().subtype ||
        visibleWidth != m_width || visibleHeight != m_height ||
        (GetAvgTimePerFrame(dynamicType) > 0 && GetAvgTimePerFrame(m_pInput->CurrentMediaType()) > 0 &&
         GetAvgTimePerFrame(dynamicType) != GetAvgTimePerFrame(m_pInput->CurrentMediaType()))) {
        LogError(site, "Dynamic input sample changes subtype, visible resolution or frame cadence midstream. "
            "DirectShow output renegotiation is required; refusing to feed the old AviSynth geometry. dynamic{" +
            VideoHeaderAudit(dynamicType) + "}");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (((visible.left | visible.top | visibleWidth | visibleHeight) & 1) != 0 ||
        surfaceWidth < visible.right || surfaceHeight < visible.bottom) {
        LogError(site, "Dynamic input crop/surface alignment is invalid for 4:2:0. " + VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    int stride = surfaceWidth;
    const int imageSize = GetImageSize(dynamicType);
    const size_t rows = static_cast<size_t>(surfaceHeight) * 3u / 2u;
    if (imageSize > 0 && rows > 0 && static_cast<size_t>(imageSize) % rows == 0) {
        const size_t inferred = static_cast<size_t>(imageSize) / rows;
        if (inferred >= static_cast<size_t>(surfaceWidth) &&
            inferred <= static_cast<size_t>(surfaceWidth) + 512u &&
            inferred <= static_cast<size_t>((std::numeric_limits<int>::max)())) stride = static_cast<int>(inferred);
    }
    if ((stride & 1) != 0 || stride < visible.right) {
        LogError(site, "Dynamic input stride is invalid for the visible rectangle. " + VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    const size_t required = Nv12BytesForStride(stride, surfaceHeight);
    const LONG actual = sample->GetActualDataLength();
    if (required == 0 || actual < 0 || static_cast<size_t>(actual) < required) {
        LogError(site, "Dynamic input sample buffer is shorter than its advertised surface: actual=" +
            std::to_string(actual) + ", required=" + std::to_string(required) + "; " + VideoHeaderAudit(dynamicType));
        return VFW_E_BUFFER_UNDERFLOW;
    }

    const bool changed = stride != m_inputStride || surfaceHeight != m_inputSurfaceHeight ||
        visible.left != m_inputCropLeft || visible.top != m_inputCropTop;
    VIDEOINFOHEADER* dynamicVi = nullptr;
    VIDEOINFOHEADER2* dynamicVi2 = nullptr;
    const bool dynamicInterlacedTwoFields = GetVideoInfo(dynamicType, dynamicVi, dynamicVi2) &&
        dynamicVi2 && (dynamicVi2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0 &&
        (dynamicVi2->dwInterlaceFlags & AMINTERLACE_1FieldPerSample) == 0;
    m_inputInterlacedTwoFieldsPerSample.store(dynamicInterlacedTwoFields,
        std::memory_order_release);
    m_inputStride = stride;
    m_inputSurfaceHeight = surfaceHeight;
    m_inputCropLeft = visible.left;
    m_inputCropTop = visible.top;
    m_chromaStride = stride;
    m_inputBufferBytes = required;
    if (changed) {
        LogInfo(site, "Applied per-sample NV12/NV21 surface layout without rebuilding the fixed-size AviSynth picture: visible=" +
            std::to_string(m_width) + "x" + std::to_string(m_height) +
            ", surface=" + std::to_string(surfaceWidth) + "x" + std::to_string(surfaceHeight) +
            ", crop=" + std::to_string(visible.left) + "," + std::to_string(visible.top) +
            ", stridePixels=" + std::to_string(stride) + ", requiredBytes=" + std::to_string(required));
    }
    return S_OK;
}

bool CDirectMotion::ConvertInputToNv12(const BYTE* input, std::vector<uint8_t>& nv12, std::string& error) {
    ScopedDiagnosticTimer conversionTimer(DirectMotionIsLoggingEnabled(),
        m_diagInputNormalizeSamples, m_diagInputNormalizeTotalNs,
        m_diagInputNormalizeLastNs, m_diagInputNormalizeMaxNs);
    if (!input || (m_inputLayout != kLayoutNV12 && m_inputLayout != kLayoutNV21) ||
        m_width <= 0 || m_height <= 0 || m_inputStride < m_width) {
        error = "Input normalization requires a valid NV12/NV21 sample and stride.";
        return false;
    }
    if (m_inputSurfaceHeight <= 0 || m_inputCropLeft < 0 || m_inputCropTop < 0 ||
        m_inputCropLeft + m_width > m_inputStride ||
        m_inputCropTop + m_height > m_inputSurfaceHeight ||
        ((m_inputCropLeft | m_inputCropTop) & 1) != 0) {
        error = "Input visible rectangle exceeds the negotiated NV12/NV21 surface.";
        return false;
    }
    const size_t yBytes = static_cast<size_t>(m_width) * m_height;
    nv12.assign(yBytes + yBytes / 2, 0);
    const uint8_t* srcY = input + static_cast<size_t>(m_inputCropTop) * m_inputStride + m_inputCropLeft;
    const uint8_t* srcUV = input + static_cast<size_t>(m_inputStride) * m_inputSurfaceHeight +
        static_cast<size_t>(m_inputCropTop / 2) * m_inputStride + m_inputCropLeft;
    for (int y = 0; y < m_height; ++y)
        std::memcpy(nv12.data() + static_cast<size_t>(y) * m_width,
                    srcY + static_cast<size_t>(y) * m_inputStride, m_width);
    for (int y = 0; y < m_height / 2; ++y) {
        uint8_t* dstUV = nv12.data() + yBytes + static_cast<size_t>(y) * m_width;
        const uint8_t* srcUVRow = srcUV + static_cast<size_t>(y) * m_inputStride;
        if (m_inputLayout == kLayoutNV12) {
            std::memcpy(dstUV, srcUVRow, m_width);
        } else {
            // NV21 stores VU pairs; AviSynth's internal YV12 is represented to
            // the engine as canonical NV12, so swap each pair while normalizing.
            for (int x = 0; x < m_width; x += 2) {
                dstUV[x] = srcUVRow[x + 1];
                dstUV[x + 1] = srcUVRow[x];
            }
        }
    }
    return true;
}

bool CDirectMotion::ConvertNv12ToOutput(const uint8_t* nv12, BYTE* output, size_t outputBytes, std::string& error) {
    ScopedDiagnosticTimer conversionTimer(DirectMotionIsLoggingEnabled(),
        m_diagOutputCopySamples, m_diagOutputCopyTotalNs,
        m_diagOutputCopyLastNs, m_diagOutputCopyMaxNs);
    if (!nv12 || !output || (m_inputLayout != kLayoutNV12 && m_inputLayout != kLayoutNV21) ||
        m_width <= 0 || m_height <= 0 || m_outputStride < m_outputTargetLeft + m_width ||
        m_outputSurfaceHeight < m_outputTargetTop + m_height || outputBytes < m_outputBufferBytes ||
        ((m_outputTargetLeft | m_outputTargetTop) & 1) != 0) {
        error = "Semi-planar output copy, target rectangle, physical surface, pitch, or capacity is invalid.";
        return false;
    }
    const size_t visibleYBytes = static_cast<size_t>(m_width) * m_height;
    const size_t outputYBytes = static_cast<size_t>(m_outputStride) * m_outputSurfaceHeight;
    for (int y = 0; y < m_height; ++y) {
        BYTE* row = output + static_cast<size_t>(m_outputTargetTop + y) * m_outputStride + m_outputTargetLeft;
        std::memcpy(row, nv12 + static_cast<size_t>(y) * m_width, m_width);
    }
    for (int y = 0; y < m_height / 2; ++y) {
        BYTE* row = output + outputYBytes +
            static_cast<size_t>(m_outputTargetTop / 2 + y) * m_outputStride + m_outputTargetLeft;
        const uint8_t* srcUV = nv12 + visibleYBytes + static_cast<size_t>(y) * m_width;
        if (m_inputLayout == kLayoutNV12) {
            std::memcpy(row, srcUV, m_width);
        } else {
            for (int x = 0; x < m_width; x += 2) {
                row[x] = srcUV[x + 1];
                row[x + 1] = srcUV[x];
            }
        }
    }
    return true;
}

FrucRational CDirectMotion::SourceFrameRate() const {
    if (m_pInput && m_pInput->IsConnected()) {
        const REFERENCE_TIME avg = GetAvgTimePerFrame(m_pInput->CurrentMediaType());
        // Convert the 100-ns average frame duration to a compact rate such as
        // 24000/1001 or 30000/1001. Keeping 10,000,000/avg unreduced in practice
        // can overflow output-index cross-multiplications at long playback times.
        if (avg > 0) return FrucRationalFromDouble(
            10000000.0 / static_cast<double>(avg), 1001);
    }
    return FrucRational{24, 1};
}

FrucRational CDirectMotion::TargetFrameRate() const {
    const FrucRational source = SourceFrameRate();
    switch (m_settings.rateMode) {
    case kRateMode25x:
        return FrucNormalizeRational(source.num * 5, source.den * 2);
    case kRateMode3x:
        return FrucNormalizeRational(source.num * 3, source.den);
    case kRateMode4x:
        return FrucNormalizeRational(source.num * 4, source.den);
    case kRateModeAuto: {
        if (m_autoDisplayRate.num <= 0 || m_autoDisplayRate.den <= 0)
            return FrucNormalizeRational(source.num * 2, source.den);
        // Interpolation may add frames, but it must not silently decimate a source
        // that already runs at or above the display refresh rate.
        const int64_t maxValue = (std::numeric_limits<int64_t>::max)();
        const bool crossProductsFit = m_autoDisplayRate.num <= maxValue / source.den &&
            source.num <= maxValue / m_autoDisplayRate.den;
        if (crossProductsFit) {
            return m_autoDisplayRate.num * source.den > source.num * m_autoDisplayRate.den
                ? m_autoDisplayRate : source;
        }
        // Defensive fallback for pathological rational inputs only; normal media
        // and display modes use the exact integer cross-product above.
        const long double displayFps = static_cast<long double>(m_autoDisplayRate.num) /
            static_cast<long double>(m_autoDisplayRate.den);
        const long double sourceFps = static_cast<long double>(source.num) /
            static_cast<long double>(source.den);
        return displayFps > sourceFps ? m_autoDisplayRate : source;
    }
    case kRateMode2x:
    default:
        return FrucNormalizeRational(source.num * 2, source.den);
    }
}

REFERENCE_TIME CDirectMotion::TargetFrameDuration() const {
    const FrucRational rate = m_settings.enabled ? TargetFrameRate() : SourceFrameRate();
    return FrucFrameDuration100ns(rate);
}

HRESULT CDirectMotion::ApplyAllocatorMediaType(IMediaSample* output, const char* site,
                                               bool logWhenUnchanged, bool* hasDynamicOutputType) {
    if (hasDynamicOutputType) *hasDynamicOutputType = false;
    (void)logWhenUnchanged; // Retained in the internal signature for source compatibility.
    if (!output) return E_POINTER;

    // Receive is overridden, so allocator-attached output media types are handled here.
    AM_MEDIA_TYPE* rawType = nullptr;
    const HRESULT getTypeHr = output->GetMediaType(&rawType);
    if (getTypeHr == S_FALSE || (SUCCEEDED(getTypeHr) && !rawType)) {
        return S_OK;
    }
    if (FAILED(getTypeHr) || !rawType) {
        if (rawType) DeleteMediaType(rawType);
        if (FAILED(getTypeHr)) {
            LogError(site, "Output sample GetMediaType failed, HRESULT=" +
                std::to_string(static_cast<unsigned long>(getTypeHr)));
        }
        return S_OK;
    }

    HRESULT copyTypeHr = S_OK;
    CMediaType dynamicType(*rawType, &copyTypeHr);
    DeleteMediaType(rawType);
    rawType = nullptr;
    if (FAILED(copyTypeHr)) {
        LogError(site, "Could not copy the renderer's dynamic output media type. HRESULT=" +
            std::to_string(static_cast<unsigned long>(copyTypeHr)));
        return copyTypeHr;
    }
    if (!m_pInput || !m_pInput->IsConnected()) {
        LogError(site, "Allocator attached a dynamic output media type while the input pin was disconnected.");
        return VFW_E_NOT_CONNECTED;
    }

    const HRESULT transformHr = CheckTransform(&m_pInput->CurrentMediaType(), &dynamicType);
    if (FAILED(transformHr)) {
        LogError(site, "Renderer allocator supplied an unsupported dynamic output type; " +
            VideoHeaderAudit(dynamicType));
        return transformHr;
    }

    int outputSurfaceWidth = 0, outputSurfaceHeight = 0;
    if (!GetSurfaceSize(dynamicType, outputSurfaceWidth, outputSurfaceHeight)) {
        LogError(site, "Renderer allocator supplied an invalid dynamic output surface size; " +
            VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    VIDEOINFOHEADER* rectVi = nullptr;
    VIDEOINFOHEADER2* rectVi2 = nullptr;
    if (!GetVideoInfo(dynamicType, rectVi, rectVi2)) return VFW_E_TYPE_NOT_ACCEPTED;
    const RECT rawSourceRect = rectVi2 ? rectVi2->rcSource : rectVi->rcSource;
    const RECT rawTargetRect = rectVi2 ? rectVi2->rcTarget : rectVi->rcTarget;
    const auto emptyRect = [](const RECT& r) { return r.right <= r.left || r.bottom <= r.top; };
    const RECT sourceRect = emptyRect(rawSourceRect)
        ? RECT{ 0, 0, m_width, m_height } : rawSourceRect;
    const RECT targetRect = emptyRect(rawTargetRect)
        ? RECT{ 0, 0, outputSurfaceWidth, outputSurfaceHeight } : rawTargetRect;
    const int64_t targetWidth = static_cast<int64_t>(targetRect.right) - targetRect.left;
    const int64_t targetHeight = static_cast<int64_t>(targetRect.bottom) - targetRect.top;
    const int maxStride = AlignNv12Stride(m_width + 1024);
    const int maxSurfaceHeight = m_height + 256;
    const bool sourceMatches = sourceRect.left == 0 && sourceRect.top == 0 &&
        sourceRect.right == m_width && sourceRect.bottom == m_height;
    const bool targetMatches = targetRect.left >= 0 && targetRect.top >= 0 &&
        targetWidth == m_width && targetHeight == m_height &&
        targetRect.right <= outputSurfaceWidth && targetRect.bottom <= outputSurfaceHeight &&
        ((targetRect.left | targetRect.top) & 1) == 0;
    if (!sourceMatches || !targetMatches || outputSurfaceWidth < targetRect.right ||
        outputSurfaceWidth > maxStride || (outputSurfaceWidth & 1) != 0 ||
        outputSurfaceHeight < targetRect.bottom || outputSurfaceHeight > maxSurfaceHeight ||
        (outputSurfaceHeight & 1) != 0) {
        LogError(site, "Renderer dynamic output type changes the visible picture or exceeds the supported padded NV12 surface; " +
            VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    const size_t expectedBytes = Nv12BytesForStride(outputSurfaceWidth, outputSurfaceHeight);
    if (expectedBytes == 0 || expectedBytes > static_cast<size_t>((std::numeric_limits<LONG>::max)())) {
        LogError(site, "Renderer dynamic output type produces an invalid NV12 payload size.");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    // UV placement uses physical surface height, not visible picture height.
    VIDEOINFOHEADER* vi = nullptr;
    VIDEOINFOHEADER2* vi2 = nullptr;
    if (!GetVideoInfo(dynamicType, vi, vi2)) return VFW_E_TYPE_NOT_ACCEPTED;
    BITMAPINFOHEADER& bmi = vi2 ? vi2->bmiHeader : vi->bmiHeader;
    const RECT visibleSourceRect{ 0, 0, m_width, m_height };
    if (vi2) {
        vi2->rcSource = visibleSourceRect;
        vi2->rcTarget = targetRect;
    } else {
        vi->rcSource = visibleSourceRect;
        vi->rcTarget = targetRect;
    }
    const DWORD expectedDwordBytes = static_cast<DWORD>(expectedBytes);
    const bool correctedSize = bmi.biWidth != outputSurfaceWidth || bmi.biHeight != outputSurfaceHeight ||
        bmi.biSizeImage != expectedDwordBytes ||
        dynamicType.lSampleSize != static_cast<ULONG>(expectedBytes) ||
        !dynamicType.bFixedSizeSamples || dynamicType.bTemporalCompression;
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = outputSurfaceWidth;
    bmi.biHeight = outputSurfaceHeight; // positive YUV height is recommended
    bmi.biPlanes = 1;
    bmi.biBitCount = 12;
    bmi.biCompression = (m_inputLayout == kLayoutNV21) ? MAKEFOURCC('N','V','2','1') : MAKEFOURCC('N','V','1','2');
    bmi.biSizeImage = expectedDwordBytes;
    dynamicType.SetSampleSize(static_cast<ULONG>(expectedBytes));
    dynamicType.bFixedSizeSamples = TRUE;
    dynamicType.SetTemporalCompression(FALSE);

    // Preserve allocator layout but restore the negotiated TargetFrameDuration().
    const REFERENCE_TIME rendererAvgTimePerFrame = GetAvgTimePerFrame(dynamicType);
    const REFERENCE_TIME negotiatedFrameDuration = TargetFrameDuration();
    if (negotiatedFrameDuration <= 0) {
        LogError(site, "Could not derive a valid negotiated output frame duration while normalizing the renderer's dynamic media type. " +
            VideoHeaderAudit(dynamicType));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    SetAvgTimePerFrame(dynamicType, negotiatedFrameDuration);
    const bool correctedCadence = rendererAvgTimePerFrame != negotiatedFrameDuration;

    HRESULT hr = output->SetMediaType(&dynamicType);
    if (FAILED(hr)) {
        LogError(site, "Could not preserve/normalize the renderer-requested media type on its allocator sample. HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }
    hr = SetMediaType(PINDIR_OUTPUT, &dynamicType);
    if (FAILED(hr)) {
        LogError(site, "Could not apply renderer-requested output media type to directmotion. HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }
    m_pOutput->CurrentMediaType() = dynamicType;
    if (hasDynamicOutputType) *hasDynamicOutputType = true;
    LogInfo(site, "Applied dynamic output type carried by downstream allocator: surface=" +
        std::to_string(outputSurfaceWidth) + "x" + std::to_string(outputSurfaceHeight) +
        ", visible=" + std::to_string(m_width) + "x" + std::to_string(m_height) +
        ", pitchPixels=" + std::to_string(outputSurfaceWidth) +
        ", targetRect=" + std::to_string(targetRect.left) + "," + std::to_string(targetRect.top) + "," +
            std::to_string(targetRect.right) + "," + std::to_string(targetRect.bottom) +
        ", expectedBytes=" + std::to_string(expectedBytes) +
        ", allocatorCapacity=" + std::to_string(output->GetSize()) +
        ", correctedSizeFields=" + std::to_string(correctedSize ? 1 : 0) +
        ", rendererAvgTimePerFrame=" + std::to_string(rendererAvgTimePerFrame) +
        ", negotiatedAvgTimePerFrame=" + std::to_string(negotiatedFrameDuration) +
        ", correctedCadence=" + std::to_string(correctedCadence ? 1 : 0) +
        ", outputFPS=" + std::to_string(10000000.0 / static_cast<double>(negotiatedFrameDuration)) +
        ", header{" + VideoHeaderAudit(dynamicType) + "}");
    return S_OK;
}

HRESULT CDirectMotion::DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* properties) {
    if (!allocator || !properties) return E_POINTER;
    int width = m_width, height = m_height;
    if ((width <= 0 || height <= 0) && m_pInput && m_pInput->IsConnected()) {
        GetFrameSize(m_pInput->CurrentMediaType(), width, height);
    }
    if (width <= 0 || height <= 0) return E_FAIL;

    size_t requiredSize = m_outputBufferBytes;
    if (requiredSize == 0) {
        if (m_pInput && m_pInput->IsConnected()) requiredSize = static_cast<size_t>(GetImageSize(m_pInput->CurrentMediaType()));
        if (requiredSize == 0) requiredSize = static_cast<size_t>(Align4(width)) * height * 3 / 2;
    }
    if (requiredSize > static_cast<size_t>((std::numeric_limits<LONG>::max)())) return E_INVALIDARG;
    const LONG requiredBytes = static_cast<LONG>(requiredSize);
    properties->cBuffers = std::max<LONG>(properties->cBuffers, 4);
    properties->cbBuffer = std::max<LONG>(properties->cbBuffer, requiredBytes);
    properties->cbAlign = std::max<LONG>(properties->cbAlign, 1);
    properties->cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual{};
    HRESULT hr = allocator->SetProperties(properties, &actual);
    LogInfo("DecideBufferSize", "Requested buffers=" + std::to_string(properties->cBuffers) +
        ", bytes=" + std::to_string(properties->cbBuffer) +
        ", align=" + std::to_string(properties->cbAlign) +
        ", prefix=" + std::to_string(properties->cbPrefix) +
        "; actual buffers=" + std::to_string(actual.cBuffers) +
        ", bytes=" + std::to_string(actual.cbBuffer) +
        ", align=" + std::to_string(actual.cbAlign) +
        ", prefix=" + std::to_string(actual.cbPrefix) +
        ", visible=" + std::to_string(m_width) + "x" + std::to_string(m_height) +
        ", stridePixels=" + std::to_string(m_outputStride) +
        ", requiredBytes=" + std::to_string(requiredBytes) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
    if (FAILED(hr)) return hr;
    if (actual.cbBuffer < requiredBytes || actual.cBuffers < 1) return E_FAIL;
    return S_OK;
}

bool CDirectMotion::IsReceiveCommitCurrent(AsyncPipelineState::Generation generation) const noexcept {
    // Read the lifecycle flags on both sides of the generation check. BeginFlush
    // and NewSegment announce their transition before retiring the pipeline, so
    // either the lifecycle flag or the generation mismatch rejects an old sample.
    const bool transitioningBefore = IsGraphTransitioning();
    const bool generationIsCurrent = m_asyncPipeline.IsCurrentGeneration(generation);
    const bool transitioningAfter = IsGraphTransitioning();
    return DirectMotionReceiveCommitPolicy::CanCommit(
        generationIsCurrent, transitioningBefore || transitioningAfter);
}

bool CDirectMotion::EnsureEngine(std::string& error,
    AsyncPipelineState::Generation expectedReceiveGeneration) {
    if (!m_settings.enabled) return true;
    if (m_engine && m_engine->IsInitialized()) {
        if (expectedReceiveGeneration == 0) return true;
        if (m_engineGeneration.load(std::memory_order_acquire) != expectedReceiveGeneration) {
            error = "The active AviSynth/MVTools graph does not belong to this Receive sample's generation.";
            return false;
        }
        return IsReceiveCommitCurrent(expectedReceiveGeneration);
    }
    if (expectedReceiveGeneration != 0 &&
        !IsReceiveCommitCurrent(expectedReceiveGeneration)) {
        error.clear();
        return false;
    }
    if (m_width <= 0 || m_height <= 0) {
        error = "Input media type has not provided valid NV12/NV21 frame dimensions.";
        return false;
    }
    // Do not let a partially initialized graph inherit an older engine identity.
    // The worker treats generation zero as unavailable until initialization commits.
    m_engineGeneration.store(0, std::memory_order_release);
    m_engine = std::make_unique<ActiveFrucEngine>();
    m_sourceRate = SourceFrameRate();
    const FrucRational resolvedTargetRate = TargetFrameRate();
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_targetRate = resolvedTargetRate;
    }
    const int requestedGrid = m_settings.gridOverrideEnabled ? m_settings.grid : 0;
    const bool resumeWarmStart = m_resumeWarmStartPending.load(std::memory_order_acquire);
    DirectMotionLog("INFO", "Engine", "Initializing AviSynth+ / MVTools2 bridge for " +
        std::to_string(m_width) + "x" + std::to_string(m_height) +
        ", sourceFPS=" + std::to_string(static_cast<double>(m_sourceRate.num) / m_sourceRate.den) +
        ", targetFPS=" + std::to_string(static_cast<double>(resolvedTargetRate.num) / resolvedTargetRate.den) +
        ". Runtime DLLs must be loaded exclusively from the filter directory.");
    if (!m_engine->Initialize(m_width, m_height, m_sourceRate, resolvedTargetRate,
                              m_settings.performance, requestedGrid,
                              m_settings.cpuProfile, m_settings.customCpuWorkers,
                              m_settings.quickRecalcMode, m_settings.prefetchFrames,
                              m_settings.mvtools, resumeWarmStart,
                              m_avisynthRuntimeSession, m_sourceEvaluationGate, error)) {
        m_engine.reset();
        m_engineGeneration.store(0, std::memory_order_release);
        DirectMotionLog("ERROR", "Engine", "Initialization failed: " + error);
        return false;
    }
    if (expectedReceiveGeneration != 0 &&
        !IsReceiveCommitCurrent(expectedReceiveGeneration)) {
        // The graph was built while a competing lifecycle transition retired this
        // sample. No source frame has been pushed and the worker still sees engine
        // generation zero, so this newly built graph can be safely discarded here.
        DirectMotionLog("INFO", "Receive/GenerationCommit",
            "Discarded a newly initialized but uncommitted AviSynth/MVTools graph; "
            "the Receive sample generation was retired during EnsureEngine. sampleGeneration=" +
            std::to_string(expectedReceiveGeneration) + ", currentGeneration=" +
            std::to_string(m_asyncPipeline.CurrentGeneration()) + ".");
        m_engine->Reset();
        m_engine.reset();
        m_engineGeneration.store(0, std::memory_order_release);
        error.clear();
        return false;
    }
    m_resumeWarmStartPending.store(false, std::memory_order_release);
    const AsyncPipelineState::Generation engineGeneration = expectedReceiveGeneration != 0
        ? expectedReceiveGeneration : m_asyncPipeline.CurrentGeneration();
    m_engineGeneration.store(engineGeneration, std::memory_order_release);
    if (!error.empty()) {
        DirectMotionLog("WARN", "Engine", error);
    }

    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_outputTimeBase = m_engine->OutputTimeBase();
    }
    DirectMotionLog("INFO", "Engine", "AviSynth+ / MVTools2 graph initialized successfully. "
        "Receive publishes decoded frames into the shared live source cache; the dedicated processing worker owns "
        "MBlockFps GetFrame calls, and the dedicated delivery worker owns allocator acquisition and downstream Deliver. "
        "AviSynth+ Prefetch may evaluate frames on multiple internal workers. Output time base=" +
        std::to_string(m_engine->OutputTimeBase().num) + "/" +
        std::to_string(m_engine->OutputTimeBase().den));
    RefreshTrayRuntimeInfo();
    return true;
}


bool CDirectMotion::EnsureEngineForBootstrapGeneration(
    AsyncPipelineState::Generation expectedGeneration, std::string& error) {
    if (expectedGeneration == 0) {
        error = "The source-bootstrap worker cannot prepare an engine without a pipeline generation.";
        return false;
    }

    DirectMotionSettings settingsSnapshot;
    int width = 0;
    int height = 0;
    FrucRational sourceRate{};
    FrucRational targetRate{};
    bool resumeWarmStart = false;
    std::shared_ptr<AviSynthEvaluationGate> evaluationGate;
    {
        // Snapshot only the graph inputs while serialized with Receive and graph
        // lifecycle transitions. The expensive AviSynth/MVTools Initialize call
        // deliberately happens after this lock is released.
        CAutoLock receiveLock(&m_csReceive);
        if (!IsReceiveCommitCurrent(expectedGeneration) ||
            !m_sourceBootstrapQueue.IsActiveForGeneration(expectedGeneration)) {
            error.clear();
            return false;
        }
        if (m_engine && m_engine->IsInitialized()) {
            if (m_engineGeneration.load(std::memory_order_acquire) == expectedGeneration)
                return true;
            error = "A different AviSynth/MVTools engine generation is already installed.";
            return false;
        }
        if (!m_settings.enabled) return true;

        settingsSnapshot = m_settings;
        width = m_width;
        height = m_height;
        sourceRate = SourceFrameRate();
        targetRate = TargetFrameRate();
        resumeWarmStart = m_resumeWarmStartPending.load(std::memory_order_acquire);
        evaluationGate = m_sourceEvaluationGate;
    }

    if (width <= 0 || height <= 0 || !evaluationGate) {
        error = "The source-bootstrap graph snapshot has invalid dimensions or no AviSynth evaluation gate.";
        return false;
    }

    const auto buildStarted = std::chrono::steady_clock::now();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Backend,
        "SourceBootstrap / build replacement AviSynth+ MVTools graph outside Receive lock",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    struct BootstrapWorkPhaseReset final {
        ~BootstrapWorkPhaseReset() noexcept {
            DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
        }
    } workPhaseReset;

    auto candidate = std::make_unique<ActiveFrucEngine>();
    const int requestedGrid = settingsSnapshot.gridOverrideEnabled ? settingsSnapshot.grid : 0;
    if (!candidate->Initialize(width, height, sourceRate, targetRate,
            settingsSnapshot.performance, requestedGrid,
            settingsSnapshot.cpuProfile, settingsSnapshot.customCpuWorkers,
            settingsSnapshot.quickRecalcMode, settingsSnapshot.prefetchFrames,
            settingsSnapshot.mvtools, resumeWarmStart,
            m_avisynthRuntimeSession, evaluationGate, error)) {
        return false;
    }
    const double candidateBuildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - buildStarted).count();
    const FrucRational committedOutputTimeBase = candidate->OutputTimeBase();

    const auto commitLockStarted = std::chrono::steady_clock::now();
    double commitLockWaitMs = 0.0;
    bool committed = false;
    {
        CAutoLock receiveLock(&m_csReceive);
        commitLockWaitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - commitLockStarted).count();
        const bool commitAllowed = DirectMotionEngineBootstrapCommit::CanCommitCandidate(
            m_asyncPipeline.IsCurrentGeneration(expectedGeneration),
            m_sourceBootstrapQueue.IsActiveForGeneration(expectedGeneration),
            IsReceiveCommitCurrent(expectedGeneration),
            !m_seekResetState.deferredResetPending,
            !m_engine,
            candidate->IsInitialized());
        if (commitAllowed) {
            m_engine = std::move(candidate);
            m_sourceRate = sourceRate;
            {
                std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
                m_targetRate = targetRate;
                m_outputTimeBase = committedOutputTimeBase;
            }
            m_resumeWarmStartPending.store(false, std::memory_order_release);
            m_engineGeneration.store(expectedGeneration, std::memory_order_release);
            RefreshTrayRuntimeInfo();
            committed = true;
        }
    }

    if (!committed) {
        // A second seek/flush may have retired this candidate while Initialize was
        // running. Its destructor executes outside m_csReceive; no stale graph is
        // published and no newer generation is overwritten.
        error.clear();
        return false;
    }

    LogInfo("SourceBootstrap/EngineBuild", "Committed the replacement AviSynth/MVTools graph after preparing it outside m_csReceive. "
        "generation=" + std::to_string(expectedGeneration) +
        ", dimensions=" + std::to_string(width) + "x" + std::to_string(height) +
        ", candidateBuildMs=" + std::to_string(candidateBuildMs) +
        ", commitLockWaitMs=" + std::to_string(commitLockWaitMs) +
        ", outputTimeBase=" + std::to_string(committedOutputTimeBase.num) + "/" +
            std::to_string(committedOutputTimeBase.den) +
        ", stagedFramesWaiting=" + std::to_string(m_sourceBootstrapQueue.QueuedFrames()) +
        ", stagingBytesOutstanding=" + std::to_string(m_sourceBootstrapQueue.BytesOutstanding()) +
        "; upstream Receive remained free to enqueue decoded source/fallback frames during graph construction.");
    DirectMotionLog("INFO", "Engine", "AviSynth+ / MVTools2 graph initialized successfully. "
        "The post-seek candidate was committed after out-of-lock graph construction; ProcessingWorker owns MBlockFps GetFrame and DeliveryWorker owns downstream delivery. "
        "Output time base=" + std::to_string(committedOutputTimeBase.num) + "/" +
        std::to_string(committedOutputTimeBase.den));
    return true;
}

HRESULT CDirectMotion::DeliverDownstreamTracked(IMediaSample* sample, bool allowPausedControlSample) noexcept {
    if (!sample) return E_POINTER;
    if (!m_pOutput || !m_pOutput->IsConnected()) return VFW_E_NOT_CONNECTED;
    try {
        std::lock_guard<std::recursive_mutex> deliveryLock(m_deliveryMutex);
        if (IsGraphTransitioning()) return VFW_E_WRONG_STATE;
        // Synchronous paused-frame/pass-through control samples must preserve DirectShow
        // preroll/frame-step semantics. Generated output opts out and rechecks the pause
        // transition immediately before entering downstream Receive.
        if (!allowPausedControlSample && IsPauseTransitionPending()) return E_ABORT;
        return m_pOutput->Deliver(sample);
    } catch (...) {
        return E_UNEXPECTED;
    }
}

void CDirectMotion::RecordOutputProgress(const ActiveFrucEngine::OutputFrame& frame,
    FrucRational outputTimeBase) noexcept {
    try {
        if (frame.pts < 0 || m_sourceRate.num <= 0 || m_sourceRate.den <= 0 ||
            outputTimeBase.num <= 0 || outputTimeBase.den <= 0) return;
        const FrucRational sourceFrameTimeBase = FrucNormalizeRational(m_sourceRate.den, m_sourceRate.num);
        const int64_t representedSourceIndex = FrucRescaleFloor(
            frame.pts, outputTimeBase, sourceFrameTimeBase);
        int64_t previous = m_lastOutputSourceIndex.load(std::memory_order_acquire);
        while (representedSourceIndex > previous &&
            !m_lastOutputSourceIndex.compare_exchange_weak(previous, representedSourceIndex,
                std::memory_order_release, std::memory_order_acquire)) {}
    } catch (...) {
        // Progress bookkeeping is best-effort only after successful delivery.
    }
}

bool CDirectMotion::StartDeliveryWorker(std::string& error) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_deliveryWorkerLifecycleMutex);
        if (m_deliveryWorkerThread.joinable()) {
            error = "The output-delivery worker is already joinable.";
            return false;
        }

        const AsyncPipelineState::Generation generation = m_asyncPipeline.BeginGeneration();
        if (m_engine && m_engine->IsInitialized())
            m_engineGeneration.store(generation, std::memory_order_release);
        m_deliveryWorkerStop.store(false, std::memory_order_release);
        m_asyncPipeline.SetWorkerState(generation, AsyncWorkerKind::Delivery, AsyncWorkerState::Starting);
        try {
            m_deliveryWorkerThread = std::thread(&CDirectMotion::DeliveryWorkerMain, this);
        } catch (...) {
            m_asyncPipeline.CancelGeneration(generation, "delivery worker thread creation failed");
            throw;
        }
        LogInfo("AsyncDelivery/Start", "Started the dedicated downstream delivery worker. generation=" +
            std::to_string(generation) + ", queueCapacity=" +
            std::to_string(m_asyncPipeline.Snapshot().output.capacity) +
            "; the processing worker publishes output packets, and Receive does not wait for delivery.");
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not start the output-delivery worker: ") + ex.what();
    } catch (...) {
        error = "Could not start the output-delivery worker due to an unknown thread/startup error.";
    }
    return false;
}

void CDirectMotion::StopDeliveryWorker(const char* reason) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_deliveryWorkerLifecycleMutex);
        m_deliveryWorkerStop.store(true, std::memory_order_release);
        NotifyDeliveryWorker();
        const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
        if (generation != 0) {
            m_asyncPipeline.CancelGeneration(generation,
                reason ? reason : "delivery worker stopping");
        }
        if (m_deliveryWorkerThread.joinable()) {
            if (m_deliveryWorkerThread.get_id() == std::this_thread::get_id()) {
                DirectMotionLog("ERROR", "AsyncDelivery/Stop",
                    "Refusing to self-join the delivery worker; lifecycle ownership is invalid.");
                return;
            }
            m_deliveryWorkerThread.join();
        }
        LogInfo("AsyncDelivery/Stop", "Dedicated downstream delivery worker joined. generation=" +
            std::to_string(generation) + ", reason=" + (reason ? reason : "unspecified"));
    } catch (...) {
        DirectMotionLog("ERROR", "AsyncDelivery/Stop",
            "An exception occurred while cancelling/joining the output-delivery worker.");
    }
}

bool CDirectMotion::StartProcessingWorker(std::string& error) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_processingWorkerLifecycleMutex);
        if (m_processingWorkerThread.joinable()) {
            error = "The AviSynth processing worker is already joinable.";
            return false;
        }
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            m_processingWorkerStop = false;
            m_processingWorkerSuspend = false;
            m_processingWorkPending = false;
            m_processingWorkerActive = false;
            m_processingEosRequested = false;
            m_processingEosCompleted = false;
            m_processingEosResult = S_OK;
        }
        const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
        if (generation == 0) {
            error = "Cannot start the AviSynth processing worker without an active pipeline generation.";
            return false;
        }
        m_asyncPipeline.SetWorkerState(generation, AsyncWorkerKind::Processing, AsyncWorkerState::Starting);
        m_processingWorkerThread = std::thread(&CDirectMotion::ProcessingWorkerMain, this);
        m_processingWorkerStarted.store(true, std::memory_order_release);
        LogInfo("AsyncProcessing/Start", "Started the dedicated AviSynth/MVTools processing worker. generation=" +
            std::to_string(generation) + "; Receive now publishes source frames and never calls MBlockFps GetFrame.");
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not start the AviSynth processing worker: ") + ex.what();
    } catch (...) {
        error = "Could not start the AviSynth processing worker due to an unknown thread/startup error.";
    }
    return false;
}

void CDirectMotion::StopProcessingWorker(const char* reason) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_processingWorkerLifecycleMutex);
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            m_processingWorkerStop = true;
            m_processingWorkerSuspend = false;
            m_processingWorkPending = false;
            if (m_processingEosRequested && !m_processingEosCompleted) {
                m_processingEosResult = E_ABORT;
                m_processingEosCompleted = true;
            }
        }
        InterruptPendingSourceRequests(reason ? reason : "ProcessingWorker/Stop");
        m_processingWorkerChanged.notify_all();
        if (m_processingWorkerThread.joinable()) {
            if (m_processingWorkerThread.get_id() == std::this_thread::get_id()) {
                DirectMotionLog("ERROR", "AsyncProcessing/Stop",
                    "Refusing to self-join the processing worker; lifecycle ownership is invalid.");
                return;
            }
            m_processingWorkerThread.join();
        }
        m_processingWorkerStarted.store(false, std::memory_order_release);
        const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
        if (generation != 0) m_asyncPipeline.SetWorkerState(
            generation, AsyncWorkerKind::Processing, AsyncWorkerState::Exited);
        LogInfo("AsyncProcessing/Stop", "Dedicated AviSynth processing worker joined. generation=" +
            std::to_string(generation) + ", reason=" + (reason ? reason : "unspecified"));
    } catch (...) {
        DirectMotionLog("ERROR", "AsyncProcessing/Stop",
            "An exception occurred while cancelling/joining the AviSynth processing worker.");
    }
}

void CDirectMotion::NotifyProcessingWorker() noexcept {
    try {
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            if (m_processingWorkerStop || m_processingWorkerSuspend) return;
            m_processingWorkPending = true;
        }
        m_processingWorkerChanged.notify_one();
    } catch (...) {
        DirectMotionLog("ERROR", "AsyncProcessing/Notify", "Could not signal the processing worker.");
    }
}

size_t CDirectMotion::NormalizedSourceFrameBytes() const noexcept {
    if (m_width <= 0 || m_height <= 0 || (m_width & 1) != 0 || (m_height & 1) != 0)
        return 0;
    const size_t width = static_cast<size_t>(m_width);
    const size_t height = static_cast<size_t>(m_height);
    if (height > (std::numeric_limits<size_t>::max)() / width) return 0;
    const size_t pixels = width * height;
    if (pixels > (std::numeric_limits<size_t>::max)() / 3u) return 0;
    return (pixels * 3u) / 2u;
}

bool CDirectMotion::StartSourceBootstrapWorker(std::string& error) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_sourceBootstrapWorkerLifecycleMutex);
        if (m_sourceBootstrapWorkerThread.joinable()) {
            error = "The source bootstrap worker is already joinable.";
            return false;
        }
        m_sourceBootstrapWorkerStop.store(false, std::memory_order_release);
        m_sourceBootstrapWorkerThread = std::thread(&CDirectMotion::SourceBootstrapWorkerMain, this);
        m_sourceBootstrapWorkerStarted.store(true, std::memory_order_release);
        LogInfo("SourceBootstrap/Start", "Started the dedicated source-ingestion/bootstrap worker. "
            "Post-seek Receive now stages normalized NV12 and returns while the replacement AviSynth graph is rebuilt; "
            "staging byte budget=" + std::to_string(m_sourceBootstrapQueue.ByteBudget()) + ".");
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not start the source bootstrap worker: ") + ex.what();
    } catch (...) {
        error = "Could not start the source bootstrap worker due to an unknown thread/startup error.";
    }
    return false;
}

void CDirectMotion::StopSourceBootstrapWorker(const char* reason) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_sourceBootstrapWorkerLifecycleMutex);
        m_sourceBootstrapWorkerStop.store(true, std::memory_order_release);
        m_sourceBootstrapQueue.Cancel();
        InterruptPendingSourceRequests(reason ? reason : "SourceBootstrap/Stop");
        m_sourceBootstrapQueue.NotifyWaiters();
        if (m_sourceBootstrapWorkerThread.joinable()) {
            if (m_sourceBootstrapWorkerThread.get_id() == std::this_thread::get_id()) {
                DirectMotionLog("ERROR", "SourceBootstrap/Stop",
                    "Refusing to self-join the source bootstrap worker; lifecycle ownership is invalid.");
                return;
            }
            m_sourceBootstrapWorkerThread.join();
        }
        m_sourceBootstrapWorkerStarted.store(false, std::memory_order_release);
        LogInfo("SourceBootstrap/Stop", "Source-ingestion/bootstrap worker joined. reason=" +
            std::string(reason ? reason : "unspecified"));
    } catch (...) {
        DirectMotionLog("ERROR", "SourceBootstrap/Stop",
            "An exception occurred while cancelling/joining the source bootstrap worker.");
    }
}

void CDirectMotion::NotifySourceBootstrapWorker() noexcept {
    m_sourceBootstrapQueue.NotifyWaiters();
}

void CDirectMotion::SourceBootstrapWorkerMain() noexcept {
    try {
        for (;;) {
            DirectMotionSourceBootstrap::Frame staged;
            if (!m_sourceBootstrapQueue.WaitPop(staged, [this]() noexcept {
                    return m_sourceBootstrapWorkerStop.load(std::memory_order_acquire);
                })) {
                if (m_sourceBootstrapWorkerStop.load(std::memory_order_acquire)) break;
                continue;
            }

            const size_t stagedBytes = staged.chargedBytes;
            struct StagedFrameLease final {
                DirectMotionSourceBootstrap::Queue& queue;
                DirectMotionSourceBootstrap::Frame& frame;
                bool completed = false;
                ~StagedFrameLease() noexcept { Complete(); }
                void Complete() noexcept {
                    if (completed) return;
                    queue.CompleteFrame(frame);
                    completed = true;
                }
            } stagedLease{ m_sourceBootstrapQueue, staged };
            const auto releaseStagedBytes = [&stagedLease]() noexcept {
                stagedLease.Complete();
            };
            if (!m_sourceBootstrapQueue.IsCurrent(staged) ||
                !IsReceiveCommitCurrent(staged.generation)) {
                releaseStagedBytes();
                continue;
            }

            // This worker, not Receive, pays the old-engine quiescence/destruction
            // cost. The first source fallback is already in the independent delivery
            // FIFO, and upstream Receive has returned before this thread takes the
            // DirectShow receive lock for graph work.
            if (!CompleteDeferredSeekEngineReset(staged.generation)) {
                releaseStagedBytes();
                if (m_sourceBootstrapQueue.IsCurrent(staged)) {
                    m_sourceBootstrapQueue.Cancel();
                    FailRuntime("SourceBootstrap/DeferredReset",
                        "The source bootstrap worker could not complete the deferred seek reset safely.", E_ABORT);
                }
                continue;
            }

            std::string error;
            bool engineReady = false;
            if (m_sourceBootstrapQueue.IsCurrent(staged) &&
                IsReceiveCommitCurrent(staged.generation)) {
                // Do not hold m_csReceive across graph construction: that would
                // serialize every later decoder Receive behind the 4K MVTools build,
                // defeating source staging and draining the visible fallback stream.
                engineReady = EnsureEngineForBootstrapGeneration(staged.generation, error);
            }
            if (!engineReady) {
                releaseStagedBytes();
                if (!m_sourceBootstrapQueue.IsCurrent(staged) ||
                    !IsReceiveCommitCurrent(staged.generation)) continue;
                m_sourceBootstrapQueue.Cancel();
                FailRuntime("SourceBootstrap/EnsureEngine",
                    error.empty()
                        ? "The replacement AviSynth/MVTools graph was not committed for the staged source generation."
                        : error);
                continue;
            }

            DirectMotionSourceCache::PushResult pushResult = DirectMotionSourceCache::PushResult::Interrupted;
            const auto ingestStarted = std::chrono::steady_clock::now();
            for (;;) {
                // Pause is not a generation retirement. Keep ownership of the staged
                // frame (and its byte-budget charge) until Run reopens source admission;
                // dropping it here would leave a hole in the reserved source indices.
                while (IsPauseTransitionPending() &&
                    !m_sourceBootstrapWorkerStop.load(std::memory_order_acquire) &&
                    m_sourceBootstrapQueue.IsCurrent(staged) &&
                    m_asyncPipeline.IsCurrentGeneration(staged.generation)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (m_sourceBootstrapWorkerStop.load(std::memory_order_acquire) ||
                    !m_sourceBootstrapQueue.IsCurrent(staged) ||
                    !IsReceiveCommitCurrent(staged.generation)) break;
                {
                    CAutoLock receiveLock(&m_csReceive);
                    if (m_sourceBootstrapQueue.IsCurrent(staged) &&
                        IsReceiveCommitCurrent(staged.generation) && m_engine &&
                        m_engineGeneration.load(std::memory_order_acquire) == staged.generation) {
                        pushResult = m_engine->PushSourceNv12(
                            staged.nv12 ? staged.nv12->data() : nullptr,
                            m_width, staged.sourceFrameIndex, error,
                            this, [](void* context) noexcept {
                                static_cast<CDirectMotion*>(context)->NotifyProcessingWorker();
                            });
                    }
                }
                if (pushResult == DirectMotionSourceCache::PushResult::Interrupted &&
                    IsPauseTransitionPending() && m_sourceBootstrapQueue.IsCurrent(staged) &&
                    m_asyncPipeline.IsCurrentGeneration(staged.generation)) {
                    error.clear();
                    continue;
                }
                break;
            }
            const double ingestElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ingestStarted).count();
            releaseStagedBytes();

            if (pushResult == DirectMotionSourceCache::PushResult::Published) {
                m_diagInputPublishedFrames.fetch_add(1, std::memory_order_acq_rel);
                m_lastPushMs.store(ingestElapsedMs, std::memory_order_release);
                const uint64_t ingestSamples = m_pushTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
                double ingestAverage = m_avgPushMs.load(std::memory_order_acquire);
                if (ingestAverage < 0.0) ingestAverage = ingestElapsedMs;
                else ingestAverage += (ingestElapsedMs - ingestAverage) /
                    static_cast<double>(ingestSamples > 60 ? 60 : ingestSamples);
                m_avgPushMs.store(ingestAverage, std::memory_order_release);
                LogInfo("SourceBootstrap/Published", "Published staged source frame into the replacement InputClip. "
                    "generation=" + std::to_string(staged.generation) +
                    ", sourceFrame=" + std::to_string(staged.sourceFrameIndex) +
                    ", bytes=" + std::to_string(stagedBytes) +
                    ", ingestMs=" + std::to_string(ingestElapsedMs) +
                    ", stagingBytesOutstanding=" + std::to_string(m_sourceBootstrapQueue.BytesOutstanding()) + ".");
                NotifyProcessingWorker();
            } else if (pushResult == DirectMotionSourceCache::PushResult::Interrupted &&
                DirectMotionSourceCache::IsExpectedLifecycleInterruption(
                    IsGraphTransitioning(), IsPauseTransitionPending(),
                    m_asyncPipeline.IsCurrentGeneration(staged.generation))) {
                LogInfo("SourceBootstrap/Interrupted", "Discarded staged source frame during an expected lifecycle/generation transition. "
                    "generation=" + std::to_string(staged.generation) +
                    ", sourceFrame=" + std::to_string(staged.sourceFrameIndex) + ".");
            } else if (m_sourceBootstrapQueue.IsCurrent(staged) &&
                m_asyncPipeline.IsCurrentGeneration(staged.generation)) {
                m_sourceBootstrapQueue.Cancel();
                FailRuntime("SourceBootstrap/PushSourceNv12",
                    error.empty()
                        ? "Staged source-frame publication failed without an AviSynth source-cache diagnostic."
                        : error,
                    pushResult == DirectMotionSourceCache::PushResult::Interrupted ? E_ABORT : E_FAIL);
            }

            if (pushResult == DirectMotionSourceCache::PushResult::Published &&
                m_sourceBootstrapQueue.TryCompleteGeneration(staged.generation)) {
                LogInfo("SourceBootstrap/Drained", "The replacement engine has consumed every staged source frame. "
                    "Receive may return to direct source-cache publication. generation=" +
                    std::to_string(staged.generation) + ".");
            }
        }
    } catch (const std::exception& ex) {
        m_sourceBootstrapQueue.Cancel();
        if (!m_sourceBootstrapWorkerStop.load(std::memory_order_acquire))
            FailRuntime("SourceBootstrap/Worker",
                std::string("Unhandled source bootstrap worker exception: ") + ex.what(), E_UNEXPECTED);
    } catch (...) {
        m_sourceBootstrapQueue.Cancel();
        if (!m_sourceBootstrapWorkerStop.load(std::memory_order_acquire))
            FailRuntime("SourceBootstrap/Worker",
                "Unhandled unknown source bootstrap worker exception.", E_UNEXPECTED);
    }
}

// Called only while m_processingWorkerLifecycleMutex is held by ResetEngine or deferred-seek completion.
bool CDirectMotion::SuspendProcessingWorkerForEngineReset() noexcept {
    try {
        bool waitForActivePass = false;
        bool initialWorkerStarted = false;
        bool initialStopRequested = false;
        const auto waitStarted = std::chrono::steady_clock::now();
        const auto generation = m_engineGeneration.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            const bool workerStarted = m_processingWorkerStarted.load(std::memory_order_acquire);
            initialWorkerStarted = workerStarted;
            initialStopRequested = m_processingWorkerStop;
            if (workerStarted || m_processingWorkerActive) {
                // Latch suspension even when Stop has already been requested. Stop is
                // not evidence that an in-flight GetFrame has returned; the active-pass
                // bit remains the authority for engine lifetime.
                m_processingWorkerSuspend = true;
                m_processingWorkPending = false;
                m_diagProcessingPassCapped.store(false, std::memory_order_release);
                if (m_processingEosRequested && !m_processingEosCompleted) {
                    m_processingEosResult = E_ABORT;
                    m_processingEosCompleted = true;
                    m_processingEosRequested = false;
                }
            }
            const DirectMotionProcessingQuiescence::Snapshot snapshot{
                workerStarted,
                m_processingWorkerStop,
                m_processingWorkerSuspend,
                m_processingWorkerActive
            };
            waitForActivePass = !DirectMotionProcessingQuiescence::IsQuiescentForEngineReset(snapshot);
        }

        // Interrupt source-cache waits before waiting for the worker to become idle.
        // No Receive/engine mutex is held by the processor while it sleeps in GetFrame.
        InterruptPendingSourceRequests("ProcessingWorker/EngineReset");
        m_processingWorkerChanged.notify_all();
        if (!waitForActivePass) return true;

        DirectMotionLog("INFO", "AsyncProcessing/ResetWait",
            "WAIT_BEGIN engine-reset quiescence barrier; generation=" + std::to_string(generation) +
            ", workerStarted=" + std::to_string(initialWorkerStarted ? 1 : 0) +
            ", stopRequested=" + std::to_string(initialStopRequested ? 1 : 0) +
            ", sourceWaiters=" + std::to_string(DirectMotionTrayGetSourceWaiterCount()) +
            ", threadId=" + std::to_string(GetCurrentThreadId()) +
            "; engine memory remains retained until the active ProcessingWorker pass exits.");

        std::unique_lock<std::mutex> stateLock(m_processingWorkerMutex);
        auto isQuiescent = [this]() noexcept {
            return DirectMotionProcessingQuiescence::IsQuiescentForEngineReset({
                m_processingWorkerStarted.load(std::memory_order_acquire),
                m_processingWorkerStop,
                m_processingWorkerSuspend,
                m_processingWorkerActive
            });
        };
        auto nextReport = waitStarted + std::chrono::seconds(5);
        while (!isQuiescent()) {
            if (m_processingWorkerChanged.wait_until(stateLock, nextReport, isQuiescent)) break;

            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - waitStarted).count();
            const bool workerStarted = m_processingWorkerStarted.load(std::memory_order_acquire);
            const bool stopRequested = m_processingWorkerStop;
            const bool suspendRequested = m_processingWorkerSuspend;
            const bool activePass = m_processingWorkerActive;
            stateLock.unlock();

            // This snapshot is read after releasing the worker-state mutex. It contains
            // only per-thread operation records; it never dereferences the engine.
            const std::string activeContext = DirectMotionTrayGetDiagnosticContext();
            DirectMotionLog("WARN", "AsyncProcessing/ResetWait",
                "WAIT_PROGRESS engine-reset quiescence still pending; elapsedMs=" +
                std::to_string(elapsedMs) + ", generation=" + std::to_string(generation) +
                ", workerStarted=" + std::to_string(workerStarted ? 1 : 0) +
                ", stopRequested=" + std::to_string(stopRequested ? 1 : 0) +
                ", suspendRequested=" + std::to_string(suspendRequested ? 1 : 0) +
                ", activePass=" + std::to_string(activePass ? 1 : 0) +
                ", sourceWaiters=" + std::to_string(DirectMotionTrayGetSourceWaiterCount()) +
                ", activeWork=" + (activeContext.empty() ? std::string("<none tagged>") : activeContext) +
                "; reset remains blocked and no AviSynth/MVTools object has been released.");

            stateLock.lock();
            nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        const bool workerStopped = m_processingWorkerStop;
        const bool workerStarted = m_processingWorkerStarted.load(std::memory_order_acquire);
        stateLock.unlock();

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStarted).count();
        DirectMotionLog("INFO", "AsyncProcessing/ResetWait",
            "WAIT_END engine-reset quiescence acquired; elapsedMs=" + std::to_string(elapsedMs) +
            ", generation=" + std::to_string(generation) +
            ", workerStarted=" + std::to_string(workerStarted ? 1 : 0) +
            ", stopRequested=" + std::to_string(workerStopped ? 1 : 0) +
            ", activePass=0; engine reset may now safely proceed.");
        return true;
    } catch (...) {
        DirectMotionLog("ERROR", "AsyncProcessing/Suspend",
            "Could not prove ProcessingWorker quiescence during engine reset. The active engine is retained; "
            "the caller must fail the current runtime transition rather than destroy objects still in use.");
        return false;
    }
}
void CDirectMotion::ResumeProcessingWorkerAfterEngineReset() noexcept {
    try {
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            if (m_processingWorkerStop) return;
            m_processingWorkerSuspend = false;
        }
        m_processingWorkerChanged.notify_all();
    } catch (...) {
        DirectMotionLog("ERROR", "AsyncProcessing/Resume", "Could not resume the processing worker after graph reset.");
    }
}

HRESULT CDirectMotion::ProcessAvailableFrames(
    AsyncPipelineState::Generation generation,
    bool terminalDrain,
    bool& continuationRequired) {
    continuationRequired = false;

    const auto engineGeneration = m_engineGeneration.load(std::memory_order_acquire);
    if (!DirectMotionProcessingGeneration::MatchesEngineGeneration(generation, engineGeneration)) {
        LogInfo("AsyncProcessing/StaleEngineGeneration",
            "Rejected a processing pass before dereferencing the engine because its FIFO generation does not match the active AviSynth graph. processingGeneration=" +
            std::to_string(generation) + ", engineGeneration=" +
            std::to_string(engineGeneration) + ", terminalDrain=" +
            std::to_string(terminalDrain ? 1 : 0) + ", threadId=" +
            std::to_string(GetCurrentThreadId()) +
            "; the old engine remains retained until ResetEngine establishes worker quiescence.");
        return S_OK;
    }

    auto processingWasInterrupted = [this, generation, terminalDrain]() noexcept {
        if (!DirectMotionProcessingGeneration::MatchesEngineGeneration(
                generation, m_engineGeneration.load(std::memory_order_acquire)) ||
            IsGraphTransitioning() || (!terminalDrain && IsPauseTransitionPending()) ||
            DirectMotionPauseRetention::ShouldSuspendProcessing(
                m_pauseHoldActive.load(std::memory_order_acquire), terminalDrain) ||
            m_runtimeFailed.load(std::memory_order_acquire) ||
            !m_asyncPipeline.IsCurrentGeneration(generation)) return true;
        try {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            return m_processingWorkerStop || m_processingWorkerSuspend;
        } catch (...) {
            return true;
        }
    };


    if (processingWasInterrupted()) return S_OK;
    if (!m_settings.enabled || !m_engine || !m_engine->IsInitialized()) return S_OK;

    if (terminalDrain && m_engine->IsSourceFinalized() &&
        m_engine->ExpectedOutputCount() == 0) {
        return QueueDeliveryDrainBarrier(generation);
    }

    if (!m_bootstrapQueueState.HasBeenEnqueued()) {
        if (!terminalDrain &&
            m_engine->InputFrameCount() < m_engine->BootstrapInputFramesRequired()) return S_OK;

        // Snapshot the current fallback PTS frontier immediately. No configurable
        // residence timer delays this decision: the source fallback remains active
        // only until the first non-covered MVTools output is genuinely producible.
        // Once sealed, SourceBootstrapWorker may finish only the lookahead needed
        // for that exact frontier; it does not need to fill the whole cache/window.
        int64_t bootstrapStartOutputIndex = 0;
        bool selectedSeekFrontier = false;
        if (m_seekFallbackActivePublished.load(std::memory_order_acquire)) {
            bool invalidFrontier = false;
            {
                std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
                if (!DirectMotionSeekFallback::OwnsFallbackGeneration(
                        m_seekFallbackGeneration, generation)) return S_OK;
                if (m_seekFallbackActive && m_seekFallbackHasQueuedFrame) {
                    int64_t candidateOutputIndex = 0;
                    if (!DirectMotionSeekFrontier::TryFirstUncoveredOutputIndex(
                            true, m_seekFallbackLastQueuedPts, candidateOutputIndex)) {
                        invalidFrontier = true;
                    } else {
                        bootstrapStartOutputIndex = candidateOutputIndex;
                        // Snapshot and seal under the same mutex used by Receive's
                        // fallback admission. The PTS frontier cannot move afterward.
                        m_seekFallbackAcceptingSource = false;
                        selectedSeekFrontier = true;
                    }
                }
            }
            // Failure/recovery may acquire the fallback mutex; never invoke it while
            // holding that mutex or the error path can self-deadlock.
            if (invalidFrontier) {
                FailRuntime("AsyncProcessing/SeekFrontier",
                    "The source-fallback PTS watermark cannot be advanced without overflowing the first uncovered output index.", E_INVALIDARG);
                return E_FAIL;
            }
            if (selectedSeekFrontier) {
                DirectMotionTraySetSeekHandoffState(
                    DirectMotionSeekHandoffStatus::State::PtsWatermarkDrain, generation);
                std::string retentionError;
                if (!m_engine->PrimeSourceCacheForOutputIndex(
                        bootstrapStartOutputIndex, retentionError)) {
                    FailRuntime("AsyncProcessing/SeekFrontierRetention",
                        retentionError.empty()
                            ? "The source cache could not establish a bounded history anchor for the seek frontier."
                            : retentionError,
                        E_FAIL);
                    return E_FAIL;
                }
                if (terminalDrain && m_engine->IsSourceFinalized() &&
                    DirectMotionSeekFrontier::FallbackCoversFinalizedOutput(
                        bootstrapStartOutputIndex, m_engine->ExpectedOutputCount())) {
                    // EOS may arrive before the normal seek-frontier bootstrap has
                    // enough source lookahead. If fallback already covers the entire
                    // finalized output range, do not fall back to output index zero:
                    // the primed source cache has intentionally retired that prefix.
                    // The FIFO barrier preserves delivery order for all queued fallback.
                    LogInfo("AsyncProcessing/SeekFrontier",
                        "Finalized output is fully covered by the sealed source-fallback watermark; skipping MVTools bootstrap and appending only the terminal delivery barrier. generation=" +
                        std::to_string(generation) + ", firstUncoveredOutputIndex=" +
                        std::to_string(bootstrapStartOutputIndex) + ", expectedOutputCount=" +
                        std::to_string(m_engine->ExpectedOutputCount()));
                    return QueueDeliveryDrainBarrier(generation);
                }
                if (!m_engine->CanEmitOutputIndex(bootstrapStartOutputIndex)) {
                    // The fallback watermark is now stable and its already-queued
                    // packets continue draining. SourceBootstrapWorker will notify
                    // ProcessingWorker as the additional MVTools lookahead arrives.
                    return S_OK;
                }
            }
        }

        if (processingWasInterrupted()) return S_OK;
        ActiveFrucEngine::OutputFrame bootstrap;
        std::string error;
        const auto graphStarted = std::chrono::steady_clock::now();
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Backend,
            selectedSeekFrontier
                ? "ProcessingWorker / raw MBlockFps seek-frontier bootstrap GetFrame"
                : "ProcessingWorker / raw MBlockFps direct bootstrap GetFrame",
            m_inputFrameCount.load(std::memory_order_acquire),
            m_outputFrameCount.load(std::memory_order_acquire));
        const bool bootstrapReady = m_engine->EmitBootstrapFrame(
            bootstrapStartOutputIndex, bootstrap, error);
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
        const double graphElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - graphStarted).count();
        if (!bootstrapReady) {
            if (processingWasInterrupted()) return S_OK;
            FailRuntime("AsyncProcessing/Bootstrap", error.empty()
                ? "AviSynth could not produce the selected MBlockFps bootstrap frame." : error, E_FAIL);
            return E_FAIL;
        }
        m_diagOutputGeneratedFrames.fetch_add(1, std::memory_order_acq_rel);
        m_lastGraphMs.store(graphElapsedMs, std::memory_order_release);
        const uint64_t graphSample = m_graphTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
        double graphAverage = m_avgGraphMs.load(std::memory_order_acquire);
        if (graphAverage < 0.0) graphAverage = graphElapsedMs;
        else graphAverage += (graphElapsedMs - graphAverage) /
            static_cast<double>(graphSample > 60 ? 60 : graphSample);
        m_avgGraphMs.store(graphAverage, std::memory_order_release);

        // Only output zero on a cold segment is synthetic DirectShow preroll.
        // A seek-frontier frame is already beyond the source fallback watermark
        // and must be eligible to become the first visible interpolated packet.
        const bool bootstrapIsPreroll = bootstrapStartOutputIndex == 0;
        const HRESULT deliveryHr = QueueInterpolatedForDelivery(
            bootstrap, bootstrapIsPreroll, generation, false);
        if (FAILED(deliveryHr)) {
            if (processingWasInterrupted()) return S_OK;
            if ((IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) &&
                deliveryHr == VFW_E_NOT_COMMITTED) {
                m_processingNeedsReset.store(true, std::memory_order_release);
                LogInfo("AsyncProcessing/PauseRecovery",
                    "The allocator was decommitted during preroll/frontier delivery; the next Run will retire this graph before accepting new source input.");
                return S_OK;
            }
            FailRuntime("AsyncProcessing/BootstrapDeliver",
                "Could not deliver the DirectShow bootstrap frame; HRESULT=" +
                std::to_string(static_cast<unsigned long>(deliveryHr)), deliveryHr);
            return deliveryHr;
        }
        m_bootstrapQueueState.MarkEnqueued();
        LogInfo("AsyncProcessing/Bootstrap", std::string(selectedSeekFrontier
            ? "Seek-frontier frame was accepted by the output FIFO; the already-covered output prefix was not evaluated."
            : "Initial preroll frame was accepted by the output FIFO for the delivery worker; bootstrapQueued does not imply downstream delivery.") +
            " generation=" + std::to_string(generation) +
            ", outputIndex=" + std::to_string(bootstrapStartOutputIndex) +
            ", fallbackAdmissionSealed=" + std::to_string(selectedSeekFrontier ? 1 : 0) +
            ", runActive=" + std::to_string(m_runActive.load(std::memory_order_acquire) ? 1 : 0));
    }

    constexpr unsigned kMaxFramesPerPass = 128;
    unsigned emittedThisPass = 0;
    for (;;) {
        const uint64_t pauseEpochBeforeGetFrame = m_strictPauseEpoch.load(std::memory_order_acquire);
        if (processingWasInterrupted()) return S_OK;

        ActiveFrucEngine::OutputFrame frame;
        bool available = false;
        bool finished = false;
        std::string error;
        const auto graphStarted = std::chrono::steady_clock::now();
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Backend,
            terminalDrain ? "ProcessingWorker / terminal MBlockFps drain" :
                "ProcessingWorker / AviSynth Prefetch MBlockFps GetFrame",
            m_inputFrameCount.load(std::memory_order_acquire),
            m_outputFrameCount.load(std::memory_order_acquire));
        const bool ok = m_engine->EmitNextAvailable(frame, available, finished, error);
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
        const double graphElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - graphStarted).count();

        // Seek/flush/stop invalidates this result. Strict Pause is different:
        // AviSynth has already advanced m_nextOutputIndex, so dropping a completed
        // frame would leave a one-frame hole on resume. Preserve it in the same FIFO
        // generation; DeliveryWorkerMain holds it until Run.
        const bool pauseEpochChanged =
            m_strictPauseEpoch.load(std::memory_order_acquire) != pauseEpochBeforeGetFrame;
        if (processingWasInterrupted() || (!terminalDrain && pauseEpochChanged)) {
            const bool pauseInterruptedGetFrame =
                DirectMotionPauseRetention::WasGetFrameInterruptedByStrictPause(
                    pauseEpochChanged, m_pauseHoldActive.load(std::memory_order_acquire));
            if (ok && DirectMotionPauseRetention::ShouldQueueCompletedFrameAfterPause(
                    pauseInterruptedGetFrame, m_asyncPipeline.IsCurrentGeneration(generation),
                    IsGraphTransitioning(), m_runtimeFailed.load(std::memory_order_acquire),
                    available, finished)) {
                const HRESULT queuedDuringPause = QueueInterpolatedForDelivery(
                    frame, false, generation, false);
                if (SUCCEEDED(queuedDuringPause)) {
                    m_diagOutputGeneratedFrames.fetch_add(1, std::memory_order_acq_rel);
                    m_lastGraphMs.store(graphElapsedMs, std::memory_order_release);
                    LogInfo("AsyncProcessing/PauseRetention", "Preserved the output frame completed concurrently with strict Pause. generation=" +
                        std::to_string(generation) + ", pts=" + std::to_string(frame.pts) +
                        "; the delivery worker will submit it after Run.");
                } else if (pauseInterruptedGetFrame) {
                    LogInfo("AsyncProcessing/PauseRetention", "Discarded the frame completed across Pause because a later lifecycle transition invalidated its generation. HRESULT=" +
                        std::to_string(static_cast<unsigned long>(queuedDuringPause)));
                }
            }
            return S_OK;
        }

        if (!ok) {
            if (processingWasInterrupted()) return S_OK;
            FailRuntime(terminalDrain ? "AsyncProcessing/EndOfStream" : "AsyncProcessing/Graph",
                error.empty() ? "AviSynth could not produce the next safe MBlockFps output frame." : error,
                E_FAIL);
            return E_FAIL;
        }
        if (finished) return terminalDrain ? QueueDeliveryDrainBarrier(generation) : S_OK;
        if (!available) {
            if (terminalDrain) {
                FailRuntime("AsyncProcessing/EndOfStream",
                    "The finalized AviSynth source reported no output before its expected terminal frame count was reached.", E_UNEXPECTED);
                return E_UNEXPECTED;
            }
            return S_OK;
        }

        m_diagOutputGeneratedFrames.fetch_add(1, std::memory_order_acq_rel);
        m_lastGraphMs.store(graphElapsedMs, std::memory_order_release);
        const uint64_t graphSample = m_graphTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
        double graphAverage = m_avgGraphMs.load(std::memory_order_acquire);
        if (graphAverage < 0.0) graphAverage = graphElapsedMs;
        else graphAverage += (graphElapsedMs - graphAverage) /
            static_cast<double>(graphSample > 60 ? 60 : graphSample);
        m_avgGraphMs.store(graphAverage, std::memory_order_release);

        const HRESULT deliveryHr = QueueInterpolatedForDelivery(frame, false, generation, terminalDrain);
        if (FAILED(deliveryHr)) {
            if (processingWasInterrupted()) return S_OK;
            if ((IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) &&
                deliveryHr == VFW_E_NOT_COMMITTED) {
                m_processingNeedsReset.store(true, std::memory_order_release);
                LogInfo("AsyncProcessing/PauseRecovery",
                    "The allocator was decommitted during paused output delivery; the next Run will retire this graph before accepting new source input.");
                return terminalDrain ? VFW_E_NOT_COMMITTED : S_OK;
            }
            FailRuntime("AsyncProcessing/Delivery",
                "Could not deliver interpolated output frame " + std::to_string(frame.pts) +
                "; HRESULT=" + std::to_string(static_cast<unsigned long>(deliveryHr)), deliveryHr);
            return deliveryHr;
        }

        ++emittedThisPass;
        if (!terminalDrain && emittedThisPass >= kMaxFramesPerPass) {
            // The worker will re-arm itself after this pass completes. The next
            // bounded pass either consumes already-eligible output or returns
            // available=false and sleeps until a later source publication.
            continuationRequired = true;
            m_diagProcessingPassCapped.store(true, std::memory_order_release);
            LogInfo("AsyncProcessing/Drain", "Reached per-worker-pass safety cap=" +
                std::to_string(kMaxFramesPerPass) + ", generation=" + std::to_string(generation) +
                "; requesting one generation-aware bounded continuation without waiting for another source publication.");
            return S_OK;
        }
    }
}

void CDirectMotion::ProcessingWorkerMain() noexcept {
    AsyncPipelineState::Generation observedGeneration = 0;
    try {
        for (;;) {
            bool terminalDrain = false;
            {
                std::unique_lock<std::mutex> stateLock(m_processingWorkerMutex);
                m_processingWorkerChanged.wait(stateLock, [this]() {
                    return m_processingWorkerStop ||
                        (!m_processingWorkerSuspend && (m_processingWorkPending || m_processingEosRequested));
                });
                if (m_processingWorkerStop) break;
                m_processingWorkPending = false;
                terminalDrain = m_processingEosRequested;
                m_processingWorkerActive = true;
                m_diagProcessingPassCapped.store(false, std::memory_order_release);
            }

            const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
            if (generation != 0) m_asyncPipeline.SetWorkerBusy(generation, AsyncWorkerKind::Processing, true);
            if (generation != observedGeneration) {
                observedGeneration = generation;
                m_asyncPipeline.SetWorkerState(generation, AsyncWorkerKind::Processing, AsyncWorkerState::Running);
                LogInfo("AsyncProcessing/Generation", "Processing worker attached to source/graph generation=" +
                    std::to_string(generation));
            }

            HRESULT result = S_OK;
            bool continuationRequired = false;
            try {
                result = ProcessAvailableFrames(generation, terminalDrain, continuationRequired);
            } catch (const std::exception& ex) {
                result = E_UNEXPECTED;
                if (!IsGraphTransitioning()) FailRuntime("AsyncProcessing/Worker",
                    std::string("Unhandled processing-worker exception: ") + ex.what(), result);
            } catch (...) {
                result = E_UNEXPECTED;
                if (!IsGraphTransitioning()) FailRuntime("AsyncProcessing/Worker",
                    "Unhandled unknown processing-worker exception.", result);
            }
            DirectMotionTrayClearCurrentThreadWork();

            if (terminalDrain && SUCCEEDED(result) &&
                (!m_asyncPipeline.IsCurrentGeneration(generation) || IsGraphTransitioning())) {
                result = E_ABORT;
            }

            if (generation != 0) m_asyncPipeline.SetWorkerBusy(generation, AsyncWorkerKind::Processing, false);
            bool continuationScheduled = false;
            {
                std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
                m_processingWorkerActive = false;
                const bool rearmContinuation = DirectMotionProcessingContinuation::ShouldRearmAfterPass(
                    continuationRequired,
                    terminalDrain,
                    m_processingWorkerStop,
                    m_processingWorkerSuspend,
                    m_asyncPipeline.IsCurrentGeneration(generation),
                    IsGraphTransitioning(),
                    IsPauseTransitionPending(),
                    m_runtimeFailed.load(std::memory_order_acquire),
                    SUCCEEDED(result));
                if (rearmContinuation) {
                    // Do not overwrite a Receive notification that arrived while
                    // this pass was active. Both signals coalesce into the same
                    // mutex-protected pending bit, so exactly one follow-up pass
                    // is guaranteed and no wakeup can be lost.
                    m_processingWorkPending = true;
                    continuationScheduled = true;
                }
                if (terminalDrain) {
                    m_processingEosResult = result;
                    m_processingEosCompleted = true;
                    m_processingEosRequested = false;
                }
            }
            if (continuationScheduled) {
                LogInfo("AsyncProcessing/Continuation", "Re-armed one bounded processing pass after the 128-output cap. generation=" +
                    std::to_string(generation) + ", terminalDrain=0; the worker will return to its condition-variable wait if the next pass finds no eligible output.");
            }
            m_processingWorkerChanged.notify_all();
        }
    } catch (...) {
        const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
        if (generation != 0) m_asyncPipeline.FailGeneration(generation,
            "processing worker terminated after an internal synchronization exception");
        LogError("AsyncProcessing/Worker", "Processing worker terminated after an internal synchronization exception.");
    }

    bool workerStopping = false;
    {
        std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
        workerStopping = m_processingWorkerStop;
    }
    if (observedGeneration != 0) {
        m_asyncPipeline.SetWorkerBusy(observedGeneration, AsyncWorkerKind::Processing, false);
        m_asyncPipeline.SetWorkerState(observedGeneration,
            AsyncWorkerKind::Processing, workerStopping ? AsyncWorkerState::Exited : AsyncWorkerState::Failed);
    }
    {
        std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
        m_processingWorkerActive = false;
        if (m_processingEosRequested && !m_processingEosCompleted) {
            m_processingEosResult = E_ABORT;
            m_processingEosCompleted = true;
        }
    }
    m_processingWorkerStarted.store(false, std::memory_order_release);
    m_processingWorkerChanged.notify_all();
    DirectMotionTrayClearCurrentThreadWork();
    LogInfo("AsyncProcessing/Worker", "Processing worker thread exited. generation=" +
        std::to_string(observedGeneration));
}

HRESULT CDirectMotion::RequestProcessingEndOfStream(AsyncPipelineState::Generation generation) noexcept {
    try {
        if (generation == 0 || !m_asyncPipeline.IsCurrentGeneration(generation)) return E_ABORT;
        if (m_settings.enabled && m_engine && m_engine->IsInitialized()) {
            std::string error;
            // Finalization appends the bounded synthetic look-ahead and wakes any Prefetch
            // source request already in flight. It is cache-mutex protected and is safe
            // alongside the processing worker's current GetFrame call.
            if (!m_engine->FinalizeSource(error)) {
                FailRuntime("EndOfStream/FinalizeSource", error, E_FAIL);
                return E_FAIL;
            }
        }

        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            if (m_processingWorkerStop || m_processingWorkerSuspend ||
                !m_processingWorkerStarted.load(std::memory_order_acquire))
                return VFW_E_WRONG_STATE;
            m_processingEosRequested = true;
            m_processingEosCompleted = false;
            m_processingEosResult = S_OK;
            m_processingWorkPending = true;
        }
        m_processingWorkerChanged.notify_all();

        std::unique_lock<std::mutex> stateLock(m_processingWorkerMutex);
        m_processingWorkerChanged.wait(stateLock, [this]() {
            return m_processingEosCompleted || m_processingWorkerStop;
        });
        if (m_processingWorkerStop) return E_ABORT;
        return m_processingEosResult;
    } catch (const std::bad_alloc&) {
        DirectMotionLog("ERROR", "EndOfStream/AsyncProcessing", "Out of memory while requesting terminal processing drain.");
        return E_OUTOFMEMORY;
    } catch (...) {
        DirectMotionLog("ERROR", "EndOfStream/AsyncProcessing", "Unexpected exception while requesting terminal processing drain.");
        return E_UNEXPECTED;
    }
}

void CDirectMotion::NotifyDeliveryWorker() noexcept {
    try {
        m_deliveryPauseChanged.notify_all();
    } catch (...) {
        // Notification is best-effort; the delivery worker also rechecks state on a
        // bounded timed wait so shutdown/seek cannot leave a retained packet stranded.
    }
}

bool CDirectMotion::WaitForDeliveryAdmission(
    AsyncPipelineState::Generation generation, bool terminalDrain) noexcept {
    try {
        std::unique_lock<std::mutex> lock(m_deliveryPauseMutex);
        auto ready = [this, generation, terminalDrain]() noexcept {
            if (m_deliveryWorkerStop.load(std::memory_order_acquire) ||
                m_stopRequested.load(std::memory_order_acquire) ||
                !m_asyncPipeline.IsCurrentGeneration(generation) ||
                m_flushTransition.Active() ||
                m_segmentTransition.Active()) return true;
            return !DirectMotionPauseRetention::ShouldHoldDelivery(
                m_pauseHoldActive.load(std::memory_order_acquire), terminalDrain, false);
        };
        while (!ready()) m_deliveryPauseChanged.wait_for(lock, std::chrono::milliseconds(25));
        if (m_deliveryWorkerStop.load(std::memory_order_acquire) ||
            m_stopRequested.load(std::memory_order_acquire) ||
            !m_asyncPipeline.IsCurrentGeneration(generation) ||
            m_flushTransition.Active() ||
            m_segmentTransition.Active()) return false;
        return terminalDrain || !m_pauseHoldActive.load(std::memory_order_acquire);
    } catch (...) {
        return false;
    }
}

void CDirectMotion::DeliveryWorkerMain() noexcept {
    AsyncPipelineState::Generation observedGeneration = 0;
    std::shared_ptr<AsyncOutputCompletion> activeCompletion;
    AsyncPipelineState::Generation activeGeneration = 0;
    while (!m_deliveryWorkerStop.load(std::memory_order_acquire)) {
      try {
            const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
            if (generation == 0) {
                std::this_thread::yield();
                continue;
            }
            if (generation != observedGeneration) {
                observedGeneration = generation;
                m_asyncPipeline.SetWorkerState(generation, AsyncWorkerKind::Delivery, AsyncWorkerState::Running);
                LogInfo("AsyncDelivery/Generation", "Delivery worker attached to output generation=" +
                    std::to_string(generation));
            }

            AsyncOutputFrame packet;
            const AsyncQueueStatus queueStatus = m_asyncPipeline.WaitDequeueOutput(generation, packet);
            if (queueStatus == AsyncQueueStatus::StaleGeneration) continue;
            if (queueStatus == AsyncQueueStatus::Cancelled) {
                if (m_deliveryWorkerStop.load(std::memory_order_acquire)) break;
                // A cancelled generation cannot accept further frames. Back off briefly
                // until BeginGeneration publishes a replacement; never spin on a closed CV.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (queueStatus == AsyncQueueStatus::Closed) {
                // EOS is drained by the processing worker without closing the output
                // queue. A closed queue is therefore an unexpected lifecycle transition.
                if (m_deliveryWorkerStop.load(std::memory_order_acquire)) break;
                m_asyncPipeline.FailGeneration(generation,
                    "output queue closed while the delivery worker was active");
                LogError("AsyncDelivery/Queue", "Output queue closed unexpectedly; failing the active delivery generation.");
                // Keep the long-lived delivery thread alive for the next
                // BeginGeneration. A closed generation is retired, not a reason
                // to permanently remove the only downstream delivery owner.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (queueStatus != AsyncQueueStatus::Item) continue;

            activeGeneration = generation;
            activeCompletion = packet.completion;
            m_asyncPipeline.SetWorkerBusy(generation, AsyncWorkerKind::Delivery, true);
            HRESULT deliveryHr = E_ABORT;
            bool deliveryException = false;
            if (!m_deliveryWorkerStop.load(std::memory_order_acquire) &&
                m_asyncPipeline.IsCurrentGeneration(generation) && !IsGraphTransitioning() &&
                packet.generation == generation) {
                if (packet.drainBarrier) {
                    // FIFO ordering makes this marker an EOS fence: every packet ahead of it
                    // has already completed downstream Deliver before this acknowledgement.
                    deliveryHr = S_OK;
                    LogInfo("AsyncDelivery/DrainBarrier", "Reached terminal output barrier after all preceding packets. generation=" +
                        std::to_string(generation) + ", sequence=" + std::to_string(packet.sequence));
                } else {
                    const uint32_t repeatCount = packet.repeatCount > 0 ? packet.repeatCount : 1;
                    uint32_t repeatIndex = 0;
                    while (repeatIndex < repeatCount) {
                        // One fallback packet owns one source image but can represent
                        // multiple target-rate slots. Keep the current slot stable across
                        // a strict Pause retry; never replay slots already delivered.
                        const int64_t outputPts = packet.pts + static_cast<int64_t>(repeatIndex);
                        for (;;) {
                            if (!WaitForDeliveryAdmission(generation, packet.terminalDrain)) {
                                deliveryHr = E_ABORT;
                                break;
                            }
                            const uint64_t pauseEpochBeforeDelivery =
                                m_strictPauseEpoch.load(std::memory_order_acquire);
                            if (!packet.terminalDrain && m_pauseHoldActive.load(std::memory_order_acquire))
                                continue;
                            if (m_deliveryWorkerStop.load(std::memory_order_acquire) ||
                                !m_asyncPipeline.IsCurrentGeneration(generation) || IsGraphTransitioning()) {
                                deliveryHr = E_ABORT;
                                break;
                            }
                            try {
                                deliveryHr = DeliverInterpolated(packet, outputPts);
                            } catch (...) {
                                deliveryHr = E_UNEXPECTED;
                                deliveryException = true;
                                DirectMotionLog("ERROR", "AsyncDelivery/Deliver",
                                    "Exception while delivering one output packet; packet will be dropped and the delivery worker will continue.");
                            }
                            // Compare epochs rather than transient pause flags: this worker may
                            // be rescheduled after Run has already cleared the hold marker.
                            const bool pauseInterruptedPacket =
                                m_strictPauseEpoch.load(std::memory_order_acquire) != pauseEpochBeforeDelivery;
                            const bool lifecycleInvalidated = m_stopRequested.load(std::memory_order_acquire) ||
                                m_flushTransition.Active() ||
                                m_segmentTransition.Active();
                            const bool expectedPauseCancellation = deliveryHr == E_ABORT ||
                                deliveryHr == VFW_E_NOT_COMMITTED;
                            if (FAILED(deliveryHr) && DirectMotionPauseRetention::ShouldRetryInterruptedDelivery(
                                    pauseInterruptedPacket, packet.terminalDrain,
                                    m_asyncPipeline.IsCurrentGeneration(generation), lifecycleInvalidated,
                                    m_deliveryWorkerStop.load(std::memory_order_acquire),
                                    expectedPauseCancellation)) {
                                LogInfo("AsyncDelivery/PauseRetention", "Retaining the dequeued output slot interrupted by strict Pause until Run. generation=" +
                                    std::to_string(generation) + ", sequence=" + std::to_string(packet.sequence) +
                                    ", pts=" + std::to_string(outputPts) + ", HRESULT=" +
                                    std::to_string(static_cast<unsigned long>(deliveryHr)) +
                                    "; this exact slot remains ahead of queued successors.");
                                continue;
                            }
                            break;
                        }
                        if (FAILED(deliveryHr)) break;
                        if (packet.seekFallback && packet.seekFallbackFirstPacket && repeatIndex == 0) {
                            const uint64_t deliveredTickNs = MonotonicTickNanoseconds();
                            const uint64_t armedTickNs = packet.seekFallbackArmedTickNs;
                            const double seekToDownstreamMs = armedTickNs != 0 && deliveredTickNs >= armedTickNs
                                ? static_cast<double>(deliveredTickNs - armedTickNs) / 1000000.0
                                : -1.0;
                            LogInfo("SeekFallback/FirstFrameDelivered",
                                "First source-frame fallback was accepted by downstream Deliver; generation=" +
                                std::to_string(generation) + ", sourceFrame=" +
                                std::to_string(packet.fallbackSourceFrameIndex) + ", outputPts=" +
                                std::to_string(outputPts) + ", streamOrigin100ns=" + std::to_string(packet.streamTimeOrigin100ns) +
                                ", outputTimeBase=" + std::to_string(packet.outputTimeBaseNumerator) + "/" + std::to_string(packet.outputTimeBaseDenominator) +
                                ", targetFrameDuration100ns=" + std::to_string(packet.targetFrameDuration100ns) +
                                ", segmentStart100ns=" + std::to_string(packet.segmentMetadataValid ? packet.segmentStart100ns : 0) +
                                ", seekToDownstreamMs=" +
                                std::to_string(seekToDownstreamMs) +
                                ". This measures fallback-arm to downstream Receive acceptance, not EVR scan-out.");
                        }
                        ++repeatIndex;
                    }
                    if (packet.terminalDrain && FAILED(deliveryHr) &&
                        m_asyncPipeline.IsCurrentGeneration(generation) && !IsGraphTransitioning() &&
                        !m_runtimeFailed.load(std::memory_order_acquire) &&
                        (IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire))) {
                        LogInfo("AsyncDelivery/TerminalPreroll", "Terminal output could not be submitted during Pause; discarding it as non-visible preroll and preserving EOS ordering. HRESULT=" +
                            std::to_string(static_cast<unsigned long>(deliveryHr)) + ", sequence=" +
                            std::to_string(packet.sequence));
                        deliveryHr = S_OK;
                    }
                }
            } else {
                LogInfo("AsyncDelivery/Discard", "Discarded a queued frame before downstream delivery because its generation or graph state is no longer active. generation=" +
                    std::to_string(generation) + ", packetGeneration=" + std::to_string(packet.generation) +
                    ", sequence=" + std::to_string(packet.sequence));
            }

            if (SUCCEEDED(deliveryHr) && !packet.drainBarrier) {
                const long long deliveredFrames = m_outputFrameCount.load(std::memory_order_acquire);
                if (deliveredFrames > 0 && (deliveredFrames % 120) == 0)
                    AuditDownstreamRenderer(deliveredFrames);
            } else if (FAILED(deliveryHr)) {
                const bool lifecycleCancellation =
                    !m_asyncPipeline.IsCurrentGeneration(generation) || IsGraphTransitioning() ||
                    IsPauseTransitionPending() || m_pauseCallInProgress.load(std::memory_order_acquire) ||
                    !m_runActive.load(std::memory_order_acquire);
                if (lifecycleCancellation) {
                    if (deliveryHr == VFW_E_NOT_COMMITTED &&
                        !m_pauseHoldActive.load(std::memory_order_acquire))
                        m_processingNeedsReset.store(true, std::memory_order_release);
                    LogInfo("AsyncDelivery/Cancelled", "Output delivery was interrupted by Pause/Run/Flush/Stop/segment transition; HRESULT=" +
                        std::to_string(static_cast<unsigned long>(deliveryHr)) +
                        ", generation=" + std::to_string(generation) + ", sequence=" +
                        std::to_string(packet.sequence));
                } else {
                    if (deliveryException) {
                        LogError("AsyncDelivery/Deliver", "Dropped output sequence " +
                            std::to_string(packet.sequence) + " after a recoverable delivery exception; the current generation and worker remain active.");
                    } else {
                        m_asyncPipeline.FailGeneration(generation, "downstream delivery failed for output sequence " +
                            std::to_string(packet.sequence));
                        FailRuntime("AsyncDelivery/Deliver", "Could not deliver interpolated output sequence " +
                            std::to_string(packet.sequence) + "; HRESULT=" +
                            std::to_string(static_cast<unsigned long>(deliveryHr)), deliveryHr);
                    }
                }
            }
            DirectMotionTrayClearCurrentThreadWork();
            if (activeCompletion) activeCompletion->Complete(
                static_cast<int32_t>(static_cast<uint32_t>(deliveryHr)));
            activeCompletion.reset();
            m_asyncPipeline.SetWorkerBusy(generation, AsyncWorkerKind::Delivery, false);
            activeGeneration = 0;
      } catch (const std::exception& ex) {
        // An exception while handling one dequeued packet must not permanently
        // kill the single downstream delivery owner. Complete any terminal waiter,
        // drop only that packet, and continue servicing this (or the next) generation.
        if (activeCompletion) activeCompletion->Complete(static_cast<int32_t>(0x8000FFFFu)); // E_UNEXPECTED
        if (activeGeneration != 0)
            m_asyncPipeline.SetWorkerBusy(activeGeneration, AsyncWorkerKind::Delivery, false);
        activeCompletion.reset();
        activeGeneration = 0;
        DirectMotionTrayClearCurrentThreadWork();
        try {
            LogError("AsyncDelivery/Worker", std::string("Recoverable delivery iteration exception; packet discarded and worker remains alive: ") + ex.what());
        } catch (...) {
            DirectMotionLog("ERROR", "AsyncDelivery/Worker", "Recoverable delivery iteration exception; packet discarded and worker remains alive.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } catch (...) {
        if (activeCompletion) activeCompletion->Complete(static_cast<int32_t>(0x8000FFFFu)); // E_UNEXPECTED
        if (activeGeneration != 0)
            m_asyncPipeline.SetWorkerBusy(activeGeneration, AsyncWorkerKind::Delivery, false);
        activeCompletion.reset();
        activeGeneration = 0;
        DirectMotionTrayClearCurrentThreadWork();
        DirectMotionLog("ERROR", "AsyncDelivery/Worker", "Recoverable unknown delivery iteration exception; packet discarded and worker remains alive.");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    if (observedGeneration != 0) {
        m_asyncPipeline.SetWorkerBusy(observedGeneration, AsyncWorkerKind::Delivery, false);
        m_asyncPipeline.SetWorkerState(observedGeneration, AsyncWorkerKind::Delivery,
            AsyncWorkerState::Exited);
    }
    DirectMotionTrayClearCurrentThreadWork();
    LogInfo("AsyncDelivery/Worker", "Delivery worker thread exited. generation=" +
        std::to_string(observedGeneration) + ", stopRequested=" +
        std::to_string(m_deliveryWorkerStop.load(std::memory_order_acquire) ? 1 : 0));
}

HRESULT CDirectMotion::QueueInterpolatedForDelivery(
    ActiveFrucEngine::OutputFrame& frame, bool prerollSample,
    AsyncPipelineState::Generation generation, bool awaitDelivery) {
    if (!DirectMotionProcessingGeneration::MatchesEngineGeneration(
            generation, m_engineGeneration.load(std::memory_order_acquire)) ||
        !m_asyncPipeline.IsCurrentGeneration(generation)) return E_ABORT;

    // ArmSeekFallback() runs only after the output generation has advanced and
    // before the first source frame is published to that generation. Therefore a
    // current-generation ProcessingWorker pass cannot legitimately need fallback
    // classification until this release-published mirror is true. Older passes are
    // rejected by the generation checks above and again by the output FIFO.
    const bool fallbackActivePublished =
        m_seekFallbackActivePublished.load(std::memory_order_acquire);
    const bool transitionActivePublished =
        m_seekTransitionActivePublished.load(std::memory_order_acquire);
    bool fallbackHandoff = false;
    int64_t fallbackWatermark = -1;
    uint64_t fallbackPackets = 0;
    uint64_t fallbackFrames = 0;
    uint64_t fallbackDropped = 0;
    bool interpolationIsPreroll = prerollSample;
    struct OutputTimelineSnapshot final {
        REFERENCE_TIME streamTimeOrigin = 0;
        REFERENCE_TIME targetFrameDuration = 0;
        FrucRational outputTimeBase{};
        FrucRational targetRate{};
        REFERENCE_TIME prerollBoundaryTime = 0;
        int64_t prerollBoundaryOutputPts = 0;
        bool prerollBoundaryTimeValid = false;
        bool prerollBoundaryOutputPtsValid = false;
        bool visibleSourceBoundaryKnown = false;
        REFERENCE_TIME segmentStart = 0;
        bool segmentMetadataValid = false;
    } timeline;
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        timeline.streamTimeOrigin = m_streamTimeOrigin;
        timeline.targetFrameDuration = m_targetFrameDuration;
        timeline.outputTimeBase = m_outputTimeBase;
        timeline.targetRate = m_targetRate;
        timeline.prerollBoundaryTime = m_prerollBoundaryTime;
        timeline.prerollBoundaryOutputPts = m_prerollBoundaryOutputPts;
        timeline.prerollBoundaryTimeValid = m_prerollBoundaryTimeValid;
        timeline.prerollBoundaryOutputPtsValid = m_prerollBoundaryOutputPtsValid;
        timeline.visibleSourceBoundaryKnown =
            timeline.prerollBoundaryTimeValid || timeline.prerollBoundaryOutputPtsValid;
        timeline.segmentStart = m_currentSegmentStart;
        timeline.segmentMetadataValid = m_currentSegmentMetadataValid;

        // This classification is needed only while a seek fallback is active.
        // The same timeline snapshot is reused below to build the queued packet,
        // avoiding a second lock acquisition and a split view of the timeline.
        if (fallbackActivePublished) {
            REFERENCE_TIME outputStart = timeline.streamTimeOrigin;
            if (timeline.outputTimeBase.num > 0 && timeline.outputTimeBase.den > 0) {
                outputStart += FrucRescaleRound(frame.pts, timeline.outputTimeBase,
                    FrucRational{1, 10000000});
            }
            const bool beforeVisibleBoundary = timeline.prerollBoundaryTimeValid
                ? outputStart < timeline.prerollBoundaryTime
                : (timeline.prerollBoundaryOutputPtsValid &&
                    frame.pts < timeline.prerollBoundaryOutputPts);
            interpolationIsPreroll = DirectMotionSeekFallback::ShouldTreatInterpolationAsPreroll(
                interpolationIsPreroll, timeline.visibleSourceBoundaryKnown,
                outputStart < 0 || beforeVisibleBoundary);
        }
    }
    interpolationIsPreroll = interpolationIsPreroll ||
        (awaitDelivery && (IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)));
    bool fallbackWarmupReady = false;
    std::shared_ptr<const std::vector<uint8_t>> transitionSourceFrame;
    uint32_t transitionBlendIndex = 0;
    bool transitionBlendPrepared = false;
    if (fallbackActivePublished || transitionActivePublished) {
        std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
        if (fallbackActivePublished) {
            if (!DirectMotionSeekFallback::OwnsFallbackGeneration(
                    m_seekFallbackGeneration, generation)) return E_ABORT;
            if (DirectMotionSeekFallback::ShouldSealSourceFallback(
                    m_seekFallbackActive, m_seekFallbackAcceptingSource, interpolationIsPreroll)) {
                m_seekFallbackAcceptingSource = false;
                fallbackWarmupReady = true;
                DirectMotionTraySetSeekHandoffState(
                    DirectMotionSeekHandoffStatus::State::PtsWatermarkDrain, generation);
            }
            const auto decision = DirectMotionSeekFallback::ClassifyInterpolation(
                m_seekFallbackActive, m_seekFallbackHasQueuedFrame,
                m_seekFallbackLastQueuedPts, frame.pts, interpolationIsPreroll);
            if (decision == DirectMotionSeekFallback::InterpolationDecision::DropCoveredFrame)
                return S_FALSE;
            if (decision == DirectMotionSeekFallback::InterpolationDecision::HandoffToInterpolation &&
                m_seekFallbackActive) {
                fallbackHandoff = true;
                fallbackWatermark = m_seekFallbackLastQueuedPts;
                fallbackPackets = m_seekFallbackQueuedPackets;
                fallbackFrames = m_seekFallbackQueuedFrames;
                fallbackDropped = m_seekFallbackDroppedPackets;
                m_seekFallbackActive = false;
                m_seekFallbackActivePublished.store(false, std::memory_order_release);

                // Transfer the most recent successfully queued source image into a
                // short, generation-owned visual blend. The normal and fallback
                // output lanes remain separate and PTS-merged exactly as before.
                m_seekTransitionSourceFrame = m_seekFallbackLastSourceFrame;
                m_seekFallbackLastSourceFrame.reset();
                if (m_seekTransitionSourceFrame &&
                    DirectMotionSeekTransition::Begin(m_seekTransitionState, generation)) {
                    transitionSourceFrame = m_seekTransitionSourceFrame;
                    m_seekTransitionActivePublished.store(true, std::memory_order_release);
                } else {
                    m_seekTransitionSourceFrame.reset();
                    DirectMotionSeekTransition::Reset(m_seekTransitionState);
                    m_seekTransitionActivePublished.store(false, std::memory_order_release);
                }

                // The PTS handoff has sealed source-fallback admission. Keep the shared
                // byte budget active only if SourceBootstrapQueue still has staged work;
                // its SetFallbackAdmissionActive() combines both ownership states.
                m_sourceBootstrapQueue.SetFallbackAdmissionActive(false);
            }
        }

        // The ProcessingWorker is the only interpolated-output producer. Snapshot
        // the current blend step under the same generation mutex; commit it only
        // after the packet has entered the bounded output FIFO successfully.
        if (!transitionSourceFrame && m_seekTransitionSourceFrame &&
            DirectMotionSeekTransition::TryAcquire(
                m_seekTransitionState, generation, transitionBlendIndex)) {
            transitionSourceFrame = m_seekTransitionSourceFrame;
        }
        transitionBlendPrepared = static_cast<bool>(transitionSourceFrame);
        if (fallbackHandoff) {
            DirectMotionTraySetSeekHandoffState(
                transitionBlendPrepared
                    ? DirectMotionSeekHandoffStatus::State::VisualBlend
                    : DirectMotionSeekHandoffStatus::State::Idle,
                generation);
        }
    }
    if (fallbackWarmupReady) {
        const uint64_t armTickNs = m_seekFallbackArmedTickNs.load(std::memory_order_acquire);
        const uint64_t readyTickNs = DirectMotionIsLoggingEnabled() ? MonotonicTickNanoseconds() : 0;
        const double armToWarmupMs = armTickNs != 0 && readyTickNs >= armTickNs
            ? static_cast<double>(readyTickNs - armTickNs) / 1000000.0 : -1.0;
        LogInfo("SeekFallback/WarmupReady", "MVTools produced its first visible interpolated frame; stopped admitting new source duplicates and will hand off after the already queued fallback PTS watermark; armToWarmupMs=" + std::to_string(armToWarmupMs) + ".");
    }
    if (fallbackHandoff) {
        const uint64_t armTickNs = m_seekFallbackArmedTickNs.load(std::memory_order_acquire);
        const uint64_t handoffTickNs = DirectMotionIsLoggingEnabled() ? MonotonicTickNanoseconds() : 0;
        const double armToHandoffMs = armTickNs != 0 && handoffTickNs >= armTickNs
            ? static_cast<double>(handoffTickNs - armTickNs) / 1000000.0 : -1.0;
        LogInfo("SeekFallback/Handoff", "Switching from duplicated source frames to MVTools interpolation at the first non-covered output PTS; armToHandoffMs=" + std::to_string(armToHandoffMs) + ", generation=" +
            std::to_string(generation) + ", lastFallbackPts=" + std::to_string(fallbackWatermark) +
            ", firstInterpolatedPts=" + std::to_string(frame.pts) +
            ", fallbackPackets=" + std::to_string(fallbackPackets) +
            ", fallbackOutputFrames=" + std::to_string(fallbackFrames) +
            ", fallbackQueueDrops=" + std::to_string(fallbackDropped) +
            ", visualBlendSource=" + std::to_string(transitionBlendPrepared ? 1 : 0) +
            ", visualBlendFrames=" + std::to_string(transitionBlendPrepared ? DirectMotionSeekTransition::kBlendFrameCount : 0) +
            "; the output cadence and shared delivery FIFO remain unchanged.");
        m_seekFallbackArmedTickNs.store(0, std::memory_order_release);
    }

    std::shared_ptr<AsyncOutputCompletion> completion;
    if (awaitDelivery) {
        try {
            completion = std::make_shared<AsyncOutputCompletion>();
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    AsyncOutputFrame packet;
    packet.generation = generation;
    packet.sequence = m_outputPacketSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    packet.terminalDrain = awaitDelivery;
    packet.pts = frame.pts;
    packet.outputLineSizeY = frame.outputLineSizeY;
    packet.outputLineSizeUV = frame.outputLineSizeUV;
    packet.explicitPreroll = prerollSample;
    packet.typeSpecificFlags = m_interpolatedTypeSpecificFlags.load(std::memory_order_acquire);
    packet.streamTimeOrigin100ns = timeline.streamTimeOrigin;
    packet.targetFrameDuration100ns = timeline.targetFrameDuration;
    packet.outputTimeBaseNumerator = timeline.outputTimeBase.num;
    packet.outputTimeBaseDenominator = timeline.outputTimeBase.den;
    packet.targetFpsNumerator = timeline.targetRate.num;
    packet.targetFpsDenominator = timeline.targetRate.den;
    packet.prerollBoundaryTime100ns = timeline.prerollBoundaryTime;
    packet.prerollBoundaryOutputPts = timeline.prerollBoundaryOutputPts;
    packet.prerollBoundaryTimeValid = timeline.prerollBoundaryTimeValid;
    packet.prerollBoundaryOutputPtsValid = timeline.prerollBoundaryOutputPtsValid;
    packet.segmentStart100ns = timeline.segmentStart;
    packet.segmentMetadataValid = timeline.segmentMetadataValid;
    packet.transitionFromNv12 = transitionSourceFrame;
    packet.transitionBlendIndex = transitionBlendIndex;
    packet.transitionBlendFrames = transitionBlendPrepared
        ? DirectMotionSeekTransition::kBlendFrameCount : 0;
    packet.nv12 = std::move(frame.nv12);
    packet.completion = completion;

    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::OutputQueue,
        "WaitEnqueueOutput / wait for output FIFO capacity",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    const AsyncQueueStatus enqueueStatus = m_asyncPipeline.WaitEnqueueOutput(std::move(packet));
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    if (enqueueStatus != AsyncQueueStatus::Queued) {
        if (enqueueStatus == AsyncQueueStatus::StaleGeneration ||
            enqueueStatus == AsyncQueueStatus::Cancelled) return E_ABORT;
        if (enqueueStatus == AsyncQueueStatus::Closed) return VFW_E_WRONG_STATE;
        return E_FAIL;
    }

    if (transitionBlendPrepared) {
        std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
        if (m_seekTransitionSourceFrame == transitionSourceFrame &&
            DirectMotionSeekTransition::Commit(
                m_seekTransitionState, generation, transitionBlendIndex)) {
            if (m_seekTransitionState.generation == 0) {
                m_seekTransitionSourceFrame.reset();
                m_seekTransitionActivePublished.store(false, std::memory_order_release);
            }
        }
    }

    // Normal streaming is decoupled: bounded FIFO capacity is the only backpressure.
    // Terminal EOS keeps acknowledgements so downstream EndOfStream cannot overtake
    // the last generated sample.
    if (!awaitDelivery) return S_OK;
    const int32_t deliveryResult = completion->Wait();
    return static_cast<HRESULT>(static_cast<uint32_t>(deliveryResult));
}

HRESULT CDirectMotion::QueueSeekFallbackForDelivery(
    const std::vector<uint8_t>& nv12, int64_t sourceFrameIndex, bool sourcePreroll,
    AsyncPipelineState::Generation generation) noexcept {
    try {
        std::vector<uint8_t> owned(nv12);
        const auto shared = m_sourceBootstrapQueue.CreatePayload(owned);
        if (!shared) return S_FALSE;
        return QueueSeekFallbackForDelivery(shared, sourceFrameIndex, sourcePreroll, generation);
    } catch (...) {
        return S_FALSE;
    }
}

HRESULT CDirectMotion::QueueSeekFallbackForDelivery(
    const std::shared_ptr<const std::vector<uint8_t>>& sharedNv12,
    int64_t sourceFrameIndex, bool sourcePreroll,
    AsyncPipelineState::Generation generation) noexcept {
    if (!sharedNv12) return S_FALSE;
    return QueueSeekFallbackPayloadForDelivery(sharedNv12->data(), sharedNv12->size(),
        sharedNv12, sourceFrameIndex, sourcePreroll, generation);
}

HRESULT CDirectMotion::QueueSeekFallbackPayloadForDelivery(
    const uint8_t* nv12Data, size_t nv12Bytes,
    const std::shared_ptr<const std::vector<uint8_t>>& sharedNv12,
    int64_t sourceFrameIndex, bool sourcePreroll,
    AsyncPipelineState::Generation generation) noexcept {
    try {
        {
            std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
            if (!DirectMotionSeekFallback::OwnsFallbackGeneration(
                    m_seekFallbackGeneration, generation)) return S_FALSE;
            if (!DirectMotionSeekFallback::ShouldQueueSourceFallback(
                    m_seekFallbackActive, m_seekFallbackAcceptingSource)) return S_FALSE;
        }
        // Upstream preroll belongs to the decoder's hidden warm-up, not the visible
        // fallback stream. Its source indices remain part of the engine's PTS lattice.
        if (sourcePreroll) return S_FALSE;
        if (!nv12Data || sourceFrameIndex < 0 || nv12Bytes != static_cast<size_t>(m_width) *
            static_cast<size_t>(m_height) * 3 / 2 || generation == 0 ||
            !m_asyncPipeline.IsCurrentGeneration(generation)) return S_FALSE;

        FrucRational sourceRate = m_sourceRate;
        FrucRational targetRate{};
        DirectMotionSeekFallback::OutputSpan sourceSpan{};
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            targetRate = m_targetRate;
        }
        if (!DirectMotionSeekFallback::ComputeOutputSpan(
                sourceFrameIndex, sourceRate, targetRate, sourceSpan) ||
            sourceSpan.repeatCount == 0) return S_FALSE;

        AsyncOutputFrame packet;
        packet.generation = generation;
        packet.sequence = m_outputPacketSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        packet.repeatCount = sourceSpan.repeatCount;
        packet.seekFallback = true;
        packet.fallbackSourceFrameIndex = sourceFrameIndex;
        packet.explicitPreroll = sourcePreroll;
        packet.outputLineSizeY = m_width;
        packet.outputLineSizeUV = m_width;
        packet.typeSpecificFlags = m_interpolatedTypeSpecificFlags.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            packet.streamTimeOrigin100ns = m_streamTimeOrigin;
            packet.targetFrameDuration100ns = m_targetFrameDuration;
            packet.outputTimeBaseNumerator = m_outputTimeBase.num;
            packet.outputTimeBaseDenominator = m_outputTimeBase.den;
            packet.targetFpsNumerator = m_targetRate.num;
            packet.targetFpsDenominator = m_targetRate.den;
            packet.prerollBoundaryTime100ns = m_prerollBoundaryTime;
            packet.prerollBoundaryOutputPts = m_prerollBoundaryOutputPts;
            packet.prerollBoundaryTimeValid = m_prerollBoundaryTimeValid;
            packet.prerollBoundaryOutputPtsValid = m_prerollBoundaryOutputPtsValid;
            packet.segmentStart100ns = m_currentSegmentStart;
            packet.segmentMetadataValid = m_currentSegmentMetadataValid;
        }
        if (sharedNv12) {
            packet.sharedNv12 = sharedNv12;
        } else {
            packet.nv12.assign(nv12Data, nv12Data + nv12Bytes);
        }

        AsyncQueueStatus enqueueStatus = AsyncQueueStatus::Cancelled;
        int64_t firstQueuedPts = sourceSpan.firstPts;
        uint32_t repeatCount = sourceSpan.repeatCount;
        bool firstFallbackPacket = false;
        {
            std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
            if (!DirectMotionSeekFallback::OwnsFallbackGeneration(
                    m_seekFallbackGeneration, generation) ||
                !m_asyncPipeline.IsCurrentGeneration(generation)) return S_FALSE;
            if (!DirectMotionSeekFallback::ShouldQueueSourceFallback(
                    m_seekFallbackActive, m_seekFallbackAcceptingSource) ||
                !m_asyncPipeline.IsCurrentGeneration(generation)) return S_FALSE;
            if (m_seekFallbackHasQueuedFrame) {
                if (m_seekFallbackLastQueuedPts == (std::numeric_limits<int64_t>::max)()) return S_FALSE;
                firstQueuedPts = m_seekFallbackLastQueuedPts + 1;
            }
            if (firstQueuedPts >= sourceSpan.endPtsExclusive) return S_FALSE;
            const int64_t slots = sourceSpan.endPtsExclusive - firstQueuedPts;
            if (slots <= 0 || slots > static_cast<int64_t>((std::numeric_limits<uint32_t>::max)()))
                return S_FALSE;
            repeatCount = static_cast<uint32_t>(slots);
            packet.pts = firstQueuedPts;
            packet.repeatCount = repeatCount;
            firstFallbackPacket = !m_seekFallbackHasQueuedFrame;
            packet.seekFallbackFirstPacket = firstFallbackPacket;
            packet.seekFallbackArmedTickNs = m_seekFallbackArmedTickNs.load(std::memory_order_acquire);
            enqueueStatus = m_asyncPipeline.TryEnqueueOutput(std::move(packet));
            if (enqueueStatus == AsyncQueueStatus::Queued) {
                m_seekFallbackHasQueuedFrame = true;
                m_seekFallbackLastQueuedPts = sourceSpan.endPtsExclusive - 1;
                m_seekFallbackLastSourceFrame = sharedNv12;
                ++m_seekFallbackQueuedPackets;
                m_seekFallbackQueuedFrames += repeatCount;
            } else if (enqueueStatus == AsyncQueueStatus::Full) {
                ++m_seekFallbackDroppedPackets;
            }
        }

        if (enqueueStatus == AsyncQueueStatus::Queued && firstFallbackPacket) {
            const uint64_t armTickNs = m_seekFallbackArmedTickNs.load(std::memory_order_acquire);
            const uint64_t queueTickNs = DirectMotionIsLoggingEnabled() ? MonotonicTickNanoseconds() : 0;
            const double armToQueueMs = armTickNs != 0 && queueTickNs >= armTickNs
                ? static_cast<double>(queueTickNs - armTickNs) / 1000000.0 : -1.0;
            LogInfo("SeekFallback/Start", "Queued the first non-interpolated source-frame fallback packet after the segment change; armToFallbackQueueMs=" + std::to_string(armToQueueMs) + ", generation=" +
                std::to_string(generation) + ", sourceFrame=" + std::to_string(sourceFrameIndex) +
                ", firstOutputPts=" + std::to_string(firstQueuedPts) +
                ", repeatedOutputSlots=" + std::to_string(repeatCount) +
                ", targetFps=" + std::to_string(targetRate.num) + "/" + std::to_string(targetRate.den) +
                "; MVTools warm-up continues concurrently.");
        }
        // The fallback is deliberately best-effort. A full queue must never stall
        // Receive or delay source publication; the next source image can cover the gap.
        return enqueueStatus == AsyncQueueStatus::Queued ? S_OK : S_FALSE;
    } catch (const std::bad_alloc&) {
        LogInfo("SeekFallback", "Skipped an optional fallback frame because its bounded packet payload could not be allocated; MVTools warm-up continues.");
        return S_FALSE;
    } catch (...) {
        LogInfo("SeekFallback", "Skipped an optional fallback frame after an unexpected packet-construction error; MVTools warm-up continues.");
        return S_FALSE;
    }
}

void CDirectMotion::ArmSeekFallback(
    const char* reason, AsyncPipelineState::Generation generation) noexcept {
    try {
        if (!DirectMotionSeekFallback::CanArmFallbackGeneration(
                generation, m_asyncPipeline.CurrentGeneration())) return;
        std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
        // Recheck after acquiring the state mutex. A later seek may have advanced
        // the output generation while this callback was waiting for the lock.
        if (!DirectMotionSeekFallback::CanArmFallbackGeneration(
                generation, m_asyncPipeline.CurrentGeneration())) return;
        m_sourceBootstrapQueue.SetFallbackAdmissionActive(true);
        m_seekFallbackGeneration = generation;
        m_seekFallbackActive = true;
        m_seekFallbackAcceptingSource = true;
        m_seekFallbackHasQueuedFrame = false;
        m_seekFallbackLastQueuedPts = -1;
        m_seekFallbackLastSourceFrame.reset();
        m_seekTransitionSourceFrame.reset();
        DirectMotionSeekTransition::Reset(m_seekTransitionState);
        m_seekTransitionActivePublished.store(false, std::memory_order_release);
        m_seekFallbackArmedTickNs.store(MonotonicTickNanoseconds(), std::memory_order_release);
        m_seekTraceFirstReceiveLogged.store(false, std::memory_order_release);
        m_seekTraceFirstVisibleLogged.store(false, std::memory_order_release);
        m_seekTracePrerollSamples.store(0, std::memory_order_release);
        m_seekFallbackQueuedPackets = 0;
        m_seekFallbackQueuedFrames = 0;
        m_seekFallbackDroppedPackets = 0;
        m_seekFallbackActivePublished.store(true, std::memory_order_release);
        DirectMotionTraySetSeekHandoffState(
            DirectMotionSeekHandoffStatus::State::SourceFallback, generation);
        LogInfo("SeekFallback/Armed", std::string("Armed progressive source-frame fallback for a new media timeline; reason=") +
            (reason ? reason : "seek") +
            ". Handoff is immediate when a visible MVTools output beyond the sealed PTS frontier is ready; required source lookahead and generation checks remain in force.");
    } catch (...) {
        DirectMotionLog("WARN", "SeekFallback/Armed", "Could not arm progressive seek fallback; normal interpolation remains active.");
    }
}

void CDirectMotion::ClearSeekFallback(
    AsyncPipelineState::Generation expectedGeneration) noexcept {
    try {
        {
            std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
            if (expectedGeneration != 0 &&
                !DirectMotionSeekFallback::OwnsFallbackGeneration(
                    m_seekFallbackGeneration, expectedGeneration)) return;
            m_seekFallbackActive = false;
            m_seekFallbackAcceptingSource = false;
            m_seekFallbackHasQueuedFrame = false;
            m_seekFallbackLastQueuedPts = -1;
            m_seekFallbackLastSourceFrame.reset();
            m_seekTransitionSourceFrame.reset();
            DirectMotionSeekTransition::Reset(m_seekTransitionState);
            m_seekTransitionActivePublished.store(false, std::memory_order_release);
            m_seekFallbackArmedTickNs.store(0, std::memory_order_release);
            m_seekTraceFirstReceiveLogged.store(false, std::memory_order_release);
            m_seekTraceFirstVisibleLogged.store(false, std::memory_order_release);
            m_seekTracePrerollSamples.store(0, std::memory_order_release);
            m_seekFallbackQueuedPackets = 0;
            m_seekFallbackQueuedFrames = 0;
            m_seekFallbackDroppedPackets = 0;
            const AsyncPipelineState::Generation clearedGeneration = m_seekFallbackGeneration;
            m_seekFallbackGeneration = 0;
            m_seekFallbackActivePublished.store(false, std::memory_order_release);
            DirectMotionTraySetSeekHandoffState(
                DirectMotionSeekHandoffStatus::State::Idle, clearedGeneration);
            // Serialize the shared-budget handoff with ArmSeekFallback(). If this
            // update were performed after releasing m_seekFallbackMutex, a new seek
            // could arm admission first and then be accidentally disabled by this
            // older clear operation.
            m_sourceBootstrapQueue.SetFallbackAdmissionActive(false);
        }
        // The staging FIFO may already be drained, but source fallback payloads can
        // remain queued/in-flight. Stop new payload admission only when the PTS
        // handoff has completed; existing shared payloads keep their byte charge.
    } catch (...) {}
}

void CDirectMotion::ResetSeekFallbackProgress(
    AsyncPipelineState::Generation expectedGeneration) noexcept {
    try {
        std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
        if (!DirectMotionSeekFallback::OwnsFallbackGeneration(
                m_seekFallbackGeneration, expectedGeneration)) return;
        // A same-generation timeline reset must also retire a transition that has
        // already sealed fallback admission. Do this before the active-fallback
        // early return, otherwise a stale source image could remain pinned/visible.
        m_seekFallbackLastSourceFrame.reset();
        m_seekTransitionSourceFrame.reset();
        DirectMotionSeekTransition::Reset(m_seekTransitionState);
        m_seekTransitionActivePublished.store(false, std::memory_order_release);
        if (!m_seekFallbackActive) {
            DirectMotionTraySetSeekHandoffState(
                DirectMotionSeekHandoffStatus::State::Idle, expectedGeneration);
            return;
        }
        m_seekFallbackAcceptingSource = true;
        DirectMotionTraySetSeekHandoffState(
            DirectMotionSeekHandoffStatus::State::SourceFallback, expectedGeneration);
        m_seekFallbackHasQueuedFrame = false;
        m_seekFallbackLastQueuedPts = -1;
        m_seekFallbackArmedTickNs.store(MonotonicTickNanoseconds(), std::memory_order_release);
        m_seekTraceFirstReceiveLogged.store(false, std::memory_order_release);
        m_seekTraceFirstVisibleLogged.store(false, std::memory_order_release);
        m_seekTracePrerollSamples.store(0, std::memory_order_release);
        m_seekFallbackQueuedPackets = 0;
        m_seekFallbackQueuedFrames = 0;
        m_seekFallbackDroppedPackets = 0;
    } catch (...) {}
}

HRESULT CDirectMotion::QueueDeliveryDrainBarrier(AsyncPipelineState::Generation generation) {
    if (generation == 0 || !m_asyncPipeline.IsCurrentGeneration(generation)) return E_ABORT;
    std::shared_ptr<AsyncOutputCompletion> completion;
    try {
        completion = std::make_shared<AsyncOutputCompletion>();
    } catch (...) {
        return E_OUTOFMEMORY;
    }

    AsyncOutputFrame barrier;
    barrier.generation = generation;
    barrier.sequence = m_outputPacketSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    barrier.drainBarrier = true;
    barrier.completion = completion;
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::OutputQueue,
        "WaitEnqueueOutput / enqueue terminal EOS drain barrier",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    const AsyncQueueStatus enqueueStatus = m_asyncPipeline.WaitEnqueueOutput(std::move(barrier));
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    if (enqueueStatus != AsyncQueueStatus::Queued) {
        if (enqueueStatus == AsyncQueueStatus::StaleGeneration ||
            enqueueStatus == AsyncQueueStatus::Cancelled) return E_ABORT;
        if (enqueueStatus == AsyncQueueStatus::Closed) return VFW_E_WRONG_STATE;
        return E_FAIL;
    }

    // The completion is acknowledged by DeliveryWorkerMain only after all preceding FIFO
    // packets have completed Deliver. EndOfStream can therefore never overtake queued output.
    return static_cast<HRESULT>(static_cast<uint32_t>(completion->Wait()));
}

// IQualProp is optional and exists only on some video renderers. Probe it sparsely
// after the delivery worker reports a successful Deliver, to distinguish acceptance
// by the downstream pin from a frame that the renderer actually drew. This helper is diagnostic-only.
void CDirectMotion::AuditDownstreamRenderer(long long deliveredFrames) noexcept {
    IPin* connectedPin = nullptr;
    HRESULT hr = (m_pOutput && m_pOutput->IsConnected())
        ? m_pOutput->ConnectedTo(&connectedPin) : VFW_E_NOT_CONNECTED;
    if (FAILED(hr) || !connectedPin) {
        LogInfo("Output/RendererAudit", "deliveredFrames=" + std::to_string(deliveredFrames) +
            ", rendererProbe=unavailable, ConnectedToHr=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return;
    }

    PIN_INFO pinInfo{};
    hr = connectedPin->QueryPinInfo(&pinInfo);
    connectedPin->Release();
    if (FAILED(hr) || !pinInfo.pFilter) {
        LogInfo("Output/RendererAudit", "deliveredFrames=" + std::to_string(deliveredFrames) +
            ", rendererProbe=unavailable, QueryPinInfoHr=" +
            std::to_string(static_cast<unsigned long>(hr)));
        if (pinInfo.pFilter) pinInfo.pFilter->Release();
        return;
    }

    IQualProp* quality = nullptr;
    const HRESULT queryHr = pinInfo.pFilter->QueryInterface(IID_IQualProp,
        reinterpret_cast<void**>(&quality));
    const std::string rendererPin = WidePathToUtf8(pinInfo.achName);
    if (FAILED(queryHr) || !quality) {
        LogInfo("Output/RendererAudit", "deliveredFrames=" + std::to_string(deliveredFrames) +
            ", rendererPin=" + rendererPin +
            ", IQualProp=unsupported, queryHr=" +
            std::to_string(static_cast<unsigned long>(queryHr)) +
            "; successful Deliver does not confirm presentation.");
        pinInfo.pFilter->Release();
        return;
    }

    int drawn = -1, dropped = -1, avgFrameRateHundredths = -1;
    int avgSyncOffsetMs = 0, jitterMs = 0;
    const HRESULT drawnHr = quality->get_FramesDrawn(&drawn);
    const HRESULT droppedHr = quality->get_FramesDroppedInRenderer(&dropped);
    const HRESULT rateHr = quality->get_AvgFrameRate(&avgFrameRateHundredths);
    const HRESULT syncHr = quality->get_AvgSyncOffset(&avgSyncOffsetMs);
    const HRESULT jitterHr = quality->get_Jitter(&jitterMs);
    LogInfo("Output/RendererAudit", "deliveredFrames=" + std::to_string(deliveredFrames) +
        ", rendererPin=" + rendererPin +
        ", framesDrawn=" + (SUCCEEDED(drawnHr) ? std::to_string(drawn) : "unavailable") +
        ", framesDropped=" + (SUCCEEDED(droppedHr) ? std::to_string(dropped) : "unavailable") +
        ", avgFrameRateRaw=" + (SUCCEEDED(rateHr) ? std::to_string(avgFrameRateHundredths) : "unavailable") +
        ", avgFrameRateFps=" + (SUCCEEDED(rateHr) ? std::to_string(avgFrameRateHundredths / 100.0) : "unavailable") +
        ", avgSyncOffsetMs=" + (SUCCEEDED(syncHr) ? std::to_string(avgSyncOffsetMs) : "unavailable") +
        ", jitterMs=" + (SUCCEEDED(jitterHr) ? std::to_string(jitterMs) : "unavailable"));
    quality->Release();
    pinInfo.pFilter->Release();
}

// The negotiated VIDEOINFOHEADER2 may describe an interlaced source. When the
// upstream sample explicitly requests WEAVE, synthesized full-frame outputs must
// carry the same per-sample instruction; allocator samples are reused and their
// previous dwTypeSpecificFlags must never leak into a new interpolated frame.
void CDirectMotion::CaptureInterpolatedSampleFlags(IMediaSample* sample) noexcept {
    if (!sample) {
        m_interpolatedTypeSpecificFlags.store(0, std::memory_order_release);
        return;
    }

    DWORD sourceFlags = 0;
    bool haveSample2Flags = false;
    IMediaSample2* sample2 = nullptr;
    const HRESULT queryHr = sample->QueryInterface(IID_IMediaSample2,
        reinterpret_cast<void**>(&sample2));
    if (SUCCEEDED(queryHr) && sample2) {
        AM_SAMPLE2_PROPERTIES properties{};
        const long propertyBytes = FIELD_OFFSET(AM_SAMPLE2_PROPERTIES, dwStreamId);
        const HRESULT propertiesHr = sample2->GetProperties(propertyBytes,
            reinterpret_cast<PBYTE>(&properties));
        if (SUCCEEDED(propertiesHr)) {
            sourceFlags = properties.dwTypeSpecificFlags;
            haveSample2Flags = true;
        }
        sample2->Release();
    }

    const bool preserveWeave = m_inputInterlacedTwoFieldsPerSample.load(std::memory_order_acquire) &&
        haveSample2Flags && (sourceFlags & AM_VIDEO_FLAG_WEAVE) != 0;
    const DWORD outputFlags = preserveWeave ? AM_VIDEO_FLAG_WEAVE : 0;
    m_interpolatedTypeSpecificFlags.store(outputFlags, std::memory_order_release);

    if (DirectShowFullTraceEnabled()) {
        LogInfo("Input/SampleFlags", "Captured source IMediaSample2 type-specific flags=" +
            std::to_string(sourceFlags) + ", propertiesAvailable=" +
            std::to_string(haveSample2Flags ? 1 : 0) + ", interlacedTwoFieldsPerSample=" +
            std::to_string(m_inputInterlacedTwoFieldsPerSample.load(std::memory_order_acquire) ? 1 : 0) +
            ", interpolatedOutputFlags=" + std::to_string(outputFlags));
    }
}

void CDirectMotion::ResetEngineGenerationState(
    bool preserveOutputSequence,
    AsyncPipelineState::Generation expectedFallbackGeneration) noexcept {
    ResetSeekFallbackProgress(expectedFallbackGeneration);
    m_nextInputPts = 0;
    m_firstOutput.store(true, std::memory_order_release);
    m_bootstrapQueueState.Reset();
    m_processingNeedsReset.store(false, std::memory_order_release);
    m_pauseFrameGate.ClearPresented();
    m_firstSampleAfterSegment = false;
    m_streamInitialized = false;
    m_runtimeFailed.store(false, std::memory_order_release);
    m_lastOutputSourceIndex.store(-1, std::memory_order_release);
    if (!preserveOutputSequence)
        m_outputPacketSequence.store(0, std::memory_order_release);
    m_lastInputSampleStart = 0;
    m_lastInputSampleEnd = 0;
    m_lastInputTimestampValid = false;
    m_lastInputSampleStopTimeValid = false;
    m_prerollBoundaryInitialized = false;
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_streamTimeOrigin = 0;
        m_streamTimeOriginSet = false;
        m_outputTimeBase = FrucNormalizeRational(m_targetRate.den, m_targetRate.num);
        m_prerollBoundaryTime = 0;
        m_prerollBoundaryTimeValid = false;
        m_prerollBoundaryOutputPts = 0;
        m_prerollBoundaryOutputPtsValid = false;
        m_lastDeliveredOutputEnd.store(0, std::memory_order_release);
        m_lastDeliveredOutputTimeValid.store(false, std::memory_order_release);
    }
    m_inputFrameCount.store(0, std::memory_order_release);
    m_outputFrameCount.store(0, std::memory_order_release);
    m_passthroughFrameCount.store(0, std::memory_order_release);
    DirectMotionTrayResetThroughput();
    m_lastPushMs.store(-1.0, std::memory_order_release);
    m_avgPushMs.store(-1.0, std::memory_order_release);
    m_pushTimingSamples.store(0, std::memory_order_release);
    m_lastGraphMs.store(-1.0, std::memory_order_release);
    m_avgGraphMs.store(-1.0, std::memory_order_release);
    m_graphTimingSamples.store(0, std::memory_order_release);
    m_lastAllocatorWaitMs.store(-1.0, std::memory_order_release);
    m_lastDownstreamMs.store(-1.0, std::memory_order_release);
    m_avgDownstreamMs.store(-1.0, std::memory_order_release);
    m_downstreamTimingSamples.store(0, std::memory_order_release);
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
}

void CDirectMotion::ResetEngine(bool prepareResumeWarmStart) noexcept {
    // A non-deferred reset retires any queued bootstrap frames. A frame already
    // popped by the worker carries the old queue epoch and will be rejected before
    // it can touch the replacement engine.
    m_sourceBootstrapQueue.Cancel();
    std::unique_lock<std::mutex> processingLifecycleFence(m_processingWorkerLifecycleMutex);
    m_resumeWarmStartPending.store(prepareResumeWarmStart, std::memory_order_release);
    AsyncPipelineState::Generation resetGeneration = m_asyncPipeline.CurrentGeneration();
    const AsyncPipelineState::Generation currentGeneration = resetGeneration;
    const auto engineGeneration = m_engineGeneration.load(std::memory_order_acquire);
    if (m_processingWorkerStarted.load(std::memory_order_acquire) &&
        engineGeneration != 0 && currentGeneration == engineGeneration) {
        std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
        const auto nextGeneration = m_asyncPipeline.BeginGeneration();
        resetGeneration = nextGeneration;
        DirectMotionLog("INFO", "AsyncPipeline/Reset",
            "Retired queued work before engine reset. oldGeneration=" + std::to_string(currentGeneration) +
            ", newGeneration=" + std::to_string(nextGeneration));
    }
    InterruptPendingSourceRequests("ResetEngine");
    if (!SuspendProcessingWorkerForEngineReset()) {
        FailRuntime("AsyncProcessing/ResetWait",
            "Could not establish that the active ProcessingWorker pass has exited; the current AviSynth/MVTools engine is being retained to avoid use-after-free.",
            E_ABORT);
        return;
    }
    {
        CAutoLock receiveLock(&m_csReceive);
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
        if (m_engine) m_engine->InterruptSourceWaits();
        SetEvaluationEnabled(true, "engine reset/seek bootstrap re-arm");
        if (m_engine) {
            DirectMotionLog("INFO", "Engine/Reset", "Resetting and releasing the active interpolation graph.");
            m_engine->Reset();
            m_engine.reset();
            DirectMotionLog("INFO", "Engine/Reset", "Interpolation graph reset completed.");
        }
        // A delayed synchronous reset must not reset state installed by a newer seek.
        // Serialize this final state commit with BeginGeneration and carry the token
        // captured by this reset rather than adopting CurrentGeneration() again.
        {
            std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
            if (m_asyncPipeline.CurrentGeneration() == resetGeneration) {
                // Generation zero is authoritative even if a prior failed initialization
                // or partial lifecycle left no owning engine object behind.
                m_engineGeneration.store(0, std::memory_order_release);
                DirectMotionSeekReset::Reset(m_seekResetState);
                ResetEngineGenerationState(false, resetGeneration);
            } else {
                LogInfo("Engine/Reset", "Skipped stale reset-state commit because a newer delivery generation is already active. resetGeneration=" +
                    std::to_string(resetGeneration) + ", currentGeneration=" +
                    std::to_string(m_asyncPipeline.CurrentGeneration()));
            }
        }
    }
    ResumeProcessingWorkerAfterEngineReset();
    RefreshTrayRuntimeInfo();
}

bool CDirectMotion::BeginDeferredSeekEngineReset(
    AsyncPipelineState::Generation& expectedGeneration,
    bool awaitNewSegment) noexcept {
    try {
        if (expectedGeneration == 0) return false;
        std::unique_lock<std::mutex> processingLifecycleFence(m_processingWorkerLifecycleMutex);
        bool workerStarted = false;
        bool workerActive = false;

        // Hold the delivery-generation fence across every shared-state mutation.
        // Otherwise a newer BeginGeneration could overtake this callback after its
        // first validation but before it suspends processing or resets fallback PTS.
        std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
        const AsyncPipelineState::Generation currentGeneration =
            m_asyncPipeline.CurrentGeneration();
        if (currentGeneration != expectedGeneration) {
            LogInfo("Engine/DeferredSeekReset", "Rejected a stale deferred-reset callback before it could mutate a newer pipeline generation. expectedGeneration=" +
                std::to_string(expectedGeneration) + ", currentGeneration=" +
                std::to_string(currentGeneration));
            return false;
        }

        const auto engineGeneration = m_engineGeneration.load(std::memory_order_acquire);
        if (m_processingWorkerStarted.load(std::memory_order_acquire) &&
            engineGeneration != 0 && currentGeneration == engineGeneration) {
            expectedGeneration = m_asyncPipeline.BeginGeneration();
            DirectMotionLog("INFO", "AsyncPipeline/DeferredSeekReset",
                "Retired the engine generation before requesting deferred teardown. oldGeneration=" +
                std::to_string(currentGeneration) + ", newGeneration=" +
                std::to_string(expectedGeneration));
        }

        m_resumeWarmStartPending.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
            workerStarted = m_processingWorkerStarted.load(std::memory_order_acquire);
            workerActive = m_processingWorkerActive;
            if (workerStarted || workerActive) {
                m_processingWorkerSuspend = true;
                m_processingWorkPending = false;
                m_diagProcessingPassCapped.store(false, std::memory_order_release);
                if (m_processingEosRequested && !m_processingEosCompleted) {
                    m_processingEosResult = E_ABORT;
                    m_processingEosCompleted = true;
                    m_processingEosRequested = false;
                }
            }
        }
        InterruptPendingSourceRequests("Seek/DeferredEngineReset");
        m_processingWorkerChanged.notify_all();
        if (m_engine) m_engine->InterruptSourceWaits();

        // The active pass observes the generation mismatch at its next interruption
        // guard. The old engine remains alive until CompleteDeferredSeekEngineReset.
        // Reset, fallback progress and staging all commit under the same generation
        // fence, using the token that the caller will subsequently attach to samples.
        m_engineGeneration.store(0, std::memory_order_release);
        const bool reboundPendingSegment = !awaitNewSegment &&
            DirectMotionSeekReset::IsAwaitingSegment(m_seekResetState) &&
            m_seekResetState.deferredResetPending &&
            DirectMotionSeekReset::RebindForNewSegment(m_seekResetState, expectedGeneration);
        if (!reboundPendingSegment && !DirectMotionSeekReset::BeginDeferredReset(
                m_seekResetState, expectedGeneration, awaitNewSegment)) {
            return false;
        }
        ResetEngineGenerationState(true, expectedGeneration);
        if (m_sourceBootstrapWorkerStarted.load(std::memory_order_acquire)) {
            m_sourceBootstrapQueue.BeginGeneration(expectedGeneration, !awaitNewSegment);
            LogInfo("SourceBootstrap/Arm", "Opened a new bounded source-ingestion staging generation. generation=" +
                std::to_string(expectedGeneration) + ", byteBudget=" +
                std::to_string(m_sourceBootstrapQueue.ByteBudget()) +
                ", workerReady=" + std::to_string(awaitNewSegment ? 0 : 1) + ".");
        }

        DirectMotionLog("INFO", "Engine/DeferredSeekReset",
            "Requested non-blocking teardown. workerStarted=" + std::to_string(workerStarted ? 1 : 0) +
            ", activePassAtRequest=" + std::to_string(workerActive ? 1 : 0) +
            ", engineRetained=1, workerSuspended=" + std::to_string((workerStarted || workerActive) ? 1 : 0) +
            "; the source fallback can enter the delivery FIFO before waiting for GetFrame quiescence.");
        return true;
    } catch (...) {
        FailRuntime("Engine/DeferredSeekReset",
            "Could not request deferred seek teardown safely; the existing engine remains retained.", E_ABORT);
        return false;
    }
}

bool CDirectMotion::CompleteDeferredSeekEngineReset(
    AsyncPipelineState::Generation expectedGeneration) noexcept {
    try {
        ScopedDiagnosticTimer resetTimer(DirectMotionIsLoggingEnabled(),
            m_diagDeferredResetSamples, m_diagDeferredResetTotalNs,
            m_diagDeferredResetLastNs, m_diagDeferredResetMaxNs);
        const auto waitStarted = std::chrono::steady_clock::now();
        const AsyncPipelineState::Generation resetGeneration = expectedGeneration != 0
            ? expectedGeneration : m_asyncPipeline.CurrentGeneration();

        // The queue may contain Receive samples from the EndFlush -> NewSegment
        // window. Those samples are staged only; a bootstrap consumer must wait until
        // NewSegment has rebound the reset to its final output generation.
        {
            CAutoLock receiveLock(&m_csReceive);
            if (!DirectMotionSeekReset::CanComplete(m_seekResetState, resetGeneration))
                return true;
        }

        // Phase 1 owns only the processing-worker lifecycle mutex. Do not wait for
        // m_csReceive while holding it: NewSegment/BeginFlush enter with m_csReceive
        // and then request this lifecycle fence. Holding both in the opposite order
        // from the bootstrap worker would deadlock an overlapping seek.
        bool workerQuiescent = false;
        {
            std::unique_lock<std::mutex> processingLifecycleFence(m_processingWorkerLifecycleMutex);
            // m_seekResetState is protected by m_csReceive. Do not inspect it while
            // holding the opposite lifecycle lock; the exact output-generation token
            // is atomic here, and phase 2 revalidates the reset owner under m_csReceive
            // before detaching or finalizing anything.
            if (resetGeneration == 0 ||
                m_asyncPipeline.CurrentGeneration() != resetGeneration) return true;
            {
                ScopedDiagnosticTimer quiescenceTimer(DirectMotionIsLoggingEnabled(),
                    m_diagDeferredQuiescenceSamples, m_diagDeferredQuiescenceTotalNs,
                    m_diagDeferredQuiescenceLastNs, m_diagDeferredQuiescenceMaxNs);
                workerQuiescent = SuspendProcessingWorkerForEngineReset();
            }
        }
        if (!workerQuiescent) {
            FailRuntime("Engine/DeferredSeekReset",
                "Could not prove the old ProcessingWorker pass quiescent after fallback admission; engine remains retained.",
                E_ABORT);
            return false;
        }
        // Phase 2 briefly owns m_csReceive only to detach the retired engine.
        // ProcessingWorker is already quiescent and generation zero is published,
        // while Receive routes every sample into the active source-bootstrap queue.
        // Therefore the expensive AviSynth environment destruction can run after
        // releasing m_csReceive without exposing the old engine to either worker.
        std::unique_ptr<ActiveFrucEngine> retiredEngine;
        {
            CAutoLock receiveLock(&m_csReceive);
            const AsyncPipelineState::Generation currentGeneration = m_asyncPipeline.CurrentGeneration();
            if (currentGeneration != resetGeneration ||
                (expectedGeneration != 0 &&
                    !m_sourceBootstrapQueue.IsActiveForGeneration(resetGeneration)) ||
                m_sourceBootstrapWorkerStop.load(std::memory_order_acquire)) {
                return true;
            }
            if (!DirectMotionSeekReset::CanComplete(m_seekResetState, resetGeneration)) return true;
            if (m_engine) {
                DirectMotionLog("INFO", "Engine/DeferredSeekReset",
                    "Fallback is admitted and the old ProcessingWorker is quiescent; detaching the retired AviSynth/MVTools graph before destruction outside m_csReceive.");
                retiredEngine = std::move(m_engine);
            }
            m_engineGeneration.store(0, std::memory_order_release);
        }

        double retiredEngineDestroyMs = 0.0;
        if (retiredEngine) {
            const auto destroyStarted = std::chrono::steady_clock::now();
            {
                ScopedDiagnosticTimer destroyTimer(DirectMotionIsLoggingEnabled(),
                    m_diagDeferredDestroySamples, m_diagDeferredDestroyTotalNs,
                    m_diagDeferredDestroyLastNs, m_diagDeferredDestroyMaxNs);
                retiredEngine->Reset();
                retiredEngine.reset();
            }
            retiredEngineDestroyMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - destroyStarted).count();
        }

        // Destruction ran without m_csReceive, so a newer seek/Flush/Stop may have
        // retired this generation while the old AviSynth environment was closing.
        // Only the still-current owner may clear the deferred flag and resume the
        // worker; an older teardown must never release a newer generation's hold.
        {
            CAutoLock receiveLock(&m_csReceive);
            const bool finalizeCurrentReset =
                DirectMotionEngineBootstrapCommit::CanFinalizeRetiredEngine(
                    m_asyncPipeline.IsCurrentGeneration(resetGeneration),
                    expectedGeneration == 0 || m_sourceBootstrapQueue.IsActiveForGeneration(resetGeneration),
                    IsReceiveCommitCurrent(resetGeneration),
                    DirectMotionSeekReset::CanComplete(m_seekResetState, resetGeneration),
                    !m_sourceBootstrapWorkerStop.load(std::memory_order_acquire),
                    !m_engine);
            if (!finalizeCurrentReset) return true;
            m_engineGeneration.store(0, std::memory_order_release);
            if (!DirectMotionSeekReset::Complete(m_seekResetState, resetGeneration)) return true;
            // Receive and graph-lifecycle transitions cannot race this resume while
            // m_csReceive is held. StopProcessingWorker remains authoritative and
            // suppresses the resume if shutdown has already begun.
            ResumeProcessingWorkerAfterEngineReset();
        }
        const double waitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStarted).count();
        DirectMotionLog("INFO", "Engine/DeferredSeekReset",
            "Completed teardown after fallback admission with the old graph detached and destroyed outside m_csReceive. quiescenceAndDestroyMs=" +
            std::to_string(waitMs) + ", retiredEngineDestroyMs=" +
            std::to_string(retiredEngineDestroyMs) + "; ProcessingWorker resumed for the replacement engine.");
        RefreshTrayRuntimeInfo();
        return true;
    } catch (...) {
        FailRuntime("Engine/DeferredSeekReset",
            "Exception while completing deferred seek teardown; engine was not released unsafely.", E_ABORT);
        return false;
    }
}

HRESULT CDirectMotion::StartStreaming() {
    ClearSeekFallback();
    m_diagInputReceivedFrames.store(0, std::memory_order_release);
    m_diagInputNormalizedFrames.store(0, std::memory_order_release);
    m_diagInputPublishedFrames.store(0, std::memory_order_release);
    m_diagOutputGeneratedFrames.store(0, std::memory_order_release);
    m_diagOutputDeliveredFrames.store(0, std::memory_order_release);
    m_diagInputNormalizeSamples.store(0, std::memory_order_release);
    m_diagInputNormalizeTotalNs.store(0, std::memory_order_release);
    m_diagInputNormalizeLastNs.store(0, std::memory_order_release);
    m_diagInputNormalizeMaxNs.store(0, std::memory_order_release);
    m_diagOutputCopySamples.store(0, std::memory_order_release);
    m_diagOutputCopyTotalNs.store(0, std::memory_order_release);
    m_diagOutputCopyLastNs.store(0, std::memory_order_release);
    m_diagOutputCopyMaxNs.store(0, std::memory_order_release);
    m_diagDeferredResetSamples.store(0, std::memory_order_release);
    m_diagDeferredResetTotalNs.store(0, std::memory_order_release);
    m_diagDeferredResetLastNs.store(0, std::memory_order_release);
    m_diagDeferredResetMaxNs.store(0, std::memory_order_release);
    m_diagDeferredQuiescenceSamples.store(0, std::memory_order_release);
    m_diagDeferredQuiescenceTotalNs.store(0, std::memory_order_release);
    m_diagDeferredQuiescenceLastNs.store(0, std::memory_order_release);
    m_diagDeferredQuiescenceMaxNs.store(0, std::memory_order_release);
    m_diagDeferredDestroySamples.store(0, std::memory_order_release);
    m_diagDeferredDestroyTotalNs.store(0, std::memory_order_release);
    m_diagDeferredDestroyLastNs.store(0, std::memory_order_release);
    m_diagDeferredDestroyMaxNs.store(0, std::memory_order_release);
    m_diagProcessingPassCapped.store(false, std::memory_order_release);
    m_lastAsyncTelemetryLogTick.store(0, std::memory_order_release);
    m_lastAsyncTrayInfoRefreshTick.store(0, std::memory_order_release);
    m_stepAuditSegmentCount.store(0, std::memory_order_release);
    g_stepAuditFlushStackCount.store(0, std::memory_order_release);
    m_stepAuditInputSamplesRemaining.store(0, std::memory_order_release);
    m_stepAuditOutputSamplesRemaining.store(0, std::memory_order_release);
    m_stopRequested.store(false);
    m_flushTransition.Reset();
    m_initialPrerollActive.store(true, std::memory_order_release);
    m_pauseHoldActive.store(false, std::memory_order_release);
    // Pause()/Run() own m_pauseCallInProgress. Do not clear it here: StartStreaming
    // runs from inside the first base Pause callback, and the marker is also useful in
    // transition diagnostics until that callback has returned. It is not a Receive gate.
    m_runActive.store(false, std::memory_order_release);
    m_segmentTransition.Reset();
    {
        CAutoLock receiveLock(&m_csReceive);
        DirectMotionSeekReset::Reset(m_seekResetState);
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
        // Segment metadata belongs to one streaming session. Retaining it across
        // Stop/Start makes the first NewSegment of a new file look like a seek.
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_currentSegmentStart = 0;
        m_currentSegmentStop = (std::numeric_limits<REFERENCE_TIME>::max)();
        m_currentSegmentRate = 1.0;
        m_currentSegmentMetadataValid = false;
    }
    SetEvaluationEnabled(true, "StartStreaming/preroll");
    const bool requestedInterpolation = DirectMotionTrayInterpolationEnabled();
    if (requestedInterpolation != m_settings.enabled) {
        m_settings.enabled = requestedInterpolation;
        LogInfo("Tray/RuntimeToggle", std::string("Applied saved tray interpolation preference before streaming start: ") +
            (requestedInterpolation ? "enabled" : "disabled"));
    }
    ResetEngine();
    m_firstInputBmpAttempted = false;
    m_sourceRate = SourceFrameRate();
    const FrucRational streamingTargetRate = m_settings.enabled ? TargetFrameRate() : m_sourceRate;
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_targetRate = streamingTargetRate;
    }
    const REFERENCE_TIME streamingFrameDuration = TargetFrameDuration();
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_targetFrameDuration = streamingFrameDuration;
    }
    DirectMotionTraySetTargetFps(static_cast<double>(m_targetRate.num) /
        static_cast<double>(m_targetRate.den));
    LogInfo("StartStreaming", "Starting stream. Enabled=" + std::to_string(m_settings.enabled ? 1 : 0) +
        ", dimensions=" + std::to_string(m_width) + "x" + std::to_string(m_height) +
        ", sourceFPS=" + std::to_string(static_cast<double>(m_sourceRate.num) / m_sourceRate.den) +
        ", targetFPS=" + std::to_string(static_cast<double>(m_targetRate.num) / m_targetRate.den) +
        ", cpuProfile=" + std::to_string(m_settings.cpuProfile) +
        ", customCpuWorkers=" + std::to_string(m_settings.customCpuWorkers) +
        ", receiveBatchFrames=" + std::to_string(m_settings.receiveBatchFrames) +
        ", coldStartPrerollAdmission=1 (source samples feed AviSynth until Run)");
    bool backendReady = true;
    if (m_settings.enabled) {
        std::string error;
        if (!EnsureEngine(error)) {
            backendReady = false;
            FailRuntime("StartStreaming", error.empty()
                ? "Could not initialize the AviSynth+/MVTools interpolation backend." : error);
        } else {
            DirectMotionTraySetStatus(DirectMotionStatus::Standby);
        }
    } else {
        DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    }
    std::string deliveryWorkerError;
    bool deliveryWorkerReady = true;
    if (backendReady) {
        deliveryWorkerReady = StartDeliveryWorker(deliveryWorkerError);
        if (!deliveryWorkerReady) {
            backendReady = false;
            FailRuntime("StartStreaming/AsyncDelivery", deliveryWorkerError.empty()
                ? "Could not start the dedicated downstream delivery worker." : deliveryWorkerError);
        }
    } else {
        LogInfo("StartStreaming/AsyncDelivery", "Delivery worker was not started because the interpolation backend failed initialization.");
    }
    std::string processingWorkerError;
    bool processingWorkerReady = true;
    if (backendReady) {
        processingWorkerReady = StartProcessingWorker(processingWorkerError);
        if (!processingWorkerReady) {
            backendReady = false;
            FailRuntime("StartStreaming/AsyncProcessing", processingWorkerError.empty()
                ? "Could not start the dedicated AviSynth processing worker." : processingWorkerError);
        }
    }
    std::string sourceBootstrapWorkerError;
    bool sourceBootstrapWorkerReady = true;
    if (backendReady) {
        sourceBootstrapWorkerReady = StartSourceBootstrapWorker(sourceBootstrapWorkerError);
        if (!sourceBootstrapWorkerReady) {
            backendReady = false;
            FailRuntime("StartStreaming/SourceBootstrap", sourceBootstrapWorkerError.empty()
                ? "Could not start the dedicated source-ingestion/bootstrap worker."
                : sourceBootstrapWorkerError);
        }
    }
    if (backendReady && m_settings.enabled) {
        const AsyncPipelineState::Generation initialGeneration =
            m_asyncPipeline.CurrentGeneration();
        if (initialGeneration == 0) {
            backendReady = false;
            FailRuntime("StartStreaming/InitialSegmentStage",
                "Cannot arm cold-start source staging without an active delivery generation.",
                VFW_E_WRONG_STATE);
        } else {
            // The decoder may send its initial preroll samples before its first
            // NewSegment callback. Keep those frames in the existing bounded
            // staging queue, but do not let SourceBootstrapWorker publish them
            // into InputClip until the final segment has been announced downstream.
            m_sourceBootstrapQueue.BeginGeneration(initialGeneration, false);
            m_sourceBootstrapQueue.SetFallbackAdmissionActive(false);
            {
                CAutoLock receiveLock(&m_csReceive);
                m_preparedInitialGraphAwaitingSegment.store(true, std::memory_order_release);
            }
            LogInfo("StartStreaming/InitialSegmentStage",
                "Armed a dormant, generation-owned source staging lane for decoder samples that arrive before the first NewSegment. "
                "The prepared AviSynth graph remains empty; the bootstrap worker is gated until the downstream segment announcement completes. generation=" +
                std::to_string(initialGeneration) + ".");
        }
    }
    HRESULT hr = CTransformFilter::StartStreaming();
    if (FAILED(hr)) LogError("StartStreaming/Base", "Base transform StartStreaming failed, HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
    else m_streamInitialized = true;
    if (FAILED(hr)) {
        m_initialPrerollActive.store(false, std::memory_order_release);
        StopSourceBootstrapWorker("base StartStreaming failed");
        StopDeliveryWorker("base StartStreaming failed");
        StopProcessingWorker("base StartStreaming failed");
        return hr;
    }
    if (!backendReady) {
        m_initialPrerollActive.store(false, std::memory_order_release);
        StopSourceBootstrapWorker("processing backend startup failed");
        StopDeliveryWorker("processing backend startup failed");
        StopProcessingWorker("processing backend startup failed");
        return E_FAIL;
    }
    StartDiagnosticsWorker();
    return S_OK;
}

HRESULT CDirectMotion::StopStreaming() {
    ClearSeekFallback();
    m_initialPrerollActive.store(false, std::memory_order_release);
    m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.Cancel();
    LogInfo("StopStreaming", "Stopping stream. Inputs=" + std::to_string(m_inputFrameCount) +
        ", interpolated outputs=" + std::to_string(m_outputFrameCount) +
        ", pass-through outputs=" + std::to_string(m_passthroughFrameCount));
    // Cancel delivery first so any queued packets and optional terminal barriers are
    // completed/cancelled. Stop the bootstrap producer before suspending the processor
    // so an in-flight staged PushSourceNv12 is interrupted and joined before graph teardown.
    StopSourceBootstrapWorker("DirectShow StopStreaming");
    StopDeliveryWorker("DirectShow StopStreaming");
    StopProcessingWorker("DirectShow StopStreaming");
    {
        CAutoLock receiveLock(&m_csReceive);
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        m_currentSegmentStart = 0;
        m_currentSegmentStop = (std::numeric_limits<REFERENCE_TIME>::max)();
        m_currentSegmentRate = 1.0;
        m_currentSegmentMetadataValid = false;
    }
    ResetEngine();
    StopDiagnosticsWorker();
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    return CTransformFilter::StopStreaming();
}

HRESULT CDirectMotion::Receive(IMediaSample* sample) {
    struct ReceiveWorkPhaseCleanup final {
        ~ReceiveWorkPhaseCleanup() noexcept { DirectMotionTrayClearCurrentThreadWork(); }
    } workPhaseCleanup;
    if (!sample) {
        LogError("Receive", "Received a null IMediaSample.");
        return E_POINTER;
    }
    const bool traceReceive = DirectShowFullTraceEnabled();
    const auto receiveStarted = traceReceive ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct ReceiveTraceExit final {
        bool enabled; std::chrono::steady_clock::time_point started;
        ~ReceiveTraceExit() noexcept {
            if (!enabled) return;
            try {
                DirectMotionLog("TRACE", "Receive/Trace", "END elapsedMs=" + std::to_string(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()));
            } catch (...) {}
        }
    } traceExit{ traceReceive, receiveStarted };

    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::ReceiveLock,
        "Receive / waiting to acquire DirectShow streaming lock",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    CAutoLock receiveLock(&m_csReceive);
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    AsyncPipelineState::Generation deliveryGeneration = m_asyncPipeline.CurrentGeneration();
    if (!m_pOutput || !m_pOutput->IsConnected()) return VFW_E_NOT_CONNECTED;
    if (IsGraphTransitioning()) return VFW_E_WRONG_STATE;
    if (deliveryGeneration == 0) {
        FailRuntime("Receive/AsyncDelivery", "The output-delivery worker has no active pipeline generation.", VFW_E_WRONG_STATE);
        return VFW_E_WRONG_STATE;
    }
    const AM_SAMPLE2_PROPERTIES* sampleProps = m_pInput ? m_pInput->SampleProps() : nullptr;
    if (sampleProps && sampleProps->dwStreamId != AM_STREAM_MEDIA) {
        return DeliverDownstreamTracked(sample);
    }
    // The first source samples can precede the first NewSegment during the
    // Stopped -> Paused preroll. Keep them in the dormant bounded bootstrap lane;
    // clearing this marker here would let NewSegment mistake their pristine graph
    // for an already-running graph and destroy the only preroll samples.
    const bool initialSegmentStaging =
        DirectMotionInitialSegmentStagingPolicy::ShouldStageBeforeFirstSegment(
            m_preparedInitialGraphAwaitingSegment.load(std::memory_order_acquire),
            m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration),
            !m_sourceBootstrapQueue.IsWorkerReadyForGeneration(deliveryGeneration),
            m_engineGeneration.load(std::memory_order_acquire) == deliveryGeneration);
    if (!initialSegmentStaging)
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
    if (m_pauseHoldActive.load(std::memory_order_acquire)) {
        LogInfo("Receive/PausedHold", "Rejected one in-flight source sample that crossed the Running-to-Paused admission boundary; the renderer image is left unchanged and later samples remain backpressured.");
        return S_FALSE;
    }
    const auto rejectRetiredReceive = [this, &deliveryGeneration](const char* stage) -> HRESULT {
        LogInfo("Receive/GenerationCommit", "Discarded a media sample before " +
            std::string(stage ? stage : "commit") + "; sampleGeneration=" +
            std::to_string(deliveryGeneration) + ", currentGeneration=" +
            std::to_string(m_asyncPipeline.CurrentGeneration()) +
            ", graphTransitioning=" + std::to_string(IsGraphTransitioning() ? 1 : 0) +
            ". The sample was already in Receive when its lifecycle generation was retired.");
        return S_OK;
    };
    // NewSegment retires the output generation before it waits for m_csReceive.
    // A Receive that already passed the graph-transition admission gate must still
    // be allowed to finish its push into the old, dormant initial-preroll queue;
    // NewSegment will rebind that complete FIFO after acquiring this same lock.
    // This exception is deliberately narrower than a general stale-generation
    // commit: it applies only to this callback's initial-stage sample, while the
    // initial marker and old queue remain owned/gated and a segment transition is
    // active (never during flush, Stop or a failed runtime).
    const auto isCurrentOrInFlightInitialStage =
        [this, deliveryGeneration, initialSegmentStaging]() noexcept {
            if (IsReceiveCommitCurrent(deliveryGeneration)) return true;
            return DirectMotionInitialSegmentStagingPolicy::CanCommitInFlightInitialStage(
                initialSegmentStaging,
                m_segmentTransition.Active(),
                m_flushTransition.Active(),
                m_stopRequested.load(std::memory_order_acquire),
                m_runtimeFailed.load(std::memory_order_acquire),
                m_preparedInitialGraphAwaitingSegment.load(std::memory_order_acquire),
                m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration),
                !m_sourceBootstrapQueue.IsWorkerReadyForGeneration(deliveryGeneration),
                m_engineGeneration.load(std::memory_order_acquire) == deliveryGeneration);
        };
    if (!isCurrentOrInFlightInitialStage())
        return rejectRetiredReceive("input normalization");
    const uint64_t receivedInputFrame = m_diagInputReceivedFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
    DirectMotionTrayRecordInput(static_cast<long long>(receivedInputFrame));

    const HRESULT dynamicInputHr = ApplyDynamicInputSampleType(sample, "Receive/DynamicInputType");
    if (FAILED(dynamicInputHr)) {
        FailRuntime("Receive/DynamicInputType", "Rejected an input sample whose dynamic surface layout cannot be normalized safely; HRESULT=" +
            std::to_string(static_cast<unsigned long>(dynamicInputHr)), dynamicInputHr);
        return dynamicInputHr;
    }
    CaptureInterpolatedSampleFlags(sample);
    const bool inputPreroll = sample->IsPreroll() == S_OK;

    // A manual user toggle to interpolation-off remains supported. This is not a
    // runtime recovery route: it is the explicitly selected non-interpolating mode.
    const bool requestedInterpolation = DirectMotionTrayInterpolationEnabled();
    if (requestedInterpolation != m_settings.enabled) {
        REFERENCE_TIME toggleStart = 0, toggleEnd = 0;
        const HRESULT toggleTimeHr = sample->GetTime(&toggleStart, &toggleEnd);
        const DirectMotionSampleTime::Availability toggleTime =
            DirectMotionSampleTime::ClassifyGetTimeResult(
                toggleTimeHr, static_cast<HRESULT>(S_OK),
                static_cast<HRESULT>(VFW_S_NO_STOP_TIME));
        const bool hasToggleTime = toggleTime.hasStart;
        const bool previousEnabled = m_settings.enabled;
        // This reset can race a delivery worker blocked in GetDeliveryBuffer/Deliver.
        // Locally flush downstream before ResetEngine takes the delivery fence, then
        // reopen the current segment after the old-generation queue has been retired.
        m_pauseFrameGate.Invalidate();
        DirectMotionLifecycle::TransitionLease toggleTransitionLease(m_segmentTransition);
        const bool ownsToggleFlush = m_pOutput && m_pOutput->IsConnected();
        const HRESULT toggleBeginFlushHr = ownsToggleFlush
            ? m_pOutput->DeliverBeginFlush() : VFW_E_NOT_CONNECTED;
        if (FAILED(toggleBeginFlushHr)) {
            toggleTransitionLease.Release();
            FailRuntime("Tray/RuntimeToggle/BeginFlush", "Could not release downstream delivery before applying the explicit interpolation toggle; HRESULT=" +
                std::to_string(static_cast<unsigned long>(toggleBeginFlushHr)), toggleBeginFlushHr);
            return toggleBeginFlushHr;
        }
        ResetEngine(); // interrupts and joins the active processing pass before settings mutate
        m_settings.enabled = requestedInterpolation;
        m_sourceRate = SourceFrameRate();
        const FrucRational newTargetRate = m_settings.enabled ? TargetFrameRate() : m_sourceRate;
        const REFERENCE_TIME newFrameDuration = TargetFrameDuration();
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_targetRate = newTargetRate;
            m_targetFrameDuration = newFrameDuration;
            m_streamTimeOrigin = hasToggleTime ? toggleStart : 0;
            m_streamTimeOriginSet = hasToggleTime;
        }
        DirectMotionTraySetTargetFps(static_cast<double>(m_targetRate.num) /
            static_cast<double>(m_targetRate.den));
        DirectMotionTraySetStatus(DirectMotionStatus::Standby);
        HRESULT toggleEndFlushHr = ownsToggleFlush && m_pOutput && m_pOutput->IsConnected()
            ? m_pOutput->DeliverEndFlush() : VFW_E_NOT_CONNECTED;
        HRESULT toggleSegmentHr = S_OK;
        if (SUCCEEDED(toggleEndFlushHr)) {
            const REFERENCE_TIME segmentStart = m_currentSegmentMetadataValid ? m_currentSegmentStart : 0;
            const REFERENCE_TIME segmentStop = m_currentSegmentMetadataValid
                ? m_currentSegmentStop : std::numeric_limits<REFERENCE_TIME>::max();
            const double segmentRate = m_currentSegmentMetadataValid ? m_currentSegmentRate : 1.0;
            toggleSegmentHr = m_pOutput && m_pOutput->IsConnected()
                ? m_pOutput->DeliverNewSegment(segmentStart, segmentStop, segmentRate)
                : VFW_E_NOT_CONNECTED;
        }
        toggleTransitionLease.Release();
        if (FAILED(toggleEndFlushHr) || FAILED(toggleSegmentHr)) {
            const HRESULT transitionHr = FAILED(toggleEndFlushHr) ? toggleEndFlushHr : toggleSegmentHr;
            FailRuntime("Tray/RuntimeToggle/EndFlush", "Could not reopen downstream delivery after applying the explicit interpolation toggle; HRESULT=" +
                std::to_string(static_cast<unsigned long>(transitionHr)), transitionHr);
            return transitionHr;
        }
        LogInfo("Tray/RuntimeToggle", std::string("Applied explicit interpolation toggle: ") +
            (previousEnabled ? "enabled -> disabled" : "disabled -> enabled") +
            "; downstream delivery was flushed and the current segment re-announced.");
    }

    if (!m_settings.enabled) {
        DirectMotionTraySetStatus(DirectMotionStatus::Standby);
        return DeliverOriginalSample(sample, "Receive/DisabledOriginal");
    }
    if (m_runtimeFailed.load(std::memory_order_acquire)) {
        LogError("Receive", "Interpolation runtime is failed; rejecting the sample instead of switching to pass-through.");
        return E_FAIL;
    }

    REFERENCE_TIME inputStart = 0, inputEnd = 0;
    const HRESULT inputTimeHr = sample->GetTime(&inputStart, &inputEnd);
    const DirectMotionSampleTime::Availability inputTime =
        DirectMotionSampleTime::ClassifyGetTimeResult(
            inputTimeHr, static_cast<HRESULT>(S_OK),
            static_cast<HRESULT>(VFW_S_NO_STOP_TIME));
    const bool hasInputTime = inputTime.hasStart;
    const bool hasInputStopTime = inputTime.hasStop;
    // Keep stream-time and media-time distinct during paused frame stepping. Stream
    // timestamps are expected to restart at zero after NewSegment; when exposed by the
    // upstream decoder, media-time can reveal which decoded frame actually arrived.
    LONGLONG inputMediaStart = 0, inputMediaEnd = 0;
    const uint64_t seekArmedTickNs = DirectMotionIsLoggingEnabled()
        ? m_seekFallbackArmedTickNs.load(std::memory_order_acquire) : 0;
    const bool seekTraceFirstVisibleSample = seekArmedTickNs != 0 && !inputPreroll &&
        !m_seekTraceFirstVisibleLogged.load(std::memory_order_acquire);
    const bool auditMediaTime = DirectShowFullTraceEnabled() || seekTraceFirstVisibleSample ||
        (DirectShowTraceEnabled() && m_stepAuditInputSamplesRemaining.load(std::memory_order_acquire) > 0);
    const HRESULT inputMediaTimeHr = auditMediaTime
        ? sample->GetMediaTime(&inputMediaStart, &inputMediaEnd) : E_NOTIMPL;
    const bool hasInputMediaTime = auditMediaTime && inputMediaTimeHr == S_OK;
    const bool discontinuity = sample->IsDiscontinuity() == S_OK;
    const bool firstSampleAfterSegment = m_firstSampleAfterSegment;
    m_firstSampleAfterSegment = false;
    // Some upstream decoders deliver the first post-flush sample before their
    // NewSegment callback. Treat its discontinuity flag as belonging to the pending
    // segment transition; the sample is staged, not allowed to start a local reset.
    const bool awaitingNewSegment = DirectMotionSeekReset::IsAwaitingSegment(m_seekResetState);
    const bool expectedSegmentDiscontinuity = discontinuity &&
        (firstSampleAfterSegment || awaitingNewSegment || initialSegmentStaging);

    // Compact seek trace: distinguish upstream decoder/preroll latency from time
    // spent inside DirectMotion. After the first visible sample, this block performs
    // no clock read, counter update or log work on later Receive calls.
    if (seekArmedTickNs != 0) {
        const bool firstReceivePending = !m_seekTraceFirstReceiveLogged.load(std::memory_order_acquire);
        const bool firstVisiblePending = !inputPreroll &&
            !m_seekTraceFirstVisibleLogged.load(std::memory_order_acquire);
        const bool needTraceTick = firstReceivePending || firstVisiblePending;
        const uint64_t traceNowNs = needTraceTick && DirectMotionIsLoggingEnabled()
            ? MonotonicTickNanoseconds() : 0;
        if (firstReceivePending && traceNowNs != 0 &&
            !m_seekTraceFirstReceiveLogged.exchange(true, std::memory_order_acq_rel)) {
            const double elapsedMs = traceNowNs >= seekArmedTickNs
                ? static_cast<double>(traceNowNs - seekArmedTickNs) / 1000000.0 : -1.0;
            LogInfo("SeekTrace/FirstReceive", "First decoded media sample reached DirectMotion after NewSegment; armToReceiveMs=" +
                std::to_string(elapsedMs) + ", generation=" + std::to_string(deliveryGeneration) +
                ", sourceIndex=" + std::to_string(m_nextInputPts) +
                ", preroll=" + std::to_string(inputPreroll ? 1 : 0) +
                ", getTimeHr=" + std::to_string(static_cast<unsigned long>(inputTimeHr)) +
                ", hasStreamTime=" + std::to_string(hasInputTime ? 1 : 0) +
                ", hasStreamStopTime=" + std::to_string(hasInputStopTime ? 1 : 0) +
                ", streamStart100ns=" + std::to_string(hasInputTime ? inputStart : 0) +
                ", streamEnd100ns=" + std::to_string(hasInputStopTime ? inputEnd : 0) +
                ", segmentStart100ns=" + std::to_string(m_currentSegmentMetadataValid ? m_currentSegmentStart : 0) +
                ", discontinuity=" + std::to_string(discontinuity ? 1 : 0) +
                ", firstSampleAfterSegment=" + std::to_string(firstSampleAfterSegment ? 1 : 0) +
                ", dimensions=" + std::to_string(m_width) + "x" + std::to_string(m_height) + ".");
        }
        if (inputPreroll) {
            if (!m_seekTraceFirstVisibleLogged.load(std::memory_order_acquire))
                m_seekTracePrerollSamples.fetch_add(1, std::memory_order_acq_rel);
        } else if (firstVisiblePending && traceNowNs != 0 &&
            !m_seekTraceFirstVisibleLogged.exchange(true, std::memory_order_acq_rel)) {
            const double elapsedMs = traceNowNs >= seekArmedTickNs
                ? static_cast<double>(traceNowNs - seekArmedTickNs) / 1000000.0 : -1.0;
            LogInfo("SeekTrace/FirstVisibleReceive", "First non-preroll decoded sample reached DirectMotion; armToVisibleReceiveMs=" +
                std::to_string(elapsedMs) + ", precedingPrerollSamples=" +
                std::to_string(m_seekTracePrerollSamples.load(std::memory_order_acquire)) +
                ", generation=" + std::to_string(deliveryGeneration) +
                ", sourceIndex=" + std::to_string(m_nextInputPts) +
                ", getTimeHr=" + std::to_string(static_cast<unsigned long>(inputTimeHr)) +
                ", hasStreamTime=" + std::to_string(hasInputTime ? 1 : 0) +
                ", hasStreamStopTime=" + std::to_string(hasInputStopTime ? 1 : 0) +
                ", streamStart100ns=" + std::to_string(hasInputTime ? inputStart : 0) +
                ", streamEnd100ns=" + std::to_string(hasInputStopTime ? inputEnd : 0) +
                ", segmentStart100ns=" + std::to_string(m_currentSegmentMetadataValid ? m_currentSegmentStart : 0) +
                ", mediaTimeAvailable=" + std::to_string(hasInputMediaTime ? 1 : 0) +
                ", mediaStart=" + std::to_string(hasInputMediaTime ? inputMediaStart : 0) +
                ", mediaEnd=" + std::to_string(hasInputMediaTime ? inputMediaEnd : 0) +
                ", dimensions=" + std::to_string(m_width) + "x" + std::to_string(m_height) + ".");
        }
    }
    // Standard DirectShow seek paths already rebuild the engine in BeginFlush/EndFlush/
    // NewSegment. This timestamp guard covers hosts/sources that move the media timeline
    // without delivering one of those callbacks (for example a no-flush seek). It is
    // intentionally time-based: ordinary frame steps remain far below this threshold.
    bool timestampJump = false;
    const bool unannouncedSampleDiscontinuity = !initialSegmentStaging &&
        discontinuity && !expectedSegmentDiscontinuity &&
        (m_lastInputTimestampValid || m_lastDeliveredOutputTimeValid.load(std::memory_order_acquire));
    long double timestampDelta = 0.0L;
    REFERENCE_TIME previousInputStart = 0;
    REFERENCE_TIME previousInputEnd = 0;
    if (!initialSegmentStaging && m_lastInputTimestampValid) {
        previousInputStart = m_lastInputSampleStart;
        previousInputEnd = DirectMotionSampleTime::PreviousSampleContinuityAnchor(
            m_lastInputSampleStopTimeValid, m_lastInputSampleStart, m_lastInputSampleEnd);
        if (hasInputTime) {
            timestampDelta = static_cast<long double>(inputStart) -
                static_cast<long double>(previousInputEnd);
            if (!discontinuity) {
                timestampJump = std::fabs(timestampDelta) >
                    static_cast<long double>(kSeekTimestampJumpThreshold);
            }
        }
    }

    // Evaluate this only after timestampJump has been computed from the current sample.
    // The engine is rebuilt for a large timestamp jump or any explicit sample
    // discontinuity (the latter may use either the local-flush or simple-reset path).
    // Keep this diagnostic trigger in scope for the post-transition media-type audit.
    const bool rebuildForTimelineChange = !initialSegmentStaging && (timestampJump ||
        (discontinuity && !expectedSegmentDiscontinuity));

    if (timestampJump || unannouncedSampleDiscontinuity) {
        LogInfo("Receive/SeekGuard", std::string("Unannounced media-timeline discontinuity detected (trigger=") +
            (timestampJump ? "timestamp jump" : "sample discontinuity flag") +
            "); flushing the downstream video queue and rebuilding AviSynth/MVTools plus the bounded live source cache. previousStart100ns=" +
            std::to_string(previousInputStart) + ", previousEndOrStart100ns=" + std::to_string(previousInputEnd) +
            ", previousStopTimeAvailable=" + std::to_string(m_lastInputSampleStopTimeValid ? 1 : 0) +
            ", currentStart100ns=" + std::to_string(hasInputTime ? inputStart : 0) +
            ", currentStopTimeAvailable=" + std::to_string(hasInputStopTime ? 1 : 0) +
            ", deltaFromPreviousEndOrStartSeconds=" + std::to_string(static_cast<double>(timestampDelta /
                static_cast<long double>(kReferenceTimeUnitsPerSecond))) +
            ", thresholdSeconds=" + std::to_string(static_cast<double>(kSeekTimestampJumpThreshold) /
                static_cast<double>(kReferenceTimeUnitsPerSecond)) +
            "; this is a timestamp-based seek/discontinuity guard, not a frame-step trigger.");

        // A missing upstream flush must not leave synchronous downstream Deliver blocked,
        // nor let old-generation frames drain after the cache reset.
        m_pauseFrameGate.Invalidate();
        DirectMotionLifecycle::TransitionLease timestampTransitionLease(m_segmentTransition);
        const HRESULT beginFlushHr = m_pOutput && m_pOutput->IsConnected()
            ? m_pOutput->DeliverBeginFlush() : VFW_E_NOT_CONNECTED;
        if (FAILED(beginFlushHr)) {
            timestampTransitionLease.Release();
                    FailRuntime("Receive/TimestampDiscontinuity/BeginFlush",
                "Could not flush the downstream output queue before rebuilding after an unannounced timestamp jump; HRESULT=" +
                std::to_string(static_cast<unsigned long>(beginFlushHr)), beginFlushHr);
            return beginFlushHr;
        }

        // Invalidate queued packets only after the downstream flush has released any
        // active Deliver. The processor's source waits are then interrupted by ResetEngine.
        AsyncPipelineState::Generation oldGeneration = 0;
        AsyncPipelineState::Generation newGeneration = 0;
        {
            std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
            oldGeneration = m_asyncPipeline.CurrentGeneration();
            newGeneration = m_asyncPipeline.BeginGeneration();
        }
        LogInfo("Receive/SeekGuard", "Advanced the output generation for the locally detected timestamp jump. oldGeneration=" +
            std::to_string(oldGeneration) + ", newGeneration=" + std::to_string(newGeneration));
        deliveryGeneration = newGeneration;
        if (m_engine) m_engine->InterruptSourceWaits();
        const bool hadOutputTimeline = m_lastDeliveredOutputTimeValid.load(std::memory_order_acquire);
        const REFERENCE_TIME outputContinuation = m_lastDeliveredOutputEnd.load(std::memory_order_acquire);
        if (!BeginDeferredSeekEngineReset(newGeneration)) return E_ABORT;
        deliveryGeneration = newGeneration;
        ArmSeekFallback(timestampJump ? "Receive/no-flush timestamp jump" : "Receive/unannounced sample discontinuity",
            newGeneration);
        m_sourceRate = SourceFrameRate();
        const FrucRational newTargetRate = TargetFrameRate();
        const REFERENCE_TIME newFrameDuration = TargetFrameDuration();
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_targetRate = newTargetRate;
            m_targetFrameDuration = newFrameDuration;
            if (hadOutputTimeline) {
                m_streamTimeOrigin = outputContinuation;
                m_streamTimeOriginSet = true;
                m_lastDeliveredOutputEnd.store(outputContinuation, std::memory_order_release);
                m_lastDeliveredOutputTimeValid.store(true, std::memory_order_release);
            } else {
                if (hasInputTime) m_streamTimeOrigin = inputStart;
                m_streamTimeOriginSet = hasInputTime;
            }
        }

        const HRESULT endFlushHr = m_pOutput && m_pOutput->IsConnected()
            ? m_pOutput->DeliverEndFlush() : VFW_E_NOT_CONNECTED;
        HRESULT segmentHr = S_OK;
        if (SUCCEEDED(endFlushHr)) {
            // DirectShow expects NewSegment after a flush. Re-deliver the most recently
            // announced segment rather than inventing a new seek position/rate; before
            // the first upstream NewSegment, use the default unbounded 1x segment.
            const REFERENCE_TIME segmentStart = m_currentSegmentMetadataValid ? m_currentSegmentStart : 0;
            const REFERENCE_TIME segmentStop = m_currentSegmentMetadataValid
                ? m_currentSegmentStop : std::numeric_limits<REFERENCE_TIME>::max();
            const double segmentRate = m_currentSegmentMetadataValid ? m_currentSegmentRate : 1.0;
            segmentHr = m_pOutput && m_pOutput->IsConnected()
                ? m_pOutput->DeliverNewSegment(segmentStart, segmentStop, segmentRate)
                : VFW_E_NOT_CONNECTED;
            if (FAILED(segmentHr)) {
                LogError("Receive/TimestampDiscontinuity/NewSegment",
                    "Could not re-announce the current output segment after the local flush; HRESULT=" +
                    std::to_string(static_cast<unsigned long>(segmentHr)));
            }
        }
        timestampTransitionLease.Release();
        if (FAILED(endFlushHr)) {
            FailRuntime("Receive/TimestampDiscontinuity/EndFlush",
                "Could not reopen the downstream output queue after rebuilding for a timestamp jump; HRESULT=" +
                std::to_string(static_cast<unsigned long>(endFlushHr)), endFlushHr);
            return endFlushHr;
        }
        if (FAILED(segmentHr)) {
            FailRuntime("Receive/TimestampDiscontinuity/NewSegment",
                "Could not re-announce the current output segment after rebuilding for a timestamp jump; HRESULT=" +
                std::to_string(static_cast<unsigned long>(segmentHr)), segmentHr);
            return segmentHr;
        }

        // Defer engine construction until after this sample has been normalized and
        // its optional seek-fallback packet has entered the delivery FIFO. The
        // DeliveryWorker can present that source image while Receive initializes
        // AviSynth/MVTools and publishes it into the live InputClip.
    } else if (discontinuity && !expectedSegmentDiscontinuity) {
        LogInfo("Receive", "Unannounced input discontinuity flag set; rebuilding interpolation history.");
        m_pauseFrameGate.Invalidate();
        DirectMotionLifecycle::TransitionLease discontinuityTransitionLease(m_segmentTransition);
        const HRESULT discontinuityBeginFlushHr = m_pOutput && m_pOutput->IsConnected()
            ? m_pOutput->DeliverBeginFlush() : VFW_E_NOT_CONNECTED;
        if (FAILED(discontinuityBeginFlushHr)) {
            discontinuityTransitionLease.Release();
            FailRuntime("Receive/Discontinuity/BeginFlush", "Could not release downstream delivery before rebuilding interpolation history; HRESULT=" +
                std::to_string(static_cast<unsigned long>(discontinuityBeginFlushHr)), discontinuityBeginFlushHr);
            return discontinuityBeginFlushHr;
        }
        AsyncPipelineState::Generation oldGeneration = 0;
        AsyncPipelineState::Generation newGeneration = 0;
        {
            std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
            oldGeneration = m_asyncPipeline.CurrentGeneration();
            newGeneration = m_asyncPipeline.BeginGeneration();
        }
        LogInfo("Receive/Discontinuity", "Advanced the output generation before resetting interpolation history. oldGeneration=" +
            std::to_string(oldGeneration) + ", newGeneration=" + std::to_string(newGeneration));
        deliveryGeneration = newGeneration;
        if (!BeginDeferredSeekEngineReset(newGeneration)) return E_ABORT;
        deliveryGeneration = newGeneration;
        ArmSeekFallback("Receive/unannounced sample discontinuity", newGeneration);
        m_sourceRate = SourceFrameRate();
        const FrucRational newTargetRate = TargetFrameRate();
        const REFERENCE_TIME newFrameDuration = TargetFrameDuration();
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_targetRate = newTargetRate;
            m_targetFrameDuration = newFrameDuration;
            if (hasInputTime) m_streamTimeOrigin = inputStart;
            m_streamTimeOriginSet = hasInputTime;
        }
        const HRESULT discontinuityEndFlushHr = m_pOutput && m_pOutput->IsConnected()
            ? m_pOutput->DeliverEndFlush() : VFW_E_NOT_CONNECTED;
        HRESULT discontinuitySegmentHr = S_OK;
        if (SUCCEEDED(discontinuityEndFlushHr)) {
            const REFERENCE_TIME segmentStart = m_currentSegmentMetadataValid ? m_currentSegmentStart : 0;
            const REFERENCE_TIME segmentStop = m_currentSegmentMetadataValid
                ? m_currentSegmentStop : std::numeric_limits<REFERENCE_TIME>::max();
            const double segmentRate = m_currentSegmentMetadataValid ? m_currentSegmentRate : 1.0;
            discontinuitySegmentHr = m_pOutput && m_pOutput->IsConnected()
                ? m_pOutput->DeliverNewSegment(segmentStart, segmentStop, segmentRate)
                : VFW_E_NOT_CONNECTED;
        }
        discontinuityTransitionLease.Release();
        if (FAILED(discontinuityEndFlushHr) || FAILED(discontinuitySegmentHr)) {
            const HRESULT transitionHr = FAILED(discontinuityEndFlushHr)
                ? discontinuityEndFlushHr : discontinuitySegmentHr;
            FailRuntime("Receive/Discontinuity/EndFlush", "Could not reopen downstream delivery after rebuilding interpolation history; HRESULT=" +
                std::to_string(static_cast<unsigned long>(transitionHr)), transitionHr);
            return transitionHr;
        }
        // As in the timestamp-jump path, defer EnsureEngine until after fallback
        // admission so engine construction is no longer on the first-image path.
    } else {
        if (expectedSegmentDiscontinuity) {
            LogInfo("Receive/SegmentDiscontinuity",
                "Consumed the expected IsDiscontinuity flag on the first media sample after NewSegment; "
                "the segment transition already rebuilt the interpolation source cache, so no second rebuild is needed.");
        }
        if (hasInputTime && !m_streamTimeOriginSet) {
            REFERENCE_TIME rebasedStreamTimeOrigin = inputStart;
            int64_t sourceOutputPts = 0;
            bool hasRebasedOrigin = false;
            {
                std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
                hasRebasedOrigin =
                    DirectMotionSeekFallback::TryComputeRebasedStreamTimeOrigin100ns(
                        inputStart, m_nextInputPts, m_sourceRate, m_outputTimeBase,
                        rebasedStreamTimeOrigin, sourceOutputPts);
                m_streamTimeOrigin = hasRebasedOrigin ? rebasedStreamTimeOrigin : inputStart;
                m_streamTimeOriginSet = true;
            }
            if (hasRebasedOrigin && m_nextInputPts > 0) {
                LogInfo("Receive/TimelineOrigin", "Rebased stream-time origin from the first timed source sample rather than treating it as source frame zero: sourceIndex=" +
                    std::to_string(m_nextInputPts) + ", sourceOutputPts=" +
                    std::to_string(sourceOutputPts) + ", sampleStart100ns=" +
                    std::to_string(inputStart) + ", streamOrigin100ns=" +
                    std::to_string(rebasedStreamTimeOrigin) +
                    "; this aligns both seek fallback and MVTools output after untimed decoder preroll.");
            } else if (!hasRebasedOrigin) {
                LogInfo("Receive/TimelineOrigin", "Could not safely rebase the first timed sample's stream origin; using its timestamp directly. sourceIndex=" +
                    std::to_string(m_nextInputPts) + ", sampleStart100ns=" +
                    std::to_string(inputStart) + ".");
            } else {
                LogInfo("Receive", "Stream-time origin initialized from first timed source frame zero: " + std::to_string(inputStart));
            }
        }
    }

    // Capture the first visible source boundary in this segment. Some decoders use
    // non-negative timestamps for preroll samples, so timestamp<0 alone is not enough;
    // when the first visible sample is untimed, the output-frame-index mapping is used.
    if (!inputPreroll && !m_prerollBoundaryInitialized) {
        m_prerollBoundaryInitialized = true;
        // Prefer the exact stream-time of the first visible source sample. If that
        // sample is untimed, use its source-frame index mapped into MBlockFps output
        // frame units instead. Do not later move the boundary to a subsequent timed
        // sample: doing so would hide valid frames after an initially untimed sample.
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            if (hasInputTime) {
                m_prerollBoundaryTime = inputStart;
                m_prerollBoundaryTimeValid = true;
            }
            int64_t boundaryPts = 0;
            if (DirectMotionSeekFallback::TryComputeSourceFrameOutputPts(
                    m_nextInputPts, m_sourceRate, m_outputTimeBase, boundaryPts)) {
                m_prerollBoundaryOutputPts = boundaryPts;
                m_prerollBoundaryOutputPtsValid = true;
            }
        }
        LogInfo("Receive/PrerollBoundary", "First non-preroll source sample establishes the visible output boundary: sourceIndex=" +
            std::to_string(m_nextInputPts) + ", outputPts=" + std::to_string(m_prerollBoundaryOutputPts) +
            ", outputPtsValid=" + std::to_string(m_prerollBoundaryOutputPtsValid ? 1 : 0) +
            ", selectedBoundary=" + std::string(m_prerollBoundaryTimeValid ? "stream-time" :
                (m_prerollBoundaryOutputPtsValid ? "output-frame-index" : "unavailable")) +
            ", streamTime100ns=" + std::to_string(m_prerollBoundaryTime) + ".");
    }

    // Retain only the immediately previous source timestamps. ResetEngine clears this
    // tracker at every explicit graph flush/segment transition as well as on detected jumps.
    if (hasInputTime) {
        m_lastInputSampleStart = inputStart;
        // GetTime reports start+1 in inputEnd when VFW_S_NO_STOP_TIME is returned.
        // That sentinel is not a real sample boundary; use the start as the
        // conservative continuity anchor until a sample with a real stop arrives.
        m_lastInputSampleEnd = hasInputStopTime ? inputEnd : inputStart;
        m_lastInputTimestampValid = true;
        m_lastInputSampleStopTimeValid = hasInputStopTime;
    } else {
        m_lastInputTimestampValid = false;
        m_lastInputSampleStopTimeValid = false;
    }

    // Frame stepping is a presentation request, not a request to synthesize a
    // temporally interpolated frame from future input that the paused graph will
    // not deliver. A non-preroll sample received while stable/transitioning to Pause
    // is already the exact decoded frame the renderer must show. Deliver that source
    // sample once, visibly, then tear down the stale Prefetch generation. On Run the
    // next Receive builds a fresh graph from the new sequential source timeline.
    // Preroll samples remain hidden and are not mistaken for the requested step frame.
    const bool coldStartPreroll = DirectMotionInitialPrerollPolicy::AllowColdStartPreroll(
        m_initialPrerollActive.load(std::memory_order_acquire),
        m_runActive.load(std::memory_order_acquire),
        m_pauseHoldActive.load(std::memory_order_acquire));
    if (!m_runActive.load(std::memory_order_acquire) && !coldStartPreroll) {
        const PauseFrameGate::Epoch pauseEpoch = m_pauseFrameGate.CurrentEpoch();
        if (m_pauseHoldActive.load(std::memory_order_acquire)) {
            LogInfo("Receive/PausedHold", "Dropped the single in-flight sample that crossed the pause boundary before the hold became visible to the input-pin admission check; no paused-frame delivery is attempted.");
            return S_FALSE;
        }
        if (m_pauseFrameGate.WasPresented(pauseEpoch)) {
            // The input pin normally holds this sample outside m_csReceive. S_FALSE
            // is a harmless race retirement if it passed the post-lock check just
            // before another callback completed a paused-frame presentation.
            LogInfo("Receive/PauseBackpressure", "A racing media sample reached the transform after the paused-frame one-shot was closed; retiring that sample without changing the renderer image.");
            return S_FALSE;
        }
        if (inputPreroll) {
            // Keep feeding hidden preroll into the graph until the first visible
            // decoded sample arrives. Once that sample is shown, all later samples
            // for this paused epoch are suppressed by the one-shot marker above.
        } else {
            if (!m_pauseFrameGate.TryClaim(pauseEpoch)) {
                LogInfo("Receive/PausedFrameStep", "Dropping a duplicate or superseded paused-frame request; epoch=" +
                    std::to_string(pauseEpoch) + ".");
                return S_OK;
            }
            const HRESULT stepHr = DeliverOriginalSample(sample, "Receive/PausedFrameStep", pauseEpoch);
            if (stepHr == S_FALSE) {
                m_pauseFrameGate.ReleaseClaim(pauseEpoch);
                LogInfo("Receive/PausedFrameStep", "Discarded a paused-frame sample because its pause epoch was superseded while waiting for the renderer allocator.");
                return S_OK;
            }
            if (FAILED(stepHr)) {
                m_pauseFrameGate.ReleaseClaim(pauseEpoch);
                if (IsGraphTransitioning() || IsPauseTransitionPending()) return S_OK;
                FailRuntime("Receive/PausedFrameStep",
                    "Could not present the decoded frame requested while paused; HRESULT=" +
                    std::to_string(static_cast<unsigned long>(stepHr)), stepHr);
                return stepHr;
            }
            // A Run/Stop/Flush/seek may have superseded this callback while the
            // allocator was waiting. Never reset or re-anchor the new generation
            // from the completion of an obsolete paused-frame request.
            if (!IsPauseFrameRequestCurrent(pauseEpoch)) {
                LogInfo("Receive/PausedFrameStep", "The pause epoch changed after sample delivery; leaving the newer graph/timeline untouched. epoch=" +
                    std::to_string(pauseEpoch) + ".");
                return S_OK;
            }

            // A paused one-shot can be the first decoded frame of a seek. In that
            // case NewSegment already retired the old output/cache generation and
            // armed a deferred reset. The next ordinary Receive must publish into
            // this exact generation so SourceBootstrapWorker can complete teardown
            // and build the replacement graph. The generic ResetEngine path would
            // cancel SourceBootstrapQueue while leaving the seek fallback mirror
            // armed, making the next CreatePayload fail as if memory were exhausted.
            const bool preserveDeferredSeekBootstrap =
                DirectMotionSeekReset::ShouldPreserveDeferredBootstrapForPausedFrame(
                    m_seekResetState, deliveryGeneration,
                    m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration));
            if (preserveDeferredSeekBootstrap) {
                LogInfo("Receive/PausedFrameStep",
                    "Presented the paused seek frame without cancelling its deferred bootstrap. "
                    "The current generation retains source-payload admission and fallback ownership; "
                    "SourceBootstrapWorker will retire the old engine when the next source sample is staged. "
                    "generation=" + std::to_string(deliveryGeneration) + ".");
            } else {
                // Ordinary paused frame-step (no deferred seek owner): retain the
                // established synchronous reset and stale-Prefetch interruption.
                if (m_engine) m_engine->InterruptSourceWaits();
                ResetEngine();
            }
            // The next source sample becomes source index zero for the replacement
            // graph. Re-anchor its stream-time origin even when the deferred seek
            // reset is preserved; do not reset the rest of the seek-owned state.
            {
                std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
                m_streamTimeOrigin = 0;
                m_streamTimeOriginSet = false;
            }
            m_pauseFrameGate.MarkPresented(pauseEpoch);
            LogInfo("Receive/PausedFrameStep",
                "Presented the single decoded non-preroll frame for paused epoch=" + std::to_string(pauseEpoch) +
                "; the next upstream Receive is held outside m_csReceive until Run or lifecycle cancellation. "
                "MVTools lookahead was bypassed; ordinary frame-step resets the old graph, while a seek-owned deferred reset is preserved for the next source sample. inputStart100ns=" +
                std::to_string(hasInputTime ? inputStart : 0) +
                ", inputEnd100ns=" + std::to_string(hasInputStopTime ? inputEnd : 0) +
                ", inputStopTimeAvailable=" + std::to_string(hasInputStopTime ? 1 : 0) +
                ", hasMediaTime=" + std::to_string(hasInputMediaTime ? 1 : 0) +
                ", mediaStart=" + std::to_string(hasInputMediaTime ? inputMediaStart : 0) +
                ", mediaEnd=" + std::to_string(hasInputMediaTime ? inputMediaEnd : 0) + ".");
            return S_OK;
        }
    } else if (coldStartPreroll && m_diagInputReceivedFrames.load(std::memory_order_acquire) == 0) {
        LogInfo("Receive/InitialPreroll", "Cold-start paused sample is entering the normal NV12/InputClip path; the three-frame MVTools bootstrap is allowed to complete instead of parking Receive after one pass-through frame.");
    }

    const int64_t inputFrameIndex = m_inputFrameCount.fetch_add(1, std::memory_order_acq_rel);
    const bool traceInput = DirectShowFullTraceEnabled() ||
        (DirectShowTraceEnabled() && ConsumeTraceBudget(m_stepAuditInputSamplesRemaining));
    std::string error;
    BYTE* input = nullptr;
    HRESULT hr = sample->GetPointer(&input);
    if (FAILED(hr) || !input) {
        error = "IMediaSample::GetPointer failed, HRESULT=" + std::to_string(static_cast<unsigned long>(hr));
        FailRuntime("Receive/GetPointer", error, FAILED(hr) ? hr : E_POINTER);
        return FAILED(hr) ? hr : E_POINTER;
    }
    const LONG actualLength = sample->GetActualDataLength();
    if (actualLength < 0 || static_cast<size_t>(actualLength) < m_inputBufferBytes) {
        error = "Input sample is shorter than the negotiated semi-planar frame. actual=" +
            std::to_string(actualLength) + ", expectedAtLeast=" + std::to_string(m_inputBufferBytes);
        FailRuntime("Receive/BufferLength", error, E_FAIL);
        return E_FAIL;
    }

    std::vector<uint8_t> normalizedInput;
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::InputCopy,
        "ConvertInputToNv12 / crop and NV12-NV21 normalize",
        inputFrameIndex, m_outputFrameCount.load(std::memory_order_acquire));
    const bool inputConverted = ConvertInputToNv12(input, normalizedInput, error);
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    if (!inputConverted) {
        FailRuntime("Receive/ConvertInput", error, E_FAIL);
        return E_FAIL;
    }
    m_diagInputNormalizedFrames.fetch_add(1, std::memory_order_acq_rel);
    if (m_settings.saveFirstInputBmp && !m_firstInputBmpAttempted) {
        m_firstInputBmpAttempted = true;
        std::wstring previewPath;
        std::string previewError;
        if (WriteNv12PreviewBmp(normalizedInput.data(), m_width, m_height, previewPath, previewError))
            DirectMotionLogW("INFO", "Receive/PreviewDump", L"Saved a BMP preview of the first normalized input frame to: " + previewPath);
        else DirectMotionLog("WARN", "Receive/PreviewDump", previewError);
    }

    // Do not spend fallback/engine-reset work on a sample whose generation was
    // retired while input layout conversion and NV12 normalization were running.
    if (!isCurrentOrInFlightInitialStage())
        return rejectRetiredReceive("seek fallback admission");

    // During seek fallback, reserve one immutable NV12 allocation shared by both
    // SourceBootstrapQueue and the delivery lane. Its global byte charge remains in
    // force until BOTH consumers release it, including after staging has drained.
    const int64_t publishedSourceFrameIndex = m_nextInputPts;
    bool sourceBootstrapActive =
        m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration);
    bool fallbackWasActive =
        m_seekFallbackActivePublished.load(std::memory_order_acquire);
    std::shared_ptr<const std::vector<uint8_t>> sharedSourcePayload;
    if (sourceBootstrapActive || fallbackWasActive) {
        sharedSourcePayload = m_sourceBootstrapQueue.CreatePayload(normalizedInput);
        if (!sharedSourcePayload) {
            // The fallback may complete its PTS handoff while this sample is being
            // normalized. Re-read both owners before treating a rejected allocation
            // as an error: if neither still needs shared storage, continue with the
            // Receive-owned vector on the ordinary source-ingest path.
            if (!isCurrentOrInFlightInitialStage())
                return rejectRetiredReceive("shared source payload allocation");
            sourceBootstrapActive =
                m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration);
            fallbackWasActive = m_seekFallbackActivePublished.load(std::memory_order_acquire);
            if (sourceBootstrapActive || fallbackWasActive) {
                FailRuntime("Receive/SharedSourcePayload",
                    "Could not create the bounded shared NV12 payload admitted for source fallback/bootstrap.",
                    E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
        }
    }

    HRESULT fallbackQueueHr = S_FALSE;
    // The published mirror is authoritative for the fast-path decision only:
    // false means there is no fallback work to perform, while true sends the
    // request through QueueSeekFallbackForDelivery(), which rechecks the full
    // mutex-owned state before constructing or enqueueing a packet.
    // NewSegment asserts m_segmentTransition before retiring the generation and
    // clears it only after ArmSeekFallback. A Receive already in flight when that
    // transition begins still carries the retired delivery generation; the helper
    // would reject it at its generation check, so skipping an inactive fallback is
    // equivalent and cannot suppress a valid new-generation fallback. Locally
    // detected timestamp/sample discontinuities arm the fallback in this same
    // Receive callback before reaching this gate.
    if (fallbackWasActive) {
        const bool traceFallbackAdmission = DirectShowFullTraceEnabled();
        const auto fallbackEnqueueStarted = traceFallbackAdmission
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        fallbackQueueHr = sharedSourcePayload
            ? QueueSeekFallbackForDelivery(sharedSourcePayload,
                publishedSourceFrameIndex, inputPreroll, deliveryGeneration)
            : QueueSeekFallbackForDelivery(normalizedInput,
                publishedSourceFrameIndex, inputPreroll, deliveryGeneration);
        if (fallbackQueueHr == S_OK && traceFallbackAdmission) {
            LogInfo("SeekFallback/Admission", "sourceFrame=" + std::to_string(publishedSourceFrameIndex) +
                ", enqueueCallMs=" + std::to_string(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - fallbackEnqueueStarted).count()) +
                ", engineInitializedBeforeFallback=0, sourceCachePublishedBeforeFallback=0.");
        }
    }
    if (!isCurrentOrInFlightInitialStage())
        return rejectRetiredReceive("deferred engine reset");

    // During a post-seek rebuild, do not make the decoder wait for old-engine
    // quiescence or new graph construction. Transfer the shared payload to the
    // bounded bootstrap queue and return; its dedicated worker will complete the
    // deferred reset, build the graph and publish staged frames in source order.
    // The fallback lane already owns another reference to the SAME NV12 storage.
    if (sourceBootstrapActive) {
        const DirectMotionSourceBootstrap::PushResult stageResult =
            m_sourceBootstrapQueue.TryPush(deliveryGeneration, publishedSourceFrameIndex,
                sharedSourcePayload, false);
        if (stageResult == DirectMotionSourceBootstrap::PushResult::Queued) {
            ++m_nextInputPts;
            NotifySourceBootstrapWorker();
            LogInfo("Receive/SourceBootstrap", "Transferred normalized source frame to the decoupled bootstrap worker. "
                "generation=" + std::to_string(deliveryGeneration) +
                ", sourceFrame=" + std::to_string(publishedSourceFrameIndex) +
                ", stagedFrames=" + std::to_string(m_sourceBootstrapQueue.QueuedFrames()) +
                ", stagingBytes=" + std::to_string(m_sourceBootstrapQueue.BytesOutstanding()) +
                ", byteBudget=" + std::to_string(m_sourceBootstrapQueue.ByteBudget()) +
                ", sharedFallbackPayload=" + std::to_string(fallbackQueueHr == S_OK ? 1 : 0) +
                ", initialPreSegmentPreroll=" + std::to_string(initialSegmentStaging ? 1 : 0) +
                "; Receive returns without waiting for deferred engine teardown or EnsureEngine.");
            return S_OK;
        }
        if (stageResult == DirectMotionSourceBootstrap::PushResult::StaleGeneration ||
            stageResult == DirectMotionSourceBootstrap::PushResult::Inactive) {
            if (!IsReceiveCommitCurrent(deliveryGeneration))
                return rejectRetiredReceive("source bootstrap staging");
            // Inactive means the bootstrap worker drained the queue between the
            // fast-path check and admission. The graph is ready, so retain the
            // established direct publication path for this sample.
        } else {
            FailRuntime("Receive/SourceBootstrap",
                stageResult == DirectMotionSourceBootstrap::PushResult::Full
                    ? "The bounded source bootstrap queue rejected a frame despite receive-admission backpressure."
                    : "The bounded source bootstrap queue could not take ownership of the normalized source frame.",
                stageResult == DirectMotionSourceBootstrap::PushResult::Full ? E_OUTOFMEMORY : E_FAIL);
            return stageResult == DirectMotionSourceBootstrap::PushResult::Full ? E_OUTOFMEMORY : E_FAIL;
        }
    }

    // Deferred teardown is deliberately independent of fallback admission. Even
    // when the inactive fast path skips QueueSeekFallbackForDelivery(), an old
    // engine generation must still pass its quiescence barrier before EnsureEngine.
    if (DirectMotionSeekReset::IsAwaitingSegment(m_seekResetState)) {
        LogInfo("Receive/PreSegmentHold", "A decoded sample arrived after EndFlush but before NewSegment. It was not allowed to initialize an engine for the intermediate generation; waiting for the final segment callback.");
        return S_OK;
    }
    if (m_seekResetState.deferredResetPending && !CompleteDeferredSeekEngineReset()) return E_ABORT;

    // Completing the old graph's quiescence barrier can take time. Recheck after
    // it, so a second seek/flush cannot cause this retired sample to build a graph.
    if (!IsReceiveCommitCurrent(deliveryGeneration))
        return rejectRetiredReceive("EnsureEngine");

    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Backend,
        "EnsureEngine / initialize AviSynth+ and MVTools2 graph after early seek-fallback admission",
        inputFrameIndex, m_outputFrameCount.load(std::memory_order_acquire));
    const auto ensureEngineStarted = std::chrono::steady_clock::now();
    const bool engineReady = EnsureEngine(error, deliveryGeneration);
    const double ensureEngineElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - ensureEngineStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    if (!engineReady) {
        if (!IsReceiveCommitCurrent(deliveryGeneration))
            return rejectRetiredReceive("PushSourceNv12");
        FailRuntime("Receive/EnsureEngine", error);
        return E_FAIL;
    }
    if (!IsReceiveCommitCurrent(deliveryGeneration))
        return rejectRetiredReceive("PushSourceNv12");
    if (DirectShowFullTraceEnabled() && ensureEngineElapsedMs >= 1.0) {
        LogInfo("SeekFallback/EngineWarmup", "EnsureEngine completed after early fallback admission; elapsedMs=" +
            std::to_string(ensureEngineElapsedMs) + ", sourceFrame=" +
            std::to_string(publishedSourceFrameIndex) + ".");
    }

    // Publish directly into the shared AviSynth InputClip. This must remain independent
    // from the processing worker: if Prefetch waits for a future source index, Receive is
    // the producer that makes that index available. Only GetFrame/output synthesis moved
    // to the processing worker; input normalization and source-cache publication remain here.
    const auto ingestStarted = std::chrono::steady_clock::now();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::SourceIngest,
        "PushSourceNv12 / publish decoded source frame",
        inputFrameIndex, m_outputFrameCount.load(std::memory_order_acquire));
    if (!IsReceiveCommitCurrent(deliveryGeneration)) {
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
        return rejectRetiredReceive("source-cache publication");
    }
    const uint8_t* sourcePayloadData = sharedSourcePayload
        ? sharedSourcePayload->data() : normalizedInput.data();
    const size_t sourcePayloadBytes = sharedSourcePayload
        ? sharedSourcePayload->size() : normalizedInput.size();
    const DirectMotionSourceCache::PushResult pushResult = m_engine->PushSourceNv12(
        sourcePayloadData, m_width, publishedSourceFrameIndex, error,
        this, [](void* context) noexcept {
            static_cast<CDirectMotion*>(context)->NotifyProcessingWorker();
        });
    const double ingestElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - ingestStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    m_lastPushMs.store(ingestElapsedMs, std::memory_order_release);
    const uint64_t ingestSamples = m_pushTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
    double ingestAverage = m_avgPushMs.load(std::memory_order_acquire);
    if (ingestAverage < 0.0) ingestAverage = ingestElapsedMs;
    else ingestAverage += (ingestElapsedMs - ingestAverage) / static_cast<double>(ingestSamples > 60 ? 60 : ingestSamples);
    m_avgPushMs.store(ingestAverage, std::memory_order_release);
    if (traceInput) LogInfo("Receive/SourceIngest", "input=" + std::to_string(inputFrameIndex) +
        ", generation=" + std::to_string(deliveryGeneration) +
        ", bytes=" + std::to_string(sourcePayloadBytes) + ", elapsedMs=" + std::to_string(ingestElapsedMs) +
        ", logicalSourcePts=" + std::to_string(m_nextInputPts) + ", hasTime=" + std::to_string(hasInputTime ? 1 : 0) +
        ", inputStart100ns=" + std::to_string(hasInputTime ? inputStart : 0) +
        ", inputEnd100ns=" + std::to_string(hasInputStopTime ? inputEnd : 0) +
        ", inputStopTimeAvailable=" + std::to_string(hasInputStopTime ? 1 : 0) +
        ", segmentStart100ns=" + std::to_string(m_currentSegmentMetadataValid ? m_currentSegmentStart : 0) +
        ", mediaTimeHr=" + std::to_string(static_cast<unsigned long>(inputMediaTimeHr)) +
        ", hasMediaTime=" + std::to_string(hasInputMediaTime ? 1 : 0) +
        ", mediaStart=" + std::to_string(hasInputMediaTime ? inputMediaStart : 0) +
        ", mediaEnd=" + std::to_string(hasInputMediaTime ? inputMediaEnd : 0) +
        ", sparseFingerprint=0x" + [&]() { std::ostringstream ss; ss << std::hex << SparseNv12Fingerprint(sourcePayloadData, m_width, m_height); return ss.str(); }());
    if (pushResult != DirectMotionSourceCache::PushResult::Published) {
        if (pushResult == DirectMotionSourceCache::PushResult::Interrupted &&
            DirectMotionSourceCache::IsExpectedLifecycleInterruption(
                IsGraphTransitioning(), IsPauseTransitionPending(),
                m_asyncPipeline.IsCurrentGeneration(deliveryGeneration))) {
            LogInfo("Receive/SourceIngest",
                "Source publication was cancelled by an expected Pause/Stop/Flush/seek generation transition; "
                "the in-flight sample is discarded without marking the runtime failed. Reason=" + error);
            return S_OK;
        }
        const HRESULT pushFailure = pushResult == DirectMotionSourceCache::PushResult::Interrupted
            ? E_ABORT : E_FAIL;
        FailRuntime("Receive/PushSourceNv12", error, pushFailure);
        return pushFailure;
    }
    m_diagInputPublishedFrames.fetch_add(1, std::memory_order_acq_rel);
    ++m_nextInputPts;

    // Input publication ends here. The processing worker independently notices the
    // new source-cache tail, evaluates bootstrap/MBlockFps when enough look-ahead exists,
    // and forwards generated frames to the delivery worker. Receive does not call GetFrame,
    // wait for an output acknowledgement, or hold the renderer's delivery lock.
    NotifyProcessingWorker();
    if (((inputFrameIndex + 1) % 15) == 0 || inputFrameIndex < 2) RefreshTrayRuntimeInfo();
    return S_OK;
}

HRESULT CDirectMotion::DeliverInterpolated(AsyncOutputFrame& packet, int64_t outputPts) {
    std::lock_guard<std::recursive_mutex> deliveryLock(m_deliveryMutex);
    if (packet.generation == 0 || !m_asyncPipeline.IsCurrentGeneration(packet.generation) ||
        IsGraphTransitioning() || (!packet.terminalDrain && IsPauseTransitionPending())) return E_ABORT;
    if (!m_pOutput || !m_pOutput->IsConnected()) return VFW_E_NOT_CONNECTED;
    const size_t expectedNv12Bytes = static_cast<size_t>(m_width) * m_height * 3 / 2;
    if (!packet.transitionBlendApplied && packet.transitionFromNv12 &&
        !packet.seekFallback && packet.transitionBlendFrames > 0) {
        const auto& sourceNv12 = *packet.transitionFromNv12;
        if (sourceNv12.size() == expectedNv12Bytes &&
            packet.nv12.size() == expectedNv12Bytes &&
            DirectMotionSeekTransition::BlendNv12InPlace(
                sourceNv12.data(), packet.nv12.data(), packet.nv12.size(),
                packet.transitionBlendIndex, packet.transitionBlendFrames)) {
            packet.transitionBlendApplied = true;
            if (packet.transitionBlendIndex == 0) {
                LogInfo("SeekFallback/Blend", "Started the bounded NV12 visual handoff from the last decoded source image to MVTools. generation=" +
                    std::to_string(packet.generation) + ", blendFrames=" +
                    std::to_string(packet.transitionBlendFrames) +
                    ", firstBlendWeight=1/" + std::to_string(packet.transitionBlendFrames) +
                    "; blending runs once in the dequeued DeliveryWorker packet, without a new queue or frame copy.");
            }
            if (packet.transitionBlendIndex + 1 == packet.transitionBlendFrames) {
                LogInfo("SeekFallback/BlendComplete", "Completed the source-to-MVTools visual handoff. generation=" +
                    std::to_string(packet.generation) + ", blendFrames=" +
                    std::to_string(packet.transitionBlendFrames) + ".");
            }
        } else {
            packet.transitionBlendApplied = true;
            DirectMotionLog("WARN", "SeekFallback/Blend",
                "Skipped the optional visual handoff because the retained source image and interpolated NV12 payload did not match the negotiated visible frame size.");
        }
    }
    const std::vector<uint8_t>& packetNv12 = packet.sharedNv12
        ? *packet.sharedNv12 : packet.nv12;
    if (packetNv12.size() != expectedNv12Bytes) {
        LogError("DeliverInterpolated", "Output frame payload size mismatch: got=" +
            std::to_string(packetNv12.size()) + ", expected=" +
            std::to_string(static_cast<size_t>(m_width) * m_height * 3 / 2));
        return E_INVALIDARG;
    }

    // Called only by DeliveryWorkerMain for synthesized output. Keep the normal
    // blocking DirectShow allocator contract: this worker is the single downstream
    // owner, and Stop/flush must propagate downstream to interrupt allocator waits.
    const bool traceThisFrame = DirectShowFullTraceEnabled();
    const unsigned long long allocatorId = traceThisFrame ? ++g_allocatorSequence : 0;
    if (traceThisFrame) LogInfo("Output/Allocator", "WAIT_BEGIN operation=GetDeliveryBuffer, waitId=" +
        std::to_string(allocatorId) + ", output=" + std::to_string(m_outputFrameCount + 1) +
        ", pts=" + std::to_string(outputPts) +
        ", frameKind=" + std::string(packet.seekFallback ? "seek-fallback" : "interpolated") +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    IMediaSample* output = nullptr;
    // Normal DirectShow allocator back-pressure is isolated on this delivery worker.
    // Stop/flush must propagate downstream before waiting on the Receive lock.
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Allocator,
        "GetDeliveryBuffer / wait for free renderer sample",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    const auto allocatorStarted = std::chrono::steady_clock::now();
    HRESULT hr = m_pOutput->GetDeliveryBuffer(&output, nullptr, nullptr, 0);
    const double allocatorElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - allocatorStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    m_lastAllocatorWaitMs = allocatorElapsedMs;
    if (traceThisFrame) LogInfo("Output/Allocator", "WAIT_END operation=GetDeliveryBuffer, waitId=" +
        std::to_string(allocatorId) + ", output=" + std::to_string(m_outputFrameCount + 1) +
        ", pts=" + std::to_string(outputPts) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", elapsedMs=" + std::to_string(allocatorElapsedMs) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
        ", sampleNull=" + std::to_string(output ? 0 : 1) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    if (!traceThisFrame && directmotion_diagnostics::ShouldWarnSlowOutputStage(allocatorElapsedMs)) {
        DirectMotionLog("WARN", "Output/Allocator",
            "Slow GetDeliveryBuffer: output=" + std::to_string(m_outputFrameCount + 1) +
            ", pts=" + std::to_string(outputPts) +
            ", waitMs=" + std::to_string(allocatorElapsedMs) +
            ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
            ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
            ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
            ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    }
    if (FAILED(hr) || !output) {
        if (IsGraphTransitioning() && hr == VFW_E_NOT_COMMITTED) {
            LogInfo("Output/Allocator", "Allocator was decommitted during Stop/flush after " +
                std::to_string(allocatorElapsedMs) + " ms; returning the shutdown HRESULT without retrying delivery.");
            return hr;
        }
        if ((IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) && hr == VFW_E_NOT_COMMITTED) {
            LogInfo("Output/Allocator", "Allocator was decommitted while paused or during a Run-to-Pause transition after " +
                std::to_string(allocatorElapsedMs) + " ms; returning the interruption so DeliveryWorkerMain can retain this exact packet until Run recommits downstream delivery.");
            return hr;
        }
        LogError("Output/Allocator", "GetDeliveryBuffer failed after " + std::to_string(allocatorElapsedMs) +
            " ms. HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    bool allocatorSuppliedDynamicType = false;
    hr = ApplyAllocatorMediaType(output, "Output/AllocatorMediaType", traceThisFrame,
        &allocatorSuppliedDynamicType);
    if (FAILED(hr)) {
        output->Release();
        return hr;
    }
    if (traceThisFrame) LogInfo("Output/Allocator", "Allocator returned sample after " +
        std::to_string(allocatorElapsedMs) + " ms. Size=" + std::to_string(output->GetSize()) +
        ", allocatorDynamicMediaType=" + std::to_string(allocatorSuppliedDynamicType ? 1 : 0) +
        ", selectedStridePixels=" + std::to_string(m_outputStride) +
        ", requiredPayloadBytes=" + std::to_string(m_outputBufferBytes));

    BYTE* destination = nullptr;
    hr = output->GetPointer(&destination);
    if (FAILED(hr) || !destination) {
        output->Release();
        LogError("Output/GetPointer", "Output allocator sample has no writable pointer. HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        return FAILED(hr) ? hr : E_POINTER;
    }

    const size_t required = m_outputBufferBytes;
    if (static_cast<size_t>(output->GetSize()) < required) {
        output->Release();
        LogError("Output/BufferSize", "Output sample buffer too small. got=" + std::to_string(output->GetSize()) +
            ", required=" + std::to_string(required));
        return VFW_E_BUFFER_OVERFLOW;
    }
    std::string conversionError;
    if (!ConvertNv12ToOutput(packetNv12.data(), destination, required, conversionError)) {
        output->Release();
        LogError("Output/ConvertFromNv12", conversionError);
        return E_FAIL;
    }
    hr = output->SetActualDataLength(static_cast<LONG>(required));
    REFERENCE_TIME start = packet.streamTimeOrigin100ns;
    REFERENCE_TIME end = start + packet.targetFrameDuration100ns;
    if (SUCCEEDED(hr)) {
        const FrucRational tb{packet.outputTimeBaseNumerator, packet.outputTimeBaseDenominator};
        if (tb.num > 0 && tb.den > 0) {
            start += FrucRescaleRound(outputPts, tb, FrucRational{1, 10000000});
        } else {
            DirectMotionLog("WARN", "Output/Timestamp", "Interpolation backend output time base is invalid; timestamp anchored to segment start.");
        }
        end = start + packet.targetFrameDuration100ns;
        hr = output->SetTime(&start, &end);
    }
    if (SUCCEEDED(hr)) hr = output->SetSyncPoint(TRUE);
    // IMediaSample::IsPreroll means the sample must not be displayed. A source sample's
    // preroll bit cannot be propagated to every later interpolated output generated
    // while processing it. The bootstrap is explicitly hidden; subsequent output is
    // hidden only while it precedes the first non-preroll source boundary, in either
    // stream-time units, or output-frame-index units when the first visible sample is untimed.
    const bool beforeVisibleBoundary = packet.prerollBoundaryTimeValid
        ? start < packet.prerollBoundaryTime100ns
        : (packet.prerollBoundaryOutputPtsValid && outputPts < packet.prerollBoundaryOutputPts);
    const bool terminalPausePreroll = packet.terminalDrain &&
        (IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire));
    const bool outputPreroll = packet.explicitPreroll || start < 0 || beforeVisibleBoundary || terminalPausePreroll;
    if (SUCCEEDED(hr)) hr = output->SetPreroll(outputPreroll ? TRUE : FALSE);
    if (traceThisFrame && outputPreroll) {
        LogInfo("Output/Preroll", "Suppressing output before the segment's visible boundary: pts=" +
            std::to_string(outputPts) + ", start100ns=" + std::to_string(start) +
            ", boundaryMode=" + std::string(packet.prerollBoundaryTimeValid ? "stream-time" :
                (packet.prerollBoundaryOutputPtsValid ? "output-frame-index" : "unavailable")) +
            ", boundary100ns=" + std::to_string(packet.prerollBoundaryTime100ns) +
            ", frameBoundaryValid=" + std::to_string(packet.prerollBoundaryOutputPtsValid ? 1 : 0) +
            ", boundaryOutputPts=" + std::to_string(packet.prerollBoundaryOutputPts) +
            ", explicitBootstrap=" + std::to_string(packet.explicitPreroll ? 1 : 0));
    }
    if (SUCCEEDED(hr)) hr = output->SetDiscontinuity(m_firstOutput.load(std::memory_order_acquire) ? TRUE : FALSE);
    if (FAILED(hr)) {
        output->Release();
        LogError("Output/Metadata", "Could not set output sample metadata. HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }

    const DWORD appliedTypeSpecificFlags = packet.typeSpecificFlags;
    bool sample2OutputFlagsSet = false;
    IMediaSample2* outputSample2 = nullptr;
    const HRESULT outputSample2Hr = output->QueryInterface(IID_IMediaSample2,
        reinterpret_cast<void**>(&outputSample2));
    if (SUCCEEDED(outputSample2Hr) && outputSample2) {
        AM_SAMPLE2_PROPERTIES outputProps{};
        const long propertyBytes = FIELD_OFFSET(AM_SAMPLE2_PROPERTIES, dwStreamId);
        const HRESULT getPropsHr = outputSample2->GetProperties(propertyBytes,
            reinterpret_cast<PBYTE>(&outputProps));
        if (SUCCEEDED(getPropsHr)) {
            // Assign instead of OR: allocator samples are recycled and must not
            // retain field/deinterlace flags from an earlier output sample.
            outputProps.dwTypeSpecificFlags = appliedTypeSpecificFlags;
            outputProps.lActual = static_cast<LONG>(required);
            outputProps.cbData = static_cast<DWORD>(propertyBytes);
            const HRESULT setPropsHr = outputSample2->SetProperties(propertyBytes,
                reinterpret_cast<PBYTE>(&outputProps));
            sample2OutputFlagsSet = SUCCEEDED(setPropsHr);
            if (!sample2OutputFlagsSet && traceThisFrame) {
                LogInfo("Output/SampleFlags", "Could not assign interpolated IMediaSample2 type-specific flags; HRESULT=" +
                    std::to_string(static_cast<unsigned long>(setPropsHr)));
            }
        } else if (traceThisFrame) {
            LogInfo("Output/SampleFlags", "Could not read recycled output sample properties; HRESULT=" +
                std::to_string(static_cast<unsigned long>(getPropsHr)));
        }
        outputSample2->Release();
    } else if (traceThisFrame) {
        LogInfo("Output/SampleFlags", "Downstream allocator sample does not expose IMediaSample2; per-sample type-specific flags cannot be set.");
    }

    // Consume compact audit budget only after allocation, conversion and timestamps succeeded;
    // an allocator failure must not burn one of the few useful frame fingerprints.
    const bool auditThisFrame = !DirectShowFullTraceEnabled() &&
        DirectShowTraceEnabled() && ConsumeTraceBudget(m_stepAuditOutputSamplesRemaining);
    const bool traceDeliveredProgress = !traceThisFrame && ((m_outputFrameCount + 1) % 120) == 0;

    if (traceThisFrame || auditThisFrame) {
        LogInfo("Output/PixelAudit", "frame=" + std::to_string(m_outputFrameCount + 1) +
            ", pts=" + std::to_string(outputPts) +
            ", sparseFingerprint=0x" + [&]() { std::ostringstream ss; ss << std::hex << SparseNv12Fingerprint(packetNv12.data(), m_width, m_height); return ss.str(); }() +
            ", outputLinesizeY=" + std::to_string(packet.outputLineSizeY) +
            ", outputLinesizeUV=" + std::to_string(packet.outputLineSizeUV) +
            ", visibleWidth=" + std::to_string(m_width) +
            ", visibleHeight=" + std::to_string(m_height) +
            ", outputSurface=" + std::to_string(m_outputStride) + "x" + std::to_string(m_outputSurfaceHeight) +
            ", targetOrigin=" + std::to_string(m_outputTargetLeft) + "," + std::to_string(m_outputTargetTop) +
            ", allocatorStride=" + std::to_string(m_outputStride) +
            ", outputPayloadBytes=" + std::to_string(required) +
            ", allocatorSampleSize=" + std::to_string(output->GetSize()) +
            ", interpolatedTypeSpecificFlags=" + std::to_string(appliedTypeSpecificFlags) +
            ", sample2FlagsSet=" + std::to_string(sample2OutputFlagsSet ? 1 : 0) +
            ", fullFrameCompare=disabled-in-hot-path");
        LogInfo("Output/Ready", "frame=" + std::to_string(m_outputFrameCount + 1) +
            ", pts=" + std::to_string(outputPts) + ", start100ns=" + std::to_string(start) +
            ", end100ns=" + std::to_string(end) +
            ", duration100ns=" + std::to_string(end - start) +
            ", streamOrigin100ns=" + std::to_string(packet.streamTimeOrigin100ns) +
            ", segmentStart100ns=" + std::to_string(packet.segmentMetadataValid ? packet.segmentStart100ns : 0) +
            ", segmentMetadataValid=" + std::to_string(packet.segmentMetadataValid ? 1 : 0) +
            ", targetFps=" + std::to_string(packet.targetFpsNumerator) + "/" + std::to_string(packet.targetFpsDenominator) +
            ", preroll=" + std::to_string(outputPreroll ? 1 : 0) +
            ", visibleBoundaryValid=" + std::to_string(packet.prerollBoundaryTimeValid ? 1 : 0) +
            ", visibleBoundary100ns=" + std::to_string(packet.prerollBoundaryTime100ns) +
            ", visibleBoundaryOutputPtsValid=" + std::to_string(packet.prerollBoundaryOutputPtsValid ? 1 : 0) +
            ", visibleBoundaryOutputPts=" + std::to_string(packet.prerollBoundaryOutputPts));
    }
    if (traceThisFrame) {
        LogInfo("Output/Deliver", "Calling downstream Receive for frame=" +
            std::to_string(m_outputFrameCount + 1) + ", pts=" + std::to_string(outputPts));
    }
    // Recheck after allocator acquisition/conversion. A transition which arrived while
    // this worker was waiting must not leak an obsolete packet into the new segment.
    if (!m_asyncPipeline.IsCurrentGeneration(packet.generation) || IsGraphTransitioning() ||
        (!packet.terminalDrain && IsPauseTransitionPending())) {
        output->Release();
        return E_ABORT;
    }
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Downstream,
        "Deliver / downstream renderer Receive",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    const auto downstreamStarted = std::chrono::steady_clock::now();
    hr = DeliverDownstreamTracked(output, packet.terminalDrain);
    const double downstreamElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - downstreamStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    m_lastDownstreamMs.store(downstreamElapsedMs, std::memory_order_release);
    const uint64_t downstreamSamples = m_downstreamTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
    double downstreamAverage = m_avgDownstreamMs.load(std::memory_order_acquire);
    if (downstreamAverage < 0.0) downstreamAverage = downstreamElapsedMs;
    else downstreamAverage += (downstreamElapsedMs - downstreamAverage) /
        static_cast<double>(downstreamSamples > 60 ? 60 : downstreamSamples);
    m_avgDownstreamMs.store(downstreamAverage, std::memory_order_release);
    output->Release();
    // A successful but slow downstream Receive can be transient: the ordinary
    // five-second telemetry may overwrite its last-duration sample before the next
    // snapshot, while full tracing is intentionally too noisy for routine diagnosis.
    // Emit one structured event per completed slow call; never snapshot/log from
    // inside the blocking DirectShow call and never change delivery/retry behavior.
    if (SUCCEEDED(hr) && !traceThisFrame &&
        directmotion_diagnostics::ShouldWarnSlowOutputStage(downstreamElapsedMs) &&
        DirectMotionIsLoggingEnabled() && !IsGraphTransitioning() && !IsPauseTransitionPending()) {
        try {
            const AsyncPipelineSnapshot afterDownstream = m_asyncPipeline.Snapshot();
            const AsyncQueueMetrics& queue = afterDownstream.output;
            DirectMotionLog("WARN", "Output/Downstream",
                "Slow downstream Receive completed: generation=" + std::to_string(packet.generation) +
                ", sequence=" + std::to_string(packet.sequence) +
                ", output=" + std::to_string(m_outputFrameCount.load(std::memory_order_acquire) + 1) +
                ", pts=" + std::to_string(outputPts) +
                ", elapsedMs=" + std::to_string(downstreamElapsedMs) +
                ", allocatorWaitMs=" + std::to_string(allocatorElapsedMs) +
                ", frameBudgetMs=" + std::to_string(static_cast<double>(packet.targetFrameDuration100ns) / 10000.0) +
                ", targetFps=" + std::to_string(packet.targetFpsNumerator) + "/" +
                    std::to_string(packet.targetFpsDenominator) +
                ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
                ", currentGeneration=" + std::to_string(afterDownstream.generation) +
                ", deliveryBusy=" + std::to_string(afterDownstream.deliveryBusy ? 1 : 0) +
                ", outputQueue=" + std::to_string(queue.queued) + "/" + std::to_string(queue.capacity) +
                ", outputHighWater=" + std::to_string(queue.highWater) +
                ", outputEnqueued=" + std::to_string(queue.enqueued) +
                ", outputDequeued=" + std::to_string(queue.dequeued) +
                ", outputProducerWaits=" + std::to_string(queue.producerWaits) +
                ", outputProducerWaitMaxMs=" +
                    std::to_string(static_cast<double>(queue.producerWaitMaxUs) / 1000.0) +
                ", generatedFrames=" + std::to_string(m_diagOutputGeneratedFrames.load(std::memory_order_acquire)) +
                ", deliveredFramesBeforeCommit=" + std::to_string(m_diagOutputDeliveredFrames.load(std::memory_order_acquire)));
        } catch (...) {
            // Diagnostics must never turn a successful DirectShow delivery into failure.
        }
    }
    if (FAILED(hr)) {
        if (IsGraphTransitioning() || IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) {
            LogInfo("Output/Deliver", "Downstream Receive was interrupted by a graph transition after " +
                std::to_string(downstreamElapsedMs) + " ms; not classifying the cancellation as a FRUC failure.");
            return hr;
        }
        LogError("Output/Deliver", "Downstream Receive failed after " + std::to_string(downstreamElapsedMs) +
            " ms. HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }
    if (traceThisFrame) LogInfo("Output/Deliver", "Downstream Receive returned after " +
        std::to_string(downstreamElapsedMs) + " ms.");
    if (auditThisFrame) LogInfo("StepAudit/Delivered", "frame=" +
        std::to_string(m_outputFrameCount + 1) + ", pts=" + std::to_string(outputPts) +
        ", start100ns=" + std::to_string(start) + ", end100ns=" + std::to_string(end) +
        ", downstreamReceiveMs=" + std::to_string(downstreamElapsedMs) +
        ", acceptedByDownstream=1");
    if (traceDeliveredProgress) {
        LogInfo("Output/Progress", "outputFrames=" + std::to_string(m_outputFrameCount + 1) +
            ", pts=" + std::to_string(outputPts) +
            ", allocatorWaitMs=" + std::to_string(allocatorElapsedMs) +
            ", downstreamReceiveMs=" + std::to_string(downstreamElapsedMs) +
            ", sourceIngestLastMs=" + std::to_string(m_lastPushMs.load(std::memory_order_acquire)) +
            ", sourceIngestAvgMs=" + std::to_string(m_avgPushMs.load(std::memory_order_acquire)) +
            ", graphGetFrameLastMs=" + std::to_string(m_lastGraphMs.load(std::memory_order_acquire)) +
            ", graphGetFrameAvgMs=" + std::to_string(m_avgGraphMs.load(std::memory_order_acquire)));
    }

    ActiveFrucEngine::OutputFrame deliveredProgress;
    deliveredProgress.pts = outputPts;
    RecordOutputProgress(deliveredProgress, FrucRational{
        packet.outputTimeBaseNumerator, packet.outputTimeBaseDenominator });
    m_lastDeliveredOutputEnd.store(end, std::memory_order_release);
    m_lastDeliveredOutputTimeValid.store(true, std::memory_order_release);
    m_firstOutput.store(false, std::memory_order_release);
    ++m_outputFrameCount;
    // Fallback frames are visible output but are not interpolated outputs. Keep the
    // engine-delivery diagnostic truthful while the ordinary output counter/rate
    // still includes both fallback and interpolated samples.
    if (!packet.seekFallback)
        m_diagOutputDeliveredFrames.fetch_add(1, std::memory_order_acq_rel);
    if (packet.transitionBlendFrames > 0 &&
        packet.transitionBlendIndex + 1 == packet.transitionBlendFrames &&
        packet.transitionBlendApplied) {
        std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);
        if (m_seekFallbackGeneration == packet.generation && !m_seekFallbackActive &&
            m_seekTransitionState.generation == 0 &&
            !m_seekTransitionActivePublished.load(std::memory_order_acquire) &&
            m_asyncPipeline.IsCurrentGeneration(packet.generation)) {
            DirectMotionTraySetSeekHandoffState(
                DirectMotionSeekHandoffStatus::State::Idle, packet.generation);
        }
    }
    DirectMotionTrayRecordOutput(m_outputFrameCount.load(std::memory_order_acquire));
    if (!m_runtimeFailed.load(std::memory_order_acquire)) {
        // Tray telemetry is refreshed from Receive/control callbacks. Avoid reading
        // engine/settings state from this independent delivery thread.
        DirectMotionTraySetStatus(DirectMotionStatus::Active);
    }
    return S_OK;
}

bool CDirectMotion::IsPauseFrameRequestCurrent(PauseFrameGate::Epoch epoch) const noexcept {
    return m_pauseFrameGate.IsCurrent(epoch) &&
        !m_runActive.load(std::memory_order_acquire) &&
        !m_pauseHoldActive.load(std::memory_order_acquire) &&
        !IsGraphTransitioning() &&
        !m_runtimeFailed.load(std::memory_order_acquire);
}

HRESULT CDirectMotion::DeliverOriginalSample(IMediaSample* input, const char* reason,
                                              PauseFrameGate::Epoch expectedPauseEpoch) {
    std::lock_guard<std::recursive_mutex> deliveryLock(m_deliveryMutex);
    if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)) return S_FALSE;
    if (!input) return E_POINTER;
    if (!m_pOutput || !m_pOutput->IsConnected()) return VFW_E_NOT_CONNECTED;

    const char* site = reason ? reason : "DeliverOriginalSample";
    const LONG actualLength = input->GetActualDataLength();
    REFERENCE_TIME inStart = 0, inEnd = 0;
    const HRESULT inTimeHr = input->GetTime(&inStart, &inEnd);
    const bool hasTime = (inTimeHr == S_OK || inTimeHr == VFW_S_NO_STOP_TIME);
    const bool traceThisFrame = DirectShowFullTraceEnabled() || (m_passthroughFrameCount < 2) ||
        ((m_passthroughFrameCount + 1) % 120 == 0);

    if (traceThisFrame) {
        LogInfo(site, "Pass-through source sample: bytes=" + std::to_string(actualLength) +
            ", getTimeHr=" + std::to_string(static_cast<unsigned long>(inTimeHr)) +
            ", hasTime=" + std::to_string(hasTime ? 1 : 0) +
            ", start100ns=" + std::to_string(inStart) +
            ", end100ns=" + std::to_string(inEnd) +
            ", sync=" + std::to_string(input->IsSyncPoint() == S_OK ? 1 : 0) +
            ", discontinuity=" + std::to_string(input->IsDiscontinuity() == S_OK ? 1 : 0) +
            ", preroll=" + std::to_string(input->IsPreroll() == S_OK ? 1 : 0));
    }
    if (actualLength < 0) {
        LogError(site, "Pass-through source sample has negative actual data length=" + std::to_string(actualLength));
        return E_FAIL;
    }

    // CTransformFilter owns separate input/output allocators. Copy the original bytes and
    // sample metadata into a downstream-allocator sample instead of passing a decoder-owned
    // IMediaSample directly to the renderer. No pixel processing occurs on this path.
    IMediaSample* output = nullptr;
    const unsigned long long allocatorId = traceThisFrame ? ++g_allocatorSequence : 0;
    if (traceThisFrame) LogInfo(site, "WAIT_BEGIN operation=GetDeliveryBuffer, waitId=" +
        std::to_string(allocatorId) + ", passThroughFrame=" + std::to_string(m_passthroughFrameCount) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Allocator,
        "GetDeliveryBuffer / paused-frame renderer sample",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_passthroughFrameCount.load(std::memory_order_acquire));
    const auto allocatorStarted = std::chrono::steady_clock::now();
    HRESULT hr = m_pOutput->GetDeliveryBuffer(&output,
        hasTime ? &inStart : nullptr,
        (inTimeHr == S_OK) ? &inEnd : nullptr, 0);
    const double allocatorElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - allocatorStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    if (traceThisFrame) LogInfo(site, "WAIT_END operation=GetDeliveryBuffer, waitId=" +
        std::to_string(allocatorId) + ", passThroughFrame=" + std::to_string(m_passthroughFrameCount) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", elapsedMs=" + std::to_string(allocatorElapsed) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
        ", sampleNull=" + std::to_string(output ? 0 : 1) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    m_lastAllocatorWaitMs = allocatorElapsed;
    if (!traceThisFrame && allocatorElapsed >= 250.0) {
        DirectMotionLog("WARN", site,
            "Very slow pass-through GetDeliveryBuffer: frame=" + std::to_string(m_passthroughFrameCount + 1) +
            ", waitMs=" + std::to_string(allocatorElapsed) +
            ", HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) +
            ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load() ? 1 : 0) +
            ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
            ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0));
    }
    if (FAILED(hr) || !output) {
        // A non-conforming allocator may leave a sample pointer set on failure;
        // never leak that reference on stale-epoch or decommit exits.
        if (output) {
            output->Release();
            output = nullptr;
        }
        if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)) {
            LogInfo(site, "Paused-frame allocator wait completed after its lifecycle epoch was superseded; discarding the obsolete source sample.");
            return S_FALSE;
        }
        if (expectedPauseEpoch != 0 && hr == VFW_E_NOT_COMMITTED) {
            LogInfo(site, "Paused-frame allocator is decommitted; releasing this one-shot claim so a later valid pause sample may be attempted.");
            return S_FALSE;
        }
        if (IsGraphTransitioning() && hr == VFW_E_NOT_COMMITTED) {
            LogInfo(site, "Downstream allocator was decommitted during Stop/flush; returning without classifying this as a runtime failure.");
            return hr;
        }
        if ((IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) && hr == VFW_E_NOT_COMMITTED) {
            LogInfo(site, "Downstream allocator was decommitted while paused or during Run-to-Pause; dropping this transition sample without raising the tray error.");
            DirectMotionTraySetStatus(DirectMotionStatus::Standby);
            return S_OK;
        }
        LogError(site, "Downstream GetDeliveryBuffer failed after " + std::to_string(allocatorElapsed) +
            " ms; HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)) {
        output->Release();
        LogInfo(site, "Discarded paused-frame allocator sample after Run/Stop/Flush/seek superseded its epoch.");
        return S_FALSE;
    }
    bool allocatorSuppliedDynamicType = false;
    hr = ApplyAllocatorMediaType(output, site, traceThisFrame, &allocatorSuppliedDynamicType);
    if (FAILED(hr)) {
        output->Release();
        return hr;
    }

    BYTE* sourceBytes = nullptr;
    hr = input->GetPointer(&sourceBytes);
    if (FAILED(hr) || !sourceBytes) {
        output->Release();
        // This method is used only when the user explicitly selects interpolation-off.
        // In that mode, opaque/surface-backed samples may require direct delivery.
        LogInfo(site, "Explicit interpolation-off: source sample is not CPU-readable; using direct delivery. HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Downstream,
            "Deliver / downstream renderer Receive (dynamic output media type)",
            m_inputFrameCount.load(std::memory_order_acquire),
            m_passthroughFrameCount.load(std::memory_order_acquire));
        const auto directStarted = std::chrono::steady_clock::now();
        const HRESULT directHr = DeliverDownstreamTracked(input);
        const double directElapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - directStarted).count();
        DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
        if (FAILED(directHr)) {
            LogError(site, "Direct opaque-sample delivery failed after " + std::to_string(directElapsed) +
                " ms; HRESULT=" + std::to_string(static_cast<unsigned long>(directHr)));
            return directHr;
        }
        ++m_passthroughFrameCount;
        DirectMotionTrayRecordOutput(m_passthroughFrameCount.load(std::memory_order_acquire));
        if (traceThisFrame) LogInfo(site, "Direct opaque-sample delivery returned after " +
            std::to_string(directElapsed) + " ms; passThroughCount=" + std::to_string(m_passthroughFrameCount));
        return S_OK;
    }

    BYTE* destinationBytes = nullptr;
    hr = output->GetPointer(&destinationBytes);
    if (FAILED(hr) || !destinationBytes) {
        output->Release();
        LogError(site, "Downstream allocator sample has no writable pointer; HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return FAILED(hr) ? hr : E_POINTER;
    }
    if (static_cast<size_t>(actualLength) < m_inputBufferBytes) {
        output->Release();
        LogError(site, "Input sample is shorter than its negotiated semi-planar surface: actual=" +
            std::to_string(actualLength) + ", required=" + std::to_string(m_inputBufferBytes));
        return VFW_E_BUFFER_OVERFLOW;
    }
    const size_t outputPayloadBytes = m_outputBufferBytes;
    if (outputPayloadBytes == 0 || outputPayloadBytes > static_cast<size_t>(output->GetSize())) {
        const LONG capacity = output->GetSize();
        output->Release();
        LogError(site, "Downstream allocator buffer is too small for the normalized visible picture: capacity=" +
            std::to_string(capacity) + ", requiredOutputBytes=" + std::to_string(outputPayloadBytes) +
            ", visible=" + std::to_string(m_width) + "x" + std::to_string(m_height) +
            ", outputStridePixels=" + std::to_string(m_outputStride));
        return VFW_E_BUFFER_OVERFLOW;
    }
    std::vector<uint8_t> normalizedFrame;
    std::string normalizeError;
    if (!ConvertInputToNv12(sourceBytes, normalizedFrame, normalizeError) ||
        !ConvertNv12ToOutput(normalizedFrame.data(), destinationBytes,
            static_cast<size_t>(output->GetSize()), normalizeError)) {
        output->Release();
        LogError(site, "Could not normalize/copy the visible source picture into the output surface: " + normalizeError);
        return E_FAIL;
    }

    hr = output->SetActualDataLength(static_cast<LONG>(outputPayloadBytes));
    if (SUCCEEDED(hr)) {
        if (inTimeHr == S_OK) hr = output->SetTime(&inStart, &inEnd);
        else if (inTimeHr == VFW_S_NO_STOP_TIME) hr = output->SetTime(&inStart, nullptr);
        else hr = output->SetTime(nullptr, nullptr); // clear stale timestamps in reused allocator samples
    }

    LONGLONG mediaStart = 0, mediaEnd = 0;
    const HRESULT mediaTimeHr = input->GetMediaTime(&mediaStart, &mediaEnd);
    if (SUCCEEDED(hr)) {
        const HRESULT setMediaTimeHr = (mediaTimeHr == S_OK)
            ? output->SetMediaTime(&mediaStart, &mediaEnd)
            : output->SetMediaTime(nullptr, nullptr);
        // Media time is optional for video presentation; tolerate samples that don't implement it.
        if (FAILED(setMediaTimeHr) && setMediaTimeHr != E_NOTIMPL) hr = setMediaTimeHr;
    }
    if (SUCCEEDED(hr)) hr = output->SetSyncPoint(input->IsSyncPoint() == S_OK ? TRUE : FALSE);
    if (SUCCEEDED(hr)) hr = output->SetPreroll(input->IsPreroll() == S_OK ? TRUE : FALSE);
    if (SUCCEEDED(hr)) hr = output->SetDiscontinuity(input->IsDiscontinuity() == S_OK ? TRUE : FALSE);

    AM_MEDIA_TYPE* dynamicType = nullptr;
    const HRESULT dynamicTypeHr = input->GetMediaType(&dynamicType);
    const bool hasDynamicType = (dynamicTypeHr == S_OK && dynamicType != nullptr);
    if (SUCCEEDED(hr) && hasDynamicType) {
        // The dynamic input type has already been parsed by ApplyDynamicInputSampleType.
        // The output sample must keep the transform/renderer output geometry and
        // negotiated 2x/3x cadence; attaching the decoder's media type here would
        // leak input pitch/crop metadata into the output allocator.
        LogInfo(site, std::string("Normalized source dynamic media type before delivery; ") +
            (allocatorSuppliedDynamicType ? "renderer allocator output type remains authoritative." :
                "the transform's negotiated output media type remains authoritative."));
    }
    if (dynamicType) DeleteMediaType(dynamicType);
    if (FAILED(hr)) {
        output->Release();
        LogError(site, "Could not copy pass-through sample metadata; HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }

    // Match CTransformFilter::InitializeOutputSample: preserve type-specific flags
    // and all valid source sample flags. The renderer's allocator-owned buffer,
    // stream ID and media-type storage must remain those of the destination sample.
    DWORD sourceTypeSpecificFlags = 0;
    DWORD destinationTypeSpecificFlags = 0;
    DWORD sourceSampleFlags = 0;
    DWORD destinationSampleFlags = 0;
    bool sample2PropertiesCopied = false;
    IMediaSample2* inputSample2 = nullptr;
    IMediaSample2* outputSample2 = nullptr;
    const HRESULT inputSample2Hr = input->QueryInterface(IID_IMediaSample2,
        reinterpret_cast<void**>(&inputSample2));
    const HRESULT outputSample2Hr = output->QueryInterface(IID_IMediaSample2,
        reinterpret_cast<void**>(&outputSample2));
    if (SUCCEEDED(inputSample2Hr) && inputSample2 &&
        SUCCEEDED(outputSample2Hr) && outputSample2) {
        AM_SAMPLE2_PROPERTIES inputProps{};
        AM_SAMPLE2_PROPERTIES outputProps{};
        const long propertyBytes = FIELD_OFFSET(AM_SAMPLE2_PROPERTIES, dwStreamId);
        const HRESULT inputPropsHr = inputSample2->GetProperties(propertyBytes,
            reinterpret_cast<PBYTE>(&inputProps));
        const HRESULT outputPropsHr = outputSample2->GetProperties(propertyBytes,
            reinterpret_cast<PBYTE>(&outputProps));
        if (SUCCEEDED(inputPropsHr) && SUCCEEDED(outputPropsHr)) {
            sourceTypeSpecificFlags = inputProps.dwTypeSpecificFlags;
            sourceSampleFlags = inputProps.dwSampleFlags;
            outputProps.dwTypeSpecificFlags = inputProps.dwTypeSpecificFlags;
            outputProps.dwSampleFlags = (outputProps.dwSampleFlags & AM_SAMPLE_TYPECHANGED) |
                (inputProps.dwSampleFlags & ~AM_SAMPLE_TYPECHANGED);
            outputProps.lActual = static_cast<LONG>(outputPayloadBytes);
            outputProps.tStart = inputProps.tStart;
            outputProps.tStop = inputProps.tStop;
            outputProps.cbData = static_cast<DWORD>(propertyBytes);
            const HRESULT setPropsHr = outputSample2->SetProperties(propertyBytes,
                reinterpret_cast<PBYTE>(&outputProps));
            if (SUCCEEDED(setPropsHr)) {
                sample2PropertiesCopied = true;
                destinationTypeSpecificFlags = outputProps.dwTypeSpecificFlags;
                destinationSampleFlags = outputProps.dwSampleFlags;
            } else {
                LogInfo(site, "IMediaSample2 SetProperties failed; retained legacy IMediaSample metadata. HRESULT=" +
                    std::to_string(static_cast<unsigned long>(setPropsHr)));
            }
        } else {
            LogInfo(site, "IMediaSample2 GetProperties unavailable; retained legacy IMediaSample metadata.");
        }
    } else {
        if (inputSample2) inputSample2->Release();
        if (outputSample2) outputSample2->Release();
        inputSample2 = nullptr;
        outputSample2 = nullptr;
    }
    if (inputSample2) inputSample2->Release();
    if (outputSample2) outputSample2->Release();

    if (traceThisFrame) {
        std::string audit = "Copied original sample into downstream allocator: sourceBytes=" +
            std::to_string(actualLength) + ", destinationCapacity=" + std::to_string(output->GetSize()) +
            ", outputActualBytes=" + std::to_string(outputPayloadBytes) +
            ", copyMode=normalized-visible-rectangle-to-output-surface," +
            " sourceCropLeft=" + std::to_string(m_inputCropLeft) +
            ", sourceCropTop=" + std::to_string(m_inputCropTop) +
            ", inputStridePixels=" + std::to_string(m_inputStride) +
            ", outputStridePixels=" + std::to_string(m_outputStride) +
            ", getTimeHr=" + std::to_string(static_cast<unsigned long>(inTimeHr)) +
            ", dynamicMediaType=" + std::to_string(hasDynamicType ? 1 : 0) +
            ", allocatorWaitMs=" + std::to_string(allocatorElapsed) +
            ", sample2PropertiesCopied=" + std::to_string(sample2PropertiesCopied ? 1 : 0) +
            ", sourceTypeSpecificFlags=" + std::to_string(sourceTypeSpecificFlags) +
            ", destinationTypeSpecificFlags=" + std::to_string(destinationTypeSpecificFlags) +
            ", sourceSampleFlags=" + std::to_string(sourceSampleFlags) +
            ", destinationSampleFlags=" + std::to_string(destinationSampleFlags);
        LogInfo(site, audit);
    }

    if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)) {
        output->Release();
        LogInfo(site, "Discarded the copied paused-frame sample before downstream Deliver because its lifecycle epoch is obsolete.");
        return S_FALSE;
    }
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Downstream,
        "Deliver / downstream renderer Receive (paused-frame copy)",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_passthroughFrameCount.load(std::memory_order_acquire));
    const auto downstreamStarted = std::chrono::steady_clock::now();
    hr = DeliverDownstreamTracked(output);
    const double downstreamElapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - downstreamStarted).count();
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    m_lastDownstreamMs.store(downstreamElapsed, std::memory_order_release);
    const uint64_t downstreamSamples = m_downstreamTimingSamples.fetch_add(1, std::memory_order_acq_rel) + 1;
    double downstreamAverage = m_avgDownstreamMs.load(std::memory_order_acquire);
    if (downstreamAverage < 0.0) downstreamAverage = downstreamElapsed;
    else downstreamAverage += (downstreamElapsed - downstreamAverage) /
        static_cast<double>(downstreamSamples > 60 ? 60 : downstreamSamples);
    m_avgDownstreamMs.store(downstreamAverage, std::memory_order_release);
    output->Release();
    if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)) {
        LogInfo(site, "Paused-frame downstream call completed after Run/Stop/Flush/seek superseded its epoch; ignoring the obsolete completion.");
        return S_FALSE;
    }
    if (FAILED(hr)) {
        if (IsGraphTransitioning() || IsPauseTransitionPending() || !m_runActive.load(std::memory_order_acquire)) {
            LogInfo(site, "Downstream Receive interrupted by a graph transition after " +
                std::to_string(downstreamElapsed) + " ms; cancellation is not a runtime failure.");
            return hr;
        }
        LogError(site, "Downstream Receive rejected copied pass-through sample after " +
            std::to_string(downstreamElapsed) + " ms; HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
        return hr;
    }
    ++m_passthroughFrameCount;
    DirectMotionTrayRecordOutput(m_passthroughFrameCount.load(std::memory_order_acquire));
    if ((m_passthroughFrameCount % 15) == 0 || m_passthroughFrameCount <= 2) RefreshTrayRuntimeInfo();
    if (traceThisFrame) LogInfo(site, "Downstream Receive accepted copied pass-through sample after " +
        std::to_string(downstreamElapsed) + " ms; passThroughCount=" + std::to_string(m_passthroughFrameCount));
    return S_OK;
}


void CDirectMotion::FailRuntime(const char* where, const std::string& reason, HRESULT failureHr) noexcept {
    m_pauseFrameGate.Invalidate();
    const bool alreadyFailed = m_runtimeFailed.exchange(true, std::memory_order_acq_rel);
    m_runActive.store(false, std::memory_order_release);
    m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.Cancel();
    NotifyDeliveryWorker();
    InterruptPendingSourceRequests(where ? where : "FailRuntime");
    if (alreadyFailed) return;

    try {
        LogError(where, reason + " Interpolation is aborting; runtime pass-through fallback is disabled.");
    } catch (...) {
        DirectMotionTraySetStatus(DirectMotionStatus::Error);
        DirectMotionLog("ERROR", where ? where : "RuntimeFailure", "Interpolation runtime failed; pass-through fallback is disabled.");
    }

    // Report a terminal DirectShow graph error so the player does not silently keep
    // audio alive while this video transform has stopped. CBaseFilter::NotifyEvent owns
    // the event-sink path; Receive also propagates the failure where possible.
    try {
        const HRESULT eventHr = NotifyEvent(EC_ERRORABORT,
            static_cast<LONG_PTR>(FAILED(failureHr) ? failureHr : E_FAIL), 0);
        if (FAILED(eventHr)) {
            DirectMotionLog("WARN", "RuntimeFailure", "NotifyEvent(EC_ERRORABORT) failed, HRESULT=" +
                std::to_string(static_cast<unsigned long>(eventHr)));
        } else if (eventHr == S_FALSE) {
            DirectMotionLog("WARN", "RuntimeFailure", "The filter graph has no event sink; failure remains visible through Receive/Run HRESULTs and diagnostics.");
        }
    } catch (...) {
        DirectMotionLog("WARN", "RuntimeFailure", "Could not notify the graph about the interpolation failure.");
    }
    // LogError publishes the failure status directly. Avoid a full tray snapshot here:
    // FailRuntime may be called by ProcessingWorkerMain while control threads reset the engine.
}

HRESULT CDirectMotion::EndOfStream() {
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::ReceiveLock,
        "EndOfStream / waiting to acquire DirectShow streaming lock",
        m_inputFrameCount.load(std::memory_order_acquire),
        m_outputFrameCount.load(std::memory_order_acquire));
    CAutoLock receiveLock(&m_csReceive);
    DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Idle);
    const AsyncPipelineState::Generation generation = m_asyncPipeline.CurrentGeneration();
    LogInfo("EndOfStream", "End of stream received. RuntimeFailed=" +
        std::to_string(m_runtimeFailed.load() ? 1 : 0) +
        "; final source look-ahead and output drain are owned by the processing worker.");

    if (generation == 0) {
        LogError("EndOfStream/AsyncProcessing", "Cannot drain terminal output without an active pipeline generation.");
        return VFW_E_WRONG_STATE;
    }
    if (m_runtimeFailed.load(std::memory_order_acquire)) return E_FAIL;

    const HRESULT drainHr = RequestProcessingEndOfStream(generation);
    if (FAILED(drainHr)) {
        if (IsGraphTransitioning()) return S_OK;
        FailRuntime("EndOfStream/AsyncProcessing", "The processing worker could not complete the terminal MBlockFps drain; HRESULT=" +
            std::to_string(static_cast<unsigned long>(drainHr)), drainHr);
        return drainHr;
    }
    if (m_runtimeFailed.load(std::memory_order_acquire)) return E_FAIL;

    // Every terminal output was acknowledged by the delivery worker before the
    // processing worker reported completion, so downstream EOS follows the final frame.
    return CTransformFilter::EndOfStream();
}

HRESULT CDirectMotion::BeginFlush() {
    // Retire only the fallback that belongs to the generation this flush is
    // currently replacing. A delayed older callback must not clear a newer seek.
    ClearSeekFallback(m_asyncPipeline.CurrentGeneration());
    m_pauseFrameGate.Invalidate();
    const unsigned long long flushId = ++g_flushSequence;
    m_currentFlushId.store(flushId);
    const auto started = std::chrono::steady_clock::now();
    m_flushTransition.Acquire();
    // Wake any upstream Receive waiting for staging capacity and retire the prior
    // staging epoch before the new seek generation is established below.
    m_sourceBootstrapQueue.Cancel();
    m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.Cancel();
    NotifyDeliveryWorker();
    InterruptPendingSourceRequests("BeginFlush/seek");
    LogInfo("BeginFlush", "ENTER flushId=" + std::to_string(flushId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", transitionOwners=" + std::to_string(m_flushTransition.Count()) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0) +
        "; propagating downstream before waiting for the sample lock.");
    const unsigned long long flushStackIndex = DirectShowTraceEnabled()
        ? g_stepAuditFlushStackCount.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
    if (DirectShowFullTraceEnabled() ||
        (DirectShowTraceEnabled() && flushStackIndex <= kStepAuditMaxFlushStacks))
        LogTransitionCaller("BeginFlush", flushId);
    // Critical ordering: downstream BeginFlush must happen before we wait for Receive's lock.
    // If Receive is blocked inside downstream Deliver, the flush is what releases it.
    const auto baseStarted = std::chrono::steady_clock::now();
    const HRESULT baseHr = CTransformFilter::BeginFlush();
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    LogInfo("BeginFlush/Base", "flushId=" + std::to_string(flushId) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(baseHr)) +
        ", elapsedMs=" + std::to_string(baseElapsedMs) + "; now clearing FRUC state.");
    // The downstream flush above is deliberately first: it releases a delivery worker
    // blocked in GetDeliveryBuffer/Deliver. Advancing the queue generation then wakes
    // any producer blocked by output capacity and discards undelivered obsolete packets.
    AsyncPipelineState::Generation previousDeliveryGeneration = 0;
    AsyncPipelineState::Generation nextDeliveryGeneration = 0;
    {
        std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
        previousDeliveryGeneration = m_asyncPipeline.CurrentGeneration();
        nextDeliveryGeneration = m_asyncPipeline.BeginGeneration();
    }
    LogInfo("BeginFlush/AsyncDelivery", "Invalidated queued output after downstream BeginFlush. oldGeneration=" +
        std::to_string(previousDeliveryGeneration) + ", newGeneration=" +
        std::to_string(nextDeliveryGeneration));
    const auto resetStarted = std::chrono::steady_clock::now();
    {
        CAutoLock receiveLock(&m_csReceive);
        // A real flush supersedes the cold-start pre-segment staging window.
        // Its samples belong to a seek generation and must follow the ordinary
        // deferred-reset path rather than being adopted as initial preroll.
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
        // Receive owns graph/timestamp state under m_csReceive. The delivery worker reads
        // immutable packet snapshots and updates only atomic delivery telemetry.
        if (m_engine) m_engine->InterruptSourceWaits();
        LogInfo("BeginFlush/StreamState", "flushId=" + std::to_string(flushId) +
            ", inputFramesBeforeReset=" + std::to_string(m_inputFrameCount.load(std::memory_order_acquire)) +
            ", outputFramesBeforeReset=" + std::to_string(m_outputFrameCount.load(std::memory_order_acquire)) +
            ", lastInputTimestampValid=" + std::to_string(m_lastInputTimestampValid ? 1 : 0) +
            ", lastInputStopTimeValid=" + std::to_string(m_lastInputSampleStopTimeValid ? 1 : 0) +
            ", lastInputStart100ns=" + std::to_string(m_lastInputSampleStart) +
            ", lastInputEndOrStart100ns=" + std::to_string(m_lastInputSampleEnd) +
            ", lastOutputTimestampValid=" + std::to_string(m_lastDeliveredOutputTimeValid.load(std::memory_order_acquire) ? 1 : 0) +
            ", lastOutputEnd100ns=" + std::to_string(m_lastDeliveredOutputEnd.load(std::memory_order_acquire)) +
            ", bootstrapQueued=" + std::to_string(m_bootstrapQueueState.HasBeenEnqueued() ? 1 : 0) +
            ", runActive=" + std::to_string(m_runActive.load(std::memory_order_acquire) ? 1 : 0) +
            ", pauseTransition=" + std::to_string(m_pauseTransition.load(std::memory_order_acquire) ? 1 : 0) +
            "; captured before the flush resets the graph generation; output delivery has its own worker.");
        if (!m_stopRequested.load(std::memory_order_acquire) && m_currentSegmentMetadataValid) {
            if (!BeginDeferredSeekEngineReset(nextDeliveryGeneration, true)) return E_ABORT;
            LogInfo("BeginFlush/Cache", "Requested deferred engine teardown and held source-bootstrap consumption until NewSegment binds the final segment generation.");
        } else {
            ResetEngine();
            const auto synchronousGeneration = m_asyncPipeline.CurrentGeneration();
            (void)DirectMotionSeekReset::BeginSynchronousResetAwaitingSegment(
                m_seekResetState, synchronousGeneration);
            LogInfo("BeginFlush/Cache", "Invalidated interpolation synchronously for a non-seek lifecycle flush; awaiting its matching NewSegment callback.");
        }
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_streamTimeOriginSet = false;
        }
    }
    const double resetElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - resetStarted).count();
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    LogInfo("BeginFlush", "EXIT flushId=" + std::to_string(flushId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(baseHr)) +
        ", resetAndLockElapsedMs=" + std::to_string(resetElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()));
    return baseHr;
}

HRESULT CDirectMotion::EndFlush() {
    AsyncPipelineState::Generation resetGeneration = m_asyncPipeline.CurrentGeneration();
    const unsigned long long flushId = m_currentFlushId.load();
    const auto started = std::chrono::steady_clock::now();
    InterruptPendingSourceRequests("EndFlush/seek");
    LogInfo("EndFlush", "ENTER flushId=" + std::to_string(flushId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", transitionOwners=" + std::to_string(m_flushTransition.Count()) +
        ", stopRequested=" + std::to_string(m_stopRequested.load() ? 1 : 0));
    const auto resetStarted = std::chrono::steady_clock::now();
    {
        CAutoLock receiveLock(&m_csReceive);
        if (m_engine) m_engine->InterruptSourceWaits();
        const bool awaitingMatchingSegment = DirectMotionSeekReset::IsAwaitingSegment(m_seekResetState) &&
            m_seekResetState.generation == resetGeneration;
        if (!awaitingMatchingSegment) {
            if (!m_stopRequested.load(std::memory_order_acquire) && m_currentSegmentMetadataValid) {
                if (!BeginDeferredSeekEngineReset(resetGeneration, true)) return E_ABORT;
                LogInfo("EndFlush/Cache", "Requested deferred engine teardown and held bootstrap consumption until NewSegment binds the final segment generation.");
            } else {
                ResetEngine();
                const auto synchronousGeneration = m_asyncPipeline.CurrentGeneration();
                (void)DirectMotionSeekReset::BeginSynchronousResetAwaitingSegment(
                    m_seekResetState, synchronousGeneration);
                LogInfo("EndFlush/Cache", "No matching BeginFlush reset was recorded; invalidated the engine synchronously and await NewSegment.");
            }
        } else {
            LogInfo("EndFlush/Cache", "Reusing the generation-owned reset from BeginFlush; source-bootstrap consumer remains gated until NewSegment.");
        }
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_streamTimeOriginSet = false;
        }
    }
    const double resetElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - resetStarted).count();
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    const auto baseStarted = std::chrono::steady_clock::now();
    const HRESULT baseHr = CTransformFilter::EndFlush();
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.Cancel();
    if (!m_stopRequested.load(std::memory_order_acquire)) m_flushTransition.Release();
    LogInfo("EndFlush", "EXIT flushId=" + std::to_string(flushId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(baseHr)) +
        ", remainingTransitionOwners=" + std::to_string(m_flushTransition.Count()) +
        ", resetAndLockElapsedMs=" + std::to_string(resetElapsedMs) +
        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()) +
        "; backend will reinitialize on the next sample.");
    m_currentFlushId.store(0);
    return baseHr;
}

HRESULT CDirectMotion::NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) {
    DirectMotionLifecycle::TransitionLease segmentTransitionLease(m_segmentTransition);
    m_pauseFrameGate.Invalidate();
    // The first decoder samples may arrive before this callback. Snapshot the
    // dormant initial-stage ownership before advancing the output generation;
    // ordinary seek/flush queues are still cancelled and rebuilt below.
    const AsyncPipelineState::Generation initialStageCandidateGeneration =
        m_asyncPipeline.CurrentGeneration();
    size_t initialStagedFrameCount = m_sourceBootstrapQueue.QueuedFrames();
    const bool preparedInitialGraphWasAwaitingSegment =
        m_preparedInitialGraphAwaitingSegment.load(std::memory_order_acquire);
    bool noPreviousSegmentAtEntry = false;
    {
        std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
        noPreviousSegmentAtEntry = !m_currentSegmentMetadataValid;
    }
    // Do not wait for m_csReceive before invalidating the old output generation:
    // Receive may be waiting for source-cache admission while holding that lock.
    // These atomic/short-lock preconditions are only a preflight; NewSegment
    // revalidates the full graph/seek state after the generation fence below.
    const bool preserveInitialSegmentStaging =
        preparedInitialGraphWasAwaitingSegment && noPreviousSegmentAtEntry &&
        m_engineGeneration.load(std::memory_order_acquire) == initialStageCandidateGeneration &&
        m_sourceBootstrapQueue.IsActiveForGeneration(initialStageCandidateGeneration) &&
        !m_sourceBootstrapQueue.IsWorkerReadyForGeneration(initialStageCandidateGeneration) &&
        m_diagInputPublishedFrames.load(std::memory_order_acquire) == 0 &&
        m_outputFrameCount.load(std::memory_order_acquire) == 0 &&
        !m_flushTransition.Active() &&
        !m_stopRequested.load(std::memory_order_acquire);
    if (!preserveInitialSegmentStaging) {
        m_sourceBootstrapQueue.Cancel();
    } else {
        LogInfo("NewSegment/InitialStage", "Preserving decoder preroll samples received before the first segment callback. "
            "generation=" + std::to_string(initialStageCandidateGeneration) +
            ", stagedFrames=" + std::to_string(initialStagedFrameCount) +
            "; the bootstrap consumer remains gated until downstream NewSegment succeeds.");
    }
    m_pauseHoldActive.store(false, std::memory_order_release);
    m_pauseReceiveBackpressure.Cancel();
    const uint64_t sourceInterruptEpochBeforeSegment = m_sourceEvaluationGate
        ? m_sourceEvaluationGate->SourceInterruptEpoch() : 0;
    const uint64_t sourceInterruptEpochAfterSegment =
        InterruptPendingSourceRequests("NewSegment/seek");
    const bool ownsDownstreamFlush = !m_flushTransition.Active() &&
        !m_stopRequested.load(std::memory_order_acquire) && m_pOutput && m_pOutput->IsConnected();
    if (ownsDownstreamFlush) {
        const HRESULT beginFlushHr = m_pOutput->DeliverBeginFlush();
        if (FAILED(beginFlushHr)) {
            if (preserveInitialSegmentStaging) m_sourceBootstrapQueue.Cancel();
            segmentTransitionLease.Release();
            LogError("NewSegment/LocalFlush", "Could not release an in-flight delivery before advancing the segment; HRESULT=" +
                std::to_string(static_cast<unsigned long>(beginFlushHr)));
            return beginFlushHr;
        }
        LogInfo("NewSegment/LocalFlush", "No upstream flush was active; issued a local downstream BeginFlush to release stale queued/active delivery before the generation fence.");
    }
    AsyncPipelineState::Generation previousDeliveryGeneration = 0;
    AsyncPipelineState::Generation nextDeliveryGeneration = 0;
    {
        std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);
        previousDeliveryGeneration = m_asyncPipeline.CurrentGeneration();
        nextDeliveryGeneration = m_asyncPipeline.BeginGeneration();
    }
    LogInfo("NewSegment/AsyncDelivery", "Advanced the output queue generation for the new media segment. oldGeneration=" +
        std::to_string(previousDeliveryGeneration) + ", newGeneration=" +
        std::to_string(nextDeliveryGeneration));
    const unsigned long long segmentId = ++g_segmentSequence;
    const auto started = std::chrono::steady_clock::now();
    LogInfo("NewSegment", "ENTER segmentId=" + std::to_string(segmentId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", start100ns=" + std::to_string(start) +
        ", stop100ns=" + std::to_string(stop) + ", rate=" + std::to_string(rate) +
        ", runActive=" + std::to_string(m_runActive.load(std::memory_order_acquire) ? 1 : 0) +
        ", pauseTransitionPending=" + std::to_string(m_pauseTransition.load(std::memory_order_acquire) ? 1 : 0) +
        ", pauseCallback=" + std::to_string(m_pauseCallInProgress.load(std::memory_order_acquire) ? 1 : 0) +
        ", bootstrapQueued=" + std::to_string(m_bootstrapQueueState.HasBeenEnqueued() ? 1 : 0) +
        ", segmentTransitionOwners=" + std::to_string(m_segmentTransition.Count()) +
        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0) +
        "; resetting backend for new media segment/seek.");
    const bool fullTrace = DirectShowFullTraceEnabled();
    const unsigned auditSegmentIndex = DirectShowTraceEnabled()
        ? m_stepAuditSegmentCount.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
    const bool auditSegment = DirectShowTraceEnabled() && auditSegmentIndex <= kStepAuditMaxSegments;
    if (auditSegment || fullTrace) LogTransitionCaller("NewSegment", segmentId);
    const auto resetStarted = std::chrono::steady_clock::now();
    bool previousSegmentValid = false;
    REFERENCE_TIME previousSegmentStart = 0;
    bool adoptPreparedInitialGraph = false;
    bool reboundInitialStagedGraph = false;
    {
        CAutoLock receiveLock(&m_csReceive);
        auto tryRebindInitialSourceGeneration = [&](std::string& rebindError) {
            // The InputClip generation lock drains GetFrame itself, but the engine
            // commits m_nextOutputIndex immediately after GetFrame returns. Hold the
            // worker-state mutex across this check/rebind so no pass can be between
            // those two operations, and wait for any pre-transition pass to retire.
            std::unique_lock<std::mutex> processingStateLock(m_processingWorkerMutex);
            if (m_processingWorkerActive) {
                LogInfo("NewSegment/InitialGraph", "Waiting for the active ProcessingWorker pass to exit before checking the retained graph's engine-local pristine state.");
                m_processingWorkerChanged.wait(processingStateLock, [this]() {
                    return !m_processingWorkerActive;
                });
            }
            const bool pipelineGenerationMatches =
                m_engineGeneration.load(std::memory_order_acquire) == previousDeliveryGeneration;
            // m_inputFrameCount also includes decoded samples already owned by
            // the dormant preroll FIFO. Those samples have NOT reached InputClip;
            // only this publication counter must still be zero for rebinding.
            // The no-staging prepared-graph path separately requires input count 0.
            const bool noFilterInputPublished =
                m_diagInputPublishedFrames.load(std::memory_order_acquire) == 0;
            const bool noFilterOutputEmitted =
                m_outputFrameCount.load(std::memory_order_acquire) == 0;
            return m_engine && m_engine->TryRebindInitialSourceGeneration(
                sourceInterruptEpochBeforeSegment, sourceInterruptEpochAfterSegment,
                pipelineGenerationMatches, noFilterInputPublished,
                noFilterOutputEmitted, rebindError);
        };
        adoptPreparedInitialGraph = DirectMotionInitialSegmentGraphPolicy::CanAdoptPreparedInitialGraph(
            preparedInitialGraphWasAwaitingSegment,
            m_engine && m_engine->IsInitialized(),
            m_engineGeneration.load(std::memory_order_acquire) == previousDeliveryGeneration,
            m_inputFrameCount.load(std::memory_order_acquire) == 0 &&
                m_diagInputPublishedFrames.load(std::memory_order_acquire) == 0,
            m_outputFrameCount.load(std::memory_order_acquire) == 0,
            m_seekResetState.phase == DirectMotionSeekReset::Phase::Idle &&
                !m_seekResetState.deferredResetPending,
            !m_sourceBootstrapQueue.IsActiveForGeneration(previousDeliveryGeneration),
            !m_flushTransition.Active(),
            !m_stopRequested.load(std::memory_order_acquire));
        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            previousSegmentValid = m_currentSegmentMetadataValid;
            previousSegmentStart = m_currentSegmentStart;
            m_currentSegmentStart = start;
            m_currentSegmentStop = stop;
            m_currentSegmentRate = rate;
            m_currentSegmentMetadataValid = true;
        }
        const bool stagedInitialGraphStillEligible = preserveInitialSegmentStaging &&
            previousDeliveryGeneration == initialStageCandidateGeneration &&
            DirectMotionInitialSegmentStagingPolicy::CanAdoptStagedInitialGraph(
                preparedInitialGraphWasAwaitingSegment,
                m_engine && m_engine->IsInitialized(),
                m_engineGeneration.load(std::memory_order_acquire) == previousDeliveryGeneration,
                m_sourceBootstrapQueue.IsActiveForGeneration(previousDeliveryGeneration),
                !m_sourceBootstrapQueue.IsWorkerReadyForGeneration(previousDeliveryGeneration),
                m_diagInputPublishedFrames.load(std::memory_order_acquire) == 0,
                m_outputFrameCount.load(std::memory_order_acquire) == 0,
                m_seekResetState.phase == DirectMotionSeekReset::Phase::Idle &&
                    !m_seekResetState.deferredResetPending,
                !m_flushTransition.Active(),
                !m_stopRequested.load(std::memory_order_acquire),
                !previousSegmentValid);
        initialStagedFrameCount = m_sourceBootstrapQueue.QueuedFrames();
        if (stagedInitialGraphStillEligible &&
            m_sourceBootstrapQueue.RebindDormantGeneration(previousDeliveryGeneration, nextDeliveryGeneration)) {
            std::string sourceRebindError;
            if (tryRebindInitialSourceGeneration(sourceRebindError)) {
                // The graph has consumed no source frames: all initial preroll is
                // still queued and ordered. Rebind both the queue and the InputClip
                // cancellation epoch before publishing the new pipeline generation.
                // Do NOT reset source indices, timestamp origin or preroll boundary.
                m_engineGeneration.store(nextDeliveryGeneration, std::memory_order_release);
                m_streamInitialized = true;
                reboundInitialStagedGraph = true;
                LogInfo("NewSegment/InitialStage", "Rebound the dormant pre-segment source queue and the pristine AviSynth InputClip. "
                    "oldGeneration=" + std::to_string(previousDeliveryGeneration) +
                    ", newGeneration=" + std::to_string(nextDeliveryGeneration) +
                    ", stagedFrames=" + std::to_string(initialStagedFrameCount) +
                    ", sourceInterruptEpoch=" + std::to_string(sourceInterruptEpochBeforeSegment) +
                    "->" + std::to_string(sourceInterruptEpochAfterSegment) +
                    "; source indices and pre-roll timeline state are preserved.");
            } else {
                m_sourceBootstrapQueue.Cancel();
                adoptPreparedInitialGraph = false;
                LogError("NewSegment/InitialStage", "The dormant queue generation was rebound, but the retained AviSynth graph failed its pristine source-generation rebinding checks. The queue was cancelled and the graph will be rebuilt. reason=" + sourceRebindError);
            }
        } else if (preserveInitialSegmentStaging) {
            m_sourceBootstrapQueue.Cancel();
            LogInfo("NewSegment/InitialStage", "Initial staged-graph adoption became ineligible before the generation fence; "
                "retiring the staged preroll and falling back to the ordinary reset path.");
        }

        if (m_seekResetState.deferredResetPending) {
            // A staged Receive may have arrived after EndFlush and before this
            // callback. The queue kept it dormant; now transfer reset ownership to
            // the actual NewSegment generation and open the bootstrap consumer.
            if (!BeginDeferredSeekEngineReset(nextDeliveryGeneration, false)) return E_ABORT;
            LogInfo("NewSegment/Cache", "Rebound the deferred engine reset to the final NewSegment generation. Any pre-segment staged samples were retired; only this generation may bootstrap the replacement graph.");
        } else if (m_seekResetState.phase != DirectMotionSeekReset::Phase::Idle) {
            // A non-seek flush already reset the engine synchronously. Consume its
            // segment marker and reset frame/timeline counters for the final segment.
            ResetEngineGenerationState(false, nextDeliveryGeneration);
            if (!DirectMotionSeekReset::RebindForNewSegment(m_seekResetState, nextDeliveryGeneration))
                return E_ABORT;
            LogInfo("NewSegment/Cache", "Consumed the segment marker for an already-completed synchronous engine reset.");
        } else if (reboundInitialStagedGraph) {
            // Queue and engine generations were rebound above. Keep the source
            // indices, timestamp origin and preroll visibility boundary captured by
            // Receive before NewSegment; the worker is still dormant at this point.
            // Reset only the adjacent-sample continuity anchor: timestamps may
            // restart at zero at the newly announced segment boundary, while the
            // stream origin/preroll boundary must continue to describe staged frames.
            m_lastInputTimestampValid = false;
            m_lastInputSampleStopTimeValid = false;
            m_lastInputSampleStart = 0;
            m_lastInputSampleEnd = 0;
        } else if (adoptPreparedInitialGraph) {
            // This is the first segment of a newly started stream. The graph was
            // built by StartStreaming for the current worker generation and has
            // accepted no media samples. The source epoch must be rebound as part
            // of the adoption transaction; changing only the DirectShow generation
            // would leave InputClip holding the pre-NewSegment cancellation token.
            std::string sourceRebindError;
            if (tryRebindInitialSourceGeneration(sourceRebindError)) {
                ResetEngineGenerationState(false, nextDeliveryGeneration);
                m_engineGeneration.store(nextDeliveryGeneration, std::memory_order_release);
                m_streamInitialized = true;
                LogInfo("NewSegment/InitialGraph", "Adopted the pristine StartStreaming AviSynth/MVTools graph and rebound its InputClip source epoch without teardown/rebuild. oldGeneration=" +
                    std::to_string(previousDeliveryGeneration) + ", newGeneration=" +
                    std::to_string(nextDeliveryGeneration) + ", sourceInterruptEpoch=" +
                    std::to_string(sourceInterruptEpochBeforeSegment) + "->" +
                    std::to_string(sourceInterruptEpochAfterSegment) +
                    ", sourceFramesAccepted=0, outputFramesEmitted=0.");
            } else {
                LogError("NewSegment/InitialGraph", "The prepared AviSynth graph failed its pristine source-generation rebinding checks; using the ordinary graph reset path. reason=" + sourceRebindError);
                adoptPreparedInitialGraph = false;
                ResetEngine();
            }
        } else if (previousSegmentValid && !m_stopRequested.load(std::memory_order_acquire)) {
            if (!BeginDeferredSeekEngineReset(nextDeliveryGeneration, false)) return E_ABORT;
            LogInfo("NewSegment/Cache", "No upstream flush reset preceded NewSegment; requested deferred teardown so the first source fallback can be presented first.");
        } else {
            ResetEngine();
            LogInfo("NewSegment/Cache", "No deferred reset was pending for the initial/non-seek NewSegment; invalidated the interpolation cache synchronously.");
        }
        m_firstSampleAfterSegment = true;
        // `start` is the media position being sought to, not the stream-time timestamp
        // written into downstream samples. After a seek, sample timestamps are relative
        // to the new segment; Receive() captures the first such timestamp.
        if (!reboundInitialStagedGraph) {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_streamTimeOrigin = 0;
            m_streamTimeOriginSet = false;
        }
    }
    // Reset the audit budget after the synchronous prior-generation callback has left
    // m_csReceive, so an old frame cannot consume the new segment's audit window.
    if (auditSegment) {
        m_stepAuditInputSamplesRemaining.store(kStepAuditInputSamplesPerSegment, std::memory_order_release);
        m_stepAuditOutputSamplesRemaining.store(kStepAuditOutputSamplesPerSegment, std::memory_order_release);
        LogInfo("StepAudit/Window", "segmentAudit=" + std::to_string(auditSegmentIndex) +
            ", inputSamples=" + std::to_string(kStepAuditInputSamplesPerSegment) +
            ", outputSamples=" + std::to_string(kStepAuditOutputSamplesPerSegment) +
            ", segmentLimit=" + std::to_string(kStepAuditMaxSegments) +
            "; compact audit enabled. Set DIRECTMOTION_FULL_TRACE=1 only if an unbounded trace is explicitly needed.");
    }
    if (previousSegmentValid) {
        ArmSeekFallback("NewSegment/seek", nextDeliveryGeneration);
        const long double delta100ns = static_cast<long double>(start) -
            static_cast<long double>(previousSegmentStart);
        const long double deltaSeconds = delta100ns / static_cast<long double>(kReferenceTimeUnitsPerSecond);
        const long double targetFps = (m_targetRate.num > 0 && m_targetRate.den > 0)
            ? static_cast<long double>(m_targetRate.num) / static_cast<long double>(m_targetRate.den) : 0.0L;
        const long double approximateOutputFrames = deltaSeconds * targetFps;
        LogInfo("NewSegment/Timeline", "segmentId=" + std::to_string(segmentId) +
            ", previousStart100ns=" + std::to_string(previousSegmentStart) +
            ", currentStart100ns=" + std::to_string(start) +
            ", delta100ns=" + std::to_string(static_cast<double>(delta100ns)) +
            ", deltaSeconds=" + std::to_string(static_cast<double>(deltaSeconds)) +
            ", approximateOutputFrames=" + std::to_string(static_cast<double>(approximateOutputFrames)) +
            "; values are segment-start deltas, not a claim that every transition is a user seek.");
    } else {
        ClearSeekFallback(previousDeliveryGeneration);
        LogInfo("NewSegment/Timeline", "segmentId=" + std::to_string(segmentId) +
            ", no previous segment start available; establishing baseline at " + std::to_string(start) + ".");
    }
    const double resetElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - resetStarted).count();
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);
    // Pair the local flush before forwarding NewSegment (DirectShow segment changes
    // follow EndFlush). An upstream-owned flush remains paired by EndFlush().
    HRESULT localEndFlushHr = S_OK;
    if (ownsDownstreamFlush && m_pOutput && m_pOutput->IsConnected()) {
        localEndFlushHr = m_pOutput->DeliverEndFlush();
        LogInfo("NewSegment/LocalFlush", "Paired local downstream EndFlush before forwarding NewSegment, HRESULT=" +
            std::to_string(static_cast<unsigned long>(localEndFlushHr)));
        if (FAILED(localEndFlushHr)) {
            if (reboundInitialStagedGraph) m_sourceBootstrapQueue.Cancel();
            segmentTransitionLease.Release();
            return localEndFlushHr;
        }
    }
    // Do not hold the receive lock across base-class graph/pin callbacks.
    const auto baseStarted = std::chrono::steady_clock::now();
    const HRESULT baseHr = CTransformFilter::NewSegment(start, stop, rate);
    const double baseElapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - baseStarted).count();
    segmentTransitionLease.Release();
    if (reboundInitialStagedGraph) {
        if (SUCCEEDED(baseHr) && m_asyncPipeline.IsCurrentGeneration(nextDeliveryGeneration) &&
            m_sourceBootstrapQueue.SetWorkerReadyForGeneration(nextDeliveryGeneration, true)) {
            NotifySourceBootstrapWorker();
            LogInfo("NewSegment/InitialStage", "Opened the source-bootstrap worker only after downstream accepted the first NewSegment. "
                "generation=" + std::to_string(nextDeliveryGeneration) +
                ", stagedFrames=" + std::to_string(initialStagedFrameCount) + ".");
        } else {
            if (m_asyncPipeline.IsCurrentGeneration(nextDeliveryGeneration)) m_sourceBootstrapQueue.Cancel();
            if (SUCCEEDED(baseHr)) {
                FailRuntime("NewSegment/InitialStage",
                    "The initial preroll queue could not be released for the accepted segment generation.", E_ABORT);
                return E_ABORT;
            }
        }
    }
    NotifyDeliveryWorker();
    LogInfo("NewSegment", "EXIT segmentId=" + std::to_string(segmentId) +
        ", threadId=" + std::to_string(GetCurrentThreadId()) +
        ", HRESULT=" + std::to_string(static_cast<unsigned long>(baseHr)) +
        ", remainingSegmentTransitionOwners=" + std::to_string(m_segmentTransition.Count()) +
        ", resetAndLockElapsedMs=" + std::to_string(resetElapsedMs) +
        ", baseElapsedMs=" + std::to_string(baseElapsedMs) +
        ", totalElapsedMs=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count()));
    return baseHr;
}

void CDirectMotion::LogInfo(const char* where, const std::string& message) const {
    DirectMotionLog("INFO", where ? where : "?", message);
}

void CDirectMotion::LogError(const char* where, const std::string& message) const {
    DirectMotionLog("ERROR", where ? where : "?", message);
    DirectMotionTraySetStatus(DirectMotionStatus::Error);
}


void CDirectMotion::RefreshTrayRuntimeInfo() const noexcept {
    try {
        std::vector<std::wstring> lines;
        lines.reserve(13);
        const bool runtimeFailed = m_runtimeFailed.load(std::memory_order_acquire);
        lines.push_back(runtimeFailed ? L"State: interpolation failed (no fallback)" :
            (m_settings.enabled ? L"State: interpolation enabled" : L"State: interpolation off (pass-through selected)"));
        const double sourceFps = m_sourceRate.num > 0 && m_sourceRate.den > 0
            ? static_cast<double>(m_sourceRate.num) / m_sourceRate.den : 0.0;
        const double targetFps = m_targetRate.num > 0 && m_targetRate.den > 0
            ? static_cast<double>(m_targetRate.num) / m_targetRate.den : 0.0;
        wchar_t rate[160]{};
        swprintf_s(rate, L"FPS: input %.3f -> target %.3f", sourceFps, targetFps);
        lines.emplace_back(rate);

        std::wstring profile = L"Backend: AviSynth+ / MVTools2 (async processing + delivery)";
        if (!m_engine || !m_engine->IsInitialized()) profile += L" (inactive)";
        lines.push_back(profile);
        lines.emplace_back(L"Source bridge: Receive publishes NV12; processing + delivery workers");
        wchar_t geometry[240]{};
        swprintf_s(geometry,
            L"Picture %dx%d | Input pitch %d / height %d | Output surface %dx%d @ %d",
            m_width, m_height, m_inputStride, m_inputSurfaceHeight,
            m_outputStride, m_outputSurfaceHeight, m_outputStride);
        lines.emplace_back(geometry);

        const double lastIngest = m_lastPushMs.load(std::memory_order_acquire);
        const double avgIngest = m_avgPushMs.load(std::memory_order_acquire);
        const double lastGraph = m_lastGraphMs.load(std::memory_order_acquire);
        const double avgGraph = m_avgGraphMs.load(std::memory_order_acquire);
        const double lastDownstream = m_lastDownstreamMs.load(std::memory_order_acquire);
        const double avgDownstream = m_avgDownstreamMs.load(std::memory_order_acquire);
        const double allocator = m_lastAllocatorWaitMs.load(std::memory_order_acquire);

        wchar_t timing[220]{};
        const double sourceBudgetMs = sourceFps > 0.0 ? 1000.0 / sourceFps : 0.0;
        swprintf_s(timing, L"Source ingest ms/input: %.2f / avg %.2f (budget %.2f)",
            lastIngest, avgIngest, sourceBudgetMs);
        lines.emplace_back(timing);
        wchar_t graphTiming[200]{};
        swprintf_s(graphTiming, L"AviSynth GetFrame ms/output: %.2f / avg %.2f", lastGraph, avgGraph);
        lines.emplace_back(graphTiming);
        wchar_t delivery[200]{};
        swprintf_s(delivery, L"Downstream Receive ms: %.2f / avg %.2f; allocator wait %.2f",
            lastDownstream, avgDownstream, allocator);
        lines.emplace_back(delivery);
        wchar_t counts[260]{};
        swprintf_s(counts,
            L"Stages: RX %llu | NV12 %llu | cache %llu | gen %llu | interp-delivered %llu",
            static_cast<unsigned long long>(m_diagInputReceivedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagInputNormalizedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagInputPublishedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagOutputGeneratedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagOutputDeliveredFrames.load(std::memory_order_acquire)));
        lines.emplace_back(counts);

        const AsyncPipelineSnapshot pipeline = m_asyncPipeline.Snapshot();
        const wchar_t* phase = L"dormant";
        switch (pipeline.phase) {
        case AsyncPipelinePhase::AcceptingInput: phase = L"running"; break;
        case AsyncPipelinePhase::InputComplete: phase = L"draining"; break;
        case AsyncPipelinePhase::Cancelled: phase = L"cancelled"; break;
        case AsyncPipelinePhase::Failed: phase = L"failed"; break;
        default: break;
        }
        wchar_t pipelineState[240]{};
        swprintf_s(pipelineState,
            L"Pipeline gen %llu: %ls | processing %ls | delivery %ls",
            static_cast<unsigned long long>(pipeline.generation), phase,
            pipeline.processingBusy ? L"busy" : L"idle",
            pipeline.deliveryBusy ? L"busy" : L"idle");
        lines.emplace_back(pipelineState);

        const AsyncQueueMetrics& outputQueue = pipeline.output;
        const double producerWaitAvgMs = outputQueue.producerWaits
            ? static_cast<double>(outputQueue.producerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.producerWaits) : 0.0;
        const double consumerWaitAvgMs = outputQueue.consumerWaits
            ? static_cast<double>(outputQueue.consumerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.consumerWaits) : 0.0;
        wchar_t queueState[260]{};
        swprintf_s(queueState,
            L"Output FIFO %zu/%zu high %zu | put %llu get %llu drop %llu",
            outputQueue.queued, outputQueue.capacity, outputQueue.highWater,
            static_cast<unsigned long long>(outputQueue.enqueued),
            static_cast<unsigned long long>(outputQueue.dequeued),
            static_cast<unsigned long long>(outputQueue.discarded));
        lines.emplace_back(queueState);
        wchar_t sourceLaneState[260]{};
        swprintf_s(sourceLaneState,
            L"Source lane %zu/%zu high %zu | drop %llu | shared %.1f/%.0f MiB",
            outputQueue.fallbackQueued, outputQueue.fallbackCapacity,
            outputQueue.fallbackHighWater,
            static_cast<unsigned long long>(outputQueue.fallbackDiscarded),
            static_cast<double>(m_sourceBootstrapQueue.BytesOutstanding()) / (1024.0 * 1024.0),
            static_cast<double>(m_sourceBootstrapQueue.ByteBudget()) / (1024.0 * 1024.0));
        lines.emplace_back(sourceLaneState);
        wchar_t queueWaits[240]{};
        swprintf_s(queueWaits,
            L"FIFO waits: producer %llu avg %.2f/max %.2f ms | consumer %llu avg %.2f/max %.2f ms",
            static_cast<unsigned long long>(outputQueue.producerWaits), producerWaitAvgMs,
            static_cast<double>(outputQueue.producerWaitMaxUs) / 1000.0,
            static_cast<unsigned long long>(outputQueue.consumerWaits), consumerWaitAvgMs,
            static_cast<double>(outputQueue.consumerWaitMaxUs) / 1000.0);
        lines.emplace_back(queueWaits);
        DirectMotionTraySetRuntimeInfo(lines);

    } catch (...) {
        // UI telemetry is best-effort and must never break Receive/Deliver.
    }
}

void CDirectMotion::StartDiagnosticsWorker() noexcept {
    try {
        std::lock_guard<std::mutex> lifecycleLock(m_diagnosticsWorkerLifecycleMutex);
        if (m_diagnosticsWorkerThread.joinable()) return;
        m_diagnosticsWorkerStop.store(false, std::memory_order_release);
        m_lastAsyncTelemetryLogTick.store(GetTickCount64(), std::memory_order_release);
        m_diagnosticsWorkerThread = std::thread(&CDirectMotion::DiagnosticsWorkerMain, this);
        LogInfo("Diagnostics/Start", "Started the independent asynchronous-pipeline telemetry worker.");
    } catch (const std::exception& ex) {
        m_diagnosticsWorkerStop.store(true, std::memory_order_release);
        DirectMotionLog("WARN", "Diagnostics/Start",
            std::string("Could not start independent pipeline telemetry; video streaming remains available: ") + ex.what());
    } catch (...) {
        m_diagnosticsWorkerStop.store(true, std::memory_order_release);
        DirectMotionLog("WARN", "Diagnostics/Start",
            "Could not start independent pipeline telemetry; video streaming remains available.");
    }
}

void CDirectMotion::StopDiagnosticsWorker() noexcept {
    try {
        // Serialize stop/join with Start so the next streaming generation cannot
        // clear the stop flag while the previous diagnostics thread is unwinding.
        std::lock_guard<std::mutex> lifecycleLock(m_diagnosticsWorkerLifecycleMutex);
        m_diagnosticsWorkerStop.store(true, std::memory_order_release);
        if (m_diagnosticsWorkerThread.joinable()) {
            if (m_diagnosticsWorkerThread.get_id() == std::this_thread::get_id())
                m_diagnosticsWorkerThread.detach();
            else
                m_diagnosticsWorkerThread.join();
        }
    } catch (...) {
        DirectMotionLog("WARN", "Diagnostics/Stop", "Could not cleanly join the independent pipeline telemetry worker.");
    }
}

void CDirectMotion::DiagnosticsWorkerMain() noexcept {
    using directmotion_diagnostics::ClassifyPipelineStall;
    using directmotion_diagnostics::PipelineStallKind;
    using directmotion_diagnostics::PipelineStallObservation;
    using directmotion_diagnostics::PipelineStageCounters;

    uint64_t previousCounts[5] = {};
    ULONGLONG stageLastProgress[5] = {};
    ULONGLONG lastAnyProgressTick = GetTickCount64();
    ULONGLONG lastStallSnapshotTick = 0;
    bool havePreviousCounts = false;
    PipelineStallKind previousStallKind = PipelineStallKind::None;
    std::wstring stallStatus = L"STALL WATCH: waiting for Run";

    while (!m_diagnosticsWorkerStop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (m_diagnosticsWorkerStop.load(std::memory_order_acquire)) break;

        const ULONGLONG now = GetTickCount64();
        const uint64_t counts[5] = {
            m_diagInputReceivedFrames.load(std::memory_order_acquire),
            m_diagInputNormalizedFrames.load(std::memory_order_acquire),
            m_diagInputPublishedFrames.load(std::memory_order_acquire),
            m_diagOutputGeneratedFrames.load(std::memory_order_acquire),
            m_diagOutputDeliveredFrames.load(std::memory_order_acquire)
        };
        if (!havePreviousCounts) {
            for (size_t i = 0; i < 5; ++i) {
                previousCounts[i] = counts[i];
                stageLastProgress[i] = now;
            }
            lastAnyProgressTick = now;
            havePreviousCounts = true;
        } else {
            bool anyChanged = false;
            for (size_t i = 0; i < 5; ++i) {
                if (counts[i] != previousCounts[i]) {
                    previousCounts[i] = counts[i];
                    stageLastProgress[i] = now;
                    anyChanged = true;
                }
            }
            if (anyChanged) {
                lastAnyProgressTick = now;
                // A downstream stage which has not produced its first item yet must
                // receive a grace interval from the latest upstream publication.
                // Otherwise the monitor could report a stale zero counter as a stall
                // immediately when the first sample arrives after a long graph pause.
                if (counts[0] > 0 && counts[1] == 0) stageLastProgress[1] = now;
                if (counts[1] > 0 && counts[2] == 0) stageLastProgress[2] = now;
                if (counts[2] > 0 && counts[3] == 0) stageLastProgress[3] = now;
                if (counts[3] > 0 && counts[4] == 0) stageLastProgress[4] = now;
            }
        }

        // Keep the monitor alive long enough to publish the terminal diagnosis after
        // InputClip has marked an expired source request. Other runtime failures still
        // suspend ordinary stall sampling because their terminal error is already logged.
        const bool sourceFrameExpired = DirectMotionTraySourceFrameExpired();
        const bool runtimeFailed = m_runtimeFailed.load(std::memory_order_acquire);
        const bool runActive = m_runActive.load(std::memory_order_acquire);
        const bool graphActivelyRunning =
            (runActive || sourceFrameExpired) &&
            !m_pauseTransition.load(std::memory_order_acquire) &&
            !m_pauseCallInProgress.load(std::memory_order_acquire) &&
            !m_stopRequested.load(std::memory_order_acquire) &&
            !m_flushTransition.Active() &&
            !m_segmentTransition.Active() &&
            (!runtimeFailed || sourceFrameExpired) &&
            DirectMotionTrayInterpolationEnabled();

        if (!graphActivelyRunning) {
            previousStallKind = PipelineStallKind::None;
            lastStallSnapshotTick = 0;
            stallStatus = m_runActive.load(std::memory_order_acquire)
                ? L"STALL WATCH: suspended during graph transition"
                : L"STALL WATCH: waiting for Run";
        } else if (counts[0] == 0) {
            previousStallKind = PipelineStallKind::None;
            lastStallSnapshotTick = 0;
            stallStatus = L"STALL WATCH: waiting for first media sample";
        } else {
            const AsyncPipelineSnapshot pipeline = m_asyncPipeline.Snapshot();
            bool processingPending = false;
            bool processingActive = false;
            {
                // This mutex is held only around worker state transitions, never during
                // AviSynth evaluation or queue waits. Sampling it cannot block the media
                // path behind a long processing operation.
                std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex);
                processingPending = m_processingWorkPending &&
                    !m_processingWorkerStop && !m_processingWorkerSuspend;
                processingActive = m_processingWorkerActive;
            }

            const auto ageMs = [now](ULONGLONG tick) noexcept -> uint64_t {
                return tick != 0 && now >= tick ? static_cast<uint64_t>(now - tick) : 0;
            };
            PipelineStallObservation observation{};
            observation.counters = PipelineStageCounters{
                counts[0], counts[1], counts[2], counts[3], counts[4]
            };
            observation.receiveAgeMs = ageMs(stageLastProgress[0]);
            observation.normalizeAgeMs = ageMs(stageLastProgress[1]);
            observation.publishAgeMs = ageMs(stageLastProgress[2]);
            observation.generateAgeMs = ageMs(stageLastProgress[3]);
            observation.deliverAgeMs = ageMs(stageLastProgress[4]);
            observation.anyProgressAgeMs = ageMs(lastAnyProgressTick);
            observation.outputQueued = pipeline.output.queued;
            observation.outputCapacity = pipeline.output.capacity;
            observation.processingBusy = pipeline.processingBusy || processingActive;
            observation.processingPending = processingPending;
            observation.processingPassCapped = m_diagProcessingPassCapped.load(std::memory_order_acquire);
            observation.deliveryBusy = pipeline.deliveryBusy;
            observation.sourceCacheProducerWaiting = DirectMotionTraySourceCacheProducerWaiting();
            observation.sourceFrameExpired = sourceFrameExpired;
            observation.sourceWaiters = DirectMotionTrayGetSourceWaiterCount();

            const PipelineStallKind stallKind = ClassifyPipelineStall(observation);
            if (stallKind == PipelineStallKind::None) {
                previousStallKind = PipelineStallKind::None;
                lastStallSnapshotTick = 0;
                stallStatus = L"STALL WATCH: no stalled stage detected";
            } else {
                uint64_t stallAgeMs = observation.anyProgressAgeMs;
                switch (stallKind) {
                case PipelineStallKind::ReceiveNormalization: stallAgeMs = observation.normalizeAgeMs; break;
                case PipelineStallKind::SourceCachePublication:
                case PipelineStallKind::SourceCacheBackpressure: stallAgeMs = observation.publishAgeMs; break;
                case PipelineStallKind::SourceFrameExpired: stallAgeMs = observation.anyProgressAgeMs; break;
                case PipelineStallKind::DownstreamDelivery: stallAgeMs = observation.deliverAgeMs; break;
                case PipelineStallKind::PrefetchSourceWait:
                case PipelineStallKind::AviSynthProcessing:
                case PipelineStallKind::ProcessingNotScheduled: stallAgeMs = observation.generateAgeMs; break;
                default: break;
                }

                const std::string category = directmotion_diagnostics::PipelineStallKindName(stallKind);
                stallStatus = L"STALL: " + std::wstring(category.begin(), category.end()) +
                    L" | " + std::to_wstring(static_cast<double>(stallAgeMs) / 1000.0) + L"s";

                const bool newCategory = stallKind != previousStallKind;
                const bool repeatDue = lastStallSnapshotTick == 0 || now < lastStallSnapshotTick ||
                    now - lastStallSnapshotTick >= 5000;
                if (DirectMotionIsLoggingEnabled() && (newCategory || repeatDue)) {
                    const std::string activeContext = DirectMotionTrayGetDiagnosticContext();
                    DirectMotionLog("WARN", "AsyncPipeline/StallSnapshot",
                        "classification=" + category +
                        ", stallAgeMs=" + std::to_string(stallAgeMs) +
                        ", runActive=" + std::to_string(m_runActive.load(std::memory_order_acquire) ? 1 : 0) +
                        ", pauseTransition=" + std::to_string(m_pauseTransition.load(std::memory_order_acquire) ? 1 : 0) +
                        ", flushInProgress=" + std::to_string(m_flushTransition.Active() ? 1 : 0) +
                        ", segmentTransition=" + std::to_string(m_segmentTransition.Active() ? 1 : 0) +
                        ", generation=" + std::to_string(pipeline.generation) +
                        ", pipelinePhase=" + std::to_string(static_cast<unsigned>(pipeline.phase)) +
                        ", processingBusy=" + std::to_string(observation.processingBusy ? 1 : 0) +
                        ", processingPending=" + std::to_string(processingPending ? 1 : 0) +
                        ", processingPassCapped=" + std::to_string(observation.processingPassCapped ? 1 : 0) +
                        ", processingWorkerState=" + std::to_string(static_cast<unsigned>(pipeline.processingWorker)) +
                        ", deliveryBusy=" + std::to_string(pipeline.deliveryBusy ? 1 : 0) +
                        ", deliveryWorkerState=" + std::to_string(static_cast<unsigned>(pipeline.deliveryWorker)) +
                        ", stagesRxNv12CacheGeneratedDelivered=" +
                            std::to_string(counts[0]) + "/" + std::to_string(counts[1]) + "/" +
                            std::to_string(counts[2]) + "/" + std::to_string(counts[3]) + "/" +
                            std::to_string(counts[4]) +
                        ", stageAgeMsRxNv12CacheGeneratedDelivered=" +
                            std::to_string(observation.receiveAgeMs) + "/" +
                            std::to_string(observation.normalizeAgeMs) + "/" +
                            std::to_string(observation.publishAgeMs) + "/" +
                            std::to_string(observation.generateAgeMs) + "/" +
                            std::to_string(observation.deliverAgeMs) +
                        ", anyProgressAgeMs=" + std::to_string(observation.anyProgressAgeMs) +
                        ", outputQueue=" + std::to_string(pipeline.output.queued) + "/" +
                            std::to_string(pipeline.output.capacity) +
                        ", outputHighWater=" + std::to_string(pipeline.output.highWater) +
                        ", outputEnqueued=" + std::to_string(pipeline.output.enqueued) +
                        ", outputDequeued=" + std::to_string(pipeline.output.dequeued) +
                        ", outputDiscarded=" + std::to_string(pipeline.output.discarded) +
                        ", producerWaits=" + std::to_string(pipeline.output.producerWaits) +
                        ", producerWaitMaxMs=" + std::to_string(
                            static_cast<double>(pipeline.output.producerWaitMaxUs) / 1000.0) +
                        ", consumerWaits=" + std::to_string(pipeline.output.consumerWaits) +
                        ", consumerWaitMaxMs=" + std::to_string(
                            static_cast<double>(pipeline.output.consumerWaitMaxUs) / 1000.0) +
                        ", sourceWaiters=" + std::to_string(observation.sourceWaiters) +
                        ", sourceCacheProducerWaiting=" + std::to_string(observation.sourceCacheProducerWaiting ? 1 : 0) +
                        ", sourceFrameExpired=" + std::to_string(observation.sourceFrameExpired ? 1 : 0) +
                        ", processingWorkItems=" + std::to_string(pipeline.processingWorkItems) +
                        ", deliveryWorkItems=" + std::to_string(pipeline.deliveryWorkItems) +
                        ", activeWork={" + activeContext + "}" +
                        ", ingestLastMs=" + std::to_string(m_lastPushMs.load(std::memory_order_acquire)) +
                        ", getFrameLastMs=" + std::to_string(m_lastGraphMs.load(std::memory_order_acquire)) +
                        ", allocatorLastMs=" + std::to_string(m_lastAllocatorWaitMs.load(std::memory_order_acquire)) +
                        ", downstreamLastMs=" + std::to_string(m_lastDownstreamMs.load(std::memory_order_acquire)));
                    lastStallSnapshotTick = now;
                }
                previousStallKind = stallKind;
            }
        }

        // Keep the tray's dynamic pipeline rows live even when file logging is disabled
        // or DirectShow Receive has stopped making progress.
        RefreshAsyncPipelineTrayInfo(stallStatus);
        if (!DirectMotionIsLoggingEnabled()) {
            m_lastAsyncTelemetryLogTick.store(0, std::memory_order_release);
            continue;
        }
        LogAsyncPipelineTelemetry();
    }
}

void CDirectMotion::RefreshAsyncPipelineTrayInfo(const std::wstring& stallStatus) const noexcept {
    try {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = m_lastAsyncTrayInfoRefreshTick.load(std::memory_order_acquire);
        if (previous != 0 && (now < previous || now - previous < 500)) return;
        if (!m_lastAsyncTrayInfoRefreshTick.compare_exchange_strong(previous, now,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;

        const AsyncPipelineSnapshot pipeline = m_asyncPipeline.Snapshot();
        const AsyncQueueMetrics& outputQueue = pipeline.output;
        const double producerWaitAvgMs = outputQueue.producerWaits
            ? static_cast<double>(outputQueue.producerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.producerWaits) : 0.0;
        const double consumerWaitAvgMs = outputQueue.consumerWaits
            ? static_cast<double>(outputQueue.consumerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.consumerWaits) : 0.0;
        const wchar_t* phase = L"dormant";
        switch (pipeline.phase) {
        case AsyncPipelinePhase::AcceptingInput: phase = L"running"; break;
        case AsyncPipelinePhase::InputComplete: phase = L"draining"; break;
        case AsyncPipelinePhase::Cancelled: phase = L"cancelled"; break;
        case AsyncPipelinePhase::Failed: phase = L"failed"; break;
        default: break;
        }

        std::vector<std::wstring> lines;
        lines.reserve(5);
        wchar_t counts[260]{};
        swprintf_s(counts,
            L"Stages: RX %llu | NV12 %llu | cache %llu | gen %llu | interp-delivered %llu",
            static_cast<unsigned long long>(m_diagInputReceivedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagInputNormalizedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagInputPublishedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagOutputGeneratedFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(m_diagOutputDeliveredFrames.load(std::memory_order_acquire)));
        lines.emplace_back(counts);

        wchar_t pipelineState[240]{};
        swprintf_s(pipelineState,
            L"Pipeline gen %llu: %ls | processing %ls | delivery %ls | tagged threads %llu",
            static_cast<unsigned long long>(pipeline.generation), phase,
            pipeline.processingBusy ? L"busy" : L"idle",
            pipeline.deliveryBusy ? L"busy" : L"idle",
            DirectMotionTrayGetActiveWorkCount());
        lines.emplace_back(pipelineState);

        wchar_t queueState[260]{};
        swprintf_s(queueState,
            L"Output FIFO %zu/%zu high %zu | put %llu get %llu drop %llu",
            outputQueue.queued, outputQueue.capacity, outputQueue.highWater,
            static_cast<unsigned long long>(outputQueue.enqueued),
            static_cast<unsigned long long>(outputQueue.dequeued),
            static_cast<unsigned long long>(outputQueue.discarded));
        lines.emplace_back(queueState);

        wchar_t sourceLaneState[260]{};
        swprintf_s(sourceLaneState,
            L"Source lane %zu/%zu high %zu | drop %llu | shared %.1f/%.0f MiB",
            outputQueue.fallbackQueued, outputQueue.fallbackCapacity,
            outputQueue.fallbackHighWater,
            static_cast<unsigned long long>(outputQueue.fallbackDiscarded),
            static_cast<double>(m_sourceBootstrapQueue.BytesOutstanding()) / (1024.0 * 1024.0),
            static_cast<double>(m_sourceBootstrapQueue.ByteBudget()) / (1024.0 * 1024.0));
        lines.emplace_back(sourceLaneState);

        wchar_t queueWaits[240]{};
        swprintf_s(queueWaits,
            L"FIFO waits: producer %llu avg %.2f/max %.2f ms | consumer %llu avg %.2f/max %.2f ms",
            static_cast<unsigned long long>(outputQueue.producerWaits), producerWaitAvgMs,
            static_cast<double>(outputQueue.producerWaitMaxUs) / 1000.0,
            static_cast<unsigned long long>(outputQueue.consumerWaits), consumerWaitAvgMs,
            static_cast<double>(outputQueue.consumerWaitMaxUs) / 1000.0);
        lines.emplace_back(queueWaits);
        lines.emplace_back(stallStatus);
        DirectMotionTraySetRuntimeInfoRows(8, lines);
    } catch (...) {
        // Independent tray sampling is best-effort and must not affect streaming.
    }
}

void CDirectMotion::LogAsyncPipelineTelemetry() const noexcept {
    try {
        if (!DirectMotionIsLoggingEnabled()) return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = m_lastAsyncTelemetryLogTick.load(std::memory_order_acquire);
        if (previous != 0 && (now < previous || now - previous < 5000)) return;
        if (!m_lastAsyncTelemetryLogTick.compare_exchange_strong(previous, now,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;

        const AsyncPipelineSnapshot pipeline = m_asyncPipeline.Snapshot();
        const AsyncQueueMetrics& outputQueue = pipeline.output;
        const double producerWaitAvgMs = outputQueue.producerWaits
            ? static_cast<double>(outputQueue.producerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.producerWaits) : 0.0;
        const double consumerWaitAvgMs = outputQueue.consumerWaits
            ? static_cast<double>(outputQueue.consumerWaitTotalUs) / 1000.0 /
                static_cast<double>(outputQueue.consumerWaits) : 0.0;
        const auto timingSummary = [](const char* name,
            const std::atomic<uint64_t>& samples,
            const std::atomic<uint64_t>& totalNs,
            const std::atomic<uint64_t>& lastNs,
            const std::atomic<uint64_t>& maxNs) {
            const uint64_t count = samples.load(std::memory_order_acquire);
            const uint64_t total = totalNs.load(std::memory_order_relaxed);
            const double averageMs = count != 0
                ? static_cast<double>(total) / 1000000.0 / static_cast<double>(count) : 0.0;
            return std::string(", ") + name + "Samples=" + std::to_string(count) +
                ", " + name + "LastMs=" + std::to_string(
                    static_cast<double>(lastNs.load(std::memory_order_relaxed)) / 1000000.0) +
                ", " + name + "AvgMs=" + std::to_string(averageMs) +
                ", " + name + "MaxMs=" + std::to_string(
                    static_cast<double>(maxNs.load(std::memory_order_relaxed)) / 1000000.0);
        };

        DirectMotionLog("INFO", "AsyncPipeline/Telemetry",
            "generation=" + std::to_string(pipeline.generation) +
            ", phase=" + std::to_string(static_cast<unsigned>(pipeline.phase)) +
            ", processingBusy=" + std::to_string(pipeline.processingBusy ? 1 : 0) +
            ", deliveryBusy=" + std::to_string(pipeline.deliveryBusy ? 1 : 0) +
            ", processingWorkItems=" + std::to_string(pipeline.processingWorkItems) +
            ", activeTaggedThreads=" + std::to_string(DirectMotionTrayGetActiveWorkCount()) +
            ", deliveryWorkItems=" + std::to_string(pipeline.deliveryWorkItems) +
            ", outputQueue=" + std::to_string(outputQueue.queued) + "/" +
                std::to_string(outputQueue.capacity) +
            ", outputHighWater=" + std::to_string(outputQueue.highWater) +
            ", outputEnqueued=" + std::to_string(outputQueue.enqueued) +
            ", outputDequeued=" + std::to_string(outputQueue.dequeued) +
            ", outputDiscarded=" + std::to_string(outputQueue.discarded) +
            ", outputFullRejects=" + std::to_string(outputQueue.fullRejects) +
            ", outputStaleRejects=" + std::to_string(outputQueue.staleRejects) +
            ", outputCancelledRejects=" + std::to_string(outputQueue.cancelledRejects) +
            ", outputClosedRejects=" + std::to_string(outputQueue.closedRejects) +
            ", sourceFallbackQueue=" + std::to_string(outputQueue.fallbackQueued) + "/" +
                std::to_string(outputQueue.fallbackCapacity) +
            ", sourceFallbackHighWater=" + std::to_string(outputQueue.fallbackHighWater) +
            ", sourceFallbackEnqueued=" + std::to_string(outputQueue.fallbackEnqueued) +
            ", sourceFallbackDequeued=" + std::to_string(outputQueue.fallbackDequeued) +
            ", sourceFallbackDiscarded=" + std::to_string(outputQueue.fallbackDiscarded) +
            ", sourceFallbackFullRejects=" + std::to_string(outputQueue.fallbackFullRejects) +
            ", sourceFallbackStaleRejects=" + std::to_string(outputQueue.fallbackStaleRejects) +
            ", sourceFallbackCancelledRejects=" + std::to_string(outputQueue.fallbackCancelledRejects) +
            ", sourceSharedBytesOutstanding=" + std::to_string(m_sourceBootstrapQueue.BytesOutstanding()) +
            ", sourceSharedByteBudget=" + std::to_string(m_sourceBootstrapQueue.ByteBudget()) +
            ", producerWaits=" + std::to_string(outputQueue.producerWaits) +
            ", producerWaitAvgMs=" + std::to_string(producerWaitAvgMs) +
            ", producerWaitMaxMs=" + std::to_string(static_cast<double>(outputQueue.producerWaitMaxUs) / 1000.0) +
            ", consumerWaits=" + std::to_string(outputQueue.consumerWaits) +
            ", consumerWaitAvgMs=" + std::to_string(consumerWaitAvgMs) +
            ", consumerWaitMaxMs=" + std::to_string(static_cast<double>(outputQueue.consumerWaitMaxUs) / 1000.0) +
            ", inputReceived=" + std::to_string(m_diagInputReceivedFrames.load(std::memory_order_acquire)) +
            ", inputNormalized=" + std::to_string(m_diagInputNormalizedFrames.load(std::memory_order_acquire)) +
            ", inputPublished=" + std::to_string(m_diagInputPublishedFrames.load(std::memory_order_acquire)) +
            ", outputGenerated=" + std::to_string(m_diagOutputGeneratedFrames.load(std::memory_order_acquire)) +
            ", interpolatedOutputDelivered=" + std::to_string(m_diagOutputDeliveredFrames.load(std::memory_order_acquire)) +
            ", generationInputFrames=" + std::to_string(m_inputFrameCount.load(std::memory_order_acquire)) +
            ", generationOutputFrames=" + std::to_string(m_outputFrameCount.load(std::memory_order_acquire)) +
            ", ingestLastMs=" + std::to_string(m_lastPushMs.load(std::memory_order_acquire)) +
            ", ingestAvgMs=" + std::to_string(m_avgPushMs.load(std::memory_order_acquire)) +
            ", getFrameLastMs=" + std::to_string(m_lastGraphMs.load(std::memory_order_acquire)) +
            ", getFrameAvgMs=" + std::to_string(m_avgGraphMs.load(std::memory_order_acquire)) +
            ", allocatorLastMs=" + std::to_string(m_lastAllocatorWaitMs.load(std::memory_order_acquire)) +
            ", downstreamLastMs=" + std::to_string(m_lastDownstreamMs.load(std::memory_order_acquire)) +
            ", downstreamAvgMs=" + std::to_string(m_avgDownstreamMs.load(std::memory_order_acquire)) +
            timingSummary("inputNormalize", m_diagInputNormalizeSamples,
                m_diagInputNormalizeTotalNs, m_diagInputNormalizeLastNs, m_diagInputNormalizeMaxNs) +
            timingSummary("outputCopy", m_diagOutputCopySamples,
                m_diagOutputCopyTotalNs, m_diagOutputCopyLastNs, m_diagOutputCopyMaxNs) +
            timingSummary("deferredReset", m_diagDeferredResetSamples,
                m_diagDeferredResetTotalNs, m_diagDeferredResetLastNs, m_diagDeferredResetMaxNs) +
            timingSummary("deferredQuiescence", m_diagDeferredQuiescenceSamples,
                m_diagDeferredQuiescenceTotalNs, m_diagDeferredQuiescenceLastNs, m_diagDeferredQuiescenceMaxNs) +
            timingSummary("deferredEngineDestroy", m_diagDeferredDestroySamples,
                m_diagDeferredDestroyTotalNs, m_diagDeferredDestroyLastNs, m_diagDeferredDestroyMaxNs));
    } catch (...) {
        // The independent diagnostic worker must never affect the streaming workers.
    }
}

void CDirectMotion::LoadSettings() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\directmotion", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        DirectMotionTraySetInterpolationEnabled(m_settings.enabled);
        DirectMotionSetLoggingEnabled(m_settings.logEnabled);
        return;
    }
    auto readDword = [key](LPCWSTR name, DWORD fallback) {
        DWORD value = fallback, bytes = sizeof(value), type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
            type != REG_DWORD || bytes != sizeof(value)) return fallback;
        return value;
    };
    m_settings.enabled = readDword(L"Enabled", 1) != 0;
    m_settings.rateMode = static_cast<int>(readDword(L"RateMode", 0));
    m_settings.customRateNum = static_cast<int>(readDword(L"CustomRateNum", 60));
    m_settings.customRateDen = static_cast<int>(readDword(L"CustomRateDen", 1));
    m_settings.performance = static_cast<int>(readDword(L"Performance", 2));
    m_settings.grid = static_cast<int>(readDword(L"Grid", 4));
    // Before 0.2.47 Grid defaulted to the encoded 16px value (4). Treat that
    // historical default as "use the selected profile". Non-default legacy
    // values are preserved as explicit first-pass grid overrides.
    const DWORD legacyGridOverrideDefault =
        (m_settings.grid != 0 && m_settings.grid != 4) ? 1u : 0u;
    m_settings.gridOverrideEnabled = readDword(L"GridOverrideEnabled", legacyGridOverrideDefault) != 0;
    auto& mv = m_settings.mvtools;
    mv.advanced = readDword(L"AdvancedMvTools", 0) != 0;
    mv.superPel = static_cast<int>(readDword(L"SuperPel", 1));
    mv.superSharp = static_cast<int>(readDword(L"SuperSharp", 2));
    mv.superRFilter = static_cast<int>(readDword(L"SuperRFilter", 2));
    mv.superLevels = static_cast<int>(readDword(L"SuperLevels", 0));
    mv.analyseBlockSize = static_cast<int>(readDword(L"AnalyseBlockSize", 32));
    mv.analyseSearch = static_cast<int>(readDword(L"AnalyseSearch", 3));
    mv.analyseSearchParam = static_cast<int>(readDword(L"AnalyseSearchParam", 3));
    mv.analyseBadRange = static_cast<int>(readDword(L"AnalyseBadRange", static_cast<DWORD>(-24)));
    mv.analyseOverlap = static_cast<int>(readDword(L"AnalyseOverlap", 0));
    mv.analyseChroma = readDword(L"AnalyseChroma", 0) != 0;
    mv.analyseTrueMotion = readDword(L"AnalyseTrueMotion", 1) != 0;
    mv.recalcBlockSize = static_cast<int>(readDword(L"RecalcBlockSize", 8));
    mv.recalcSearch = static_cast<int>(readDword(L"RecalcSearch", 3));
    mv.recalcSearchParam = static_cast<int>(readDword(L"RecalcSearchParam", 0));
    mv.recalcOverlap = static_cast<int>(readDword(L"RecalcOverlap", 0));
    mv.blockFpsMode = static_cast<int>(readDword(L"BlockFpsMode", 2));
    mv.blockFpsMaskScale = static_cast<double>(readDword(L"BlockFpsMaskScale100", 10000)) / 100.0;
    mv.blockFpsBlend = readDword(L"BlockFpsBlend", 1) != 0;
    mv.thSCD1 = static_cast<int>(readDword(L"ThSCD1", 400));
    mv.thSCD2 = static_cast<int>(readDword(L"ThSCD2", 130));
    m_settings.saveFirstInputBmp = readDword(L"SaveFirstInputBmp", 0) != 0;
    m_settings.logEnabled = readDword(L"LogEnabled", 0) != 0;
    m_settings.cpuProfile = static_cast<int>(readDword(L"CpuProfile", kCpuProfileAuto));
    m_settings.customCpuWorkers = static_cast<int>(readDword(L"CustomCpuWorkers", 8));
    m_settings.receiveBatchFrames = static_cast<int>(readDword(L"ReceiveBatchFrames", 1));
    m_settings.prefetchFrames = static_cast<int>(readDword(L"PrefetchFrames", 0));
    m_settings.quickRecalcMode = static_cast<int>(readDword(L"QuickRecalcMode", 0));
    RegCloseKey(key);

    // Keep 0.2.45's 2x/3x registry values stable. New modes use reserved IDs so
    // legacy 2.5x/custom/absolute-FPS values from older releases still migrate to 2x.
    if (!IsSupportedRateMode(m_settings.rateMode)) m_settings.rateMode = kRateMode2x;
    if (m_settings.customRateNum < 1 || m_settings.customRateNum > 1000000) m_settings.customRateNum = 60;
    if (m_settings.customRateDen < 1 || m_settings.customRateDen > 1000000) m_settings.customRateDen = 1;
    if (m_settings.performance < 1 || m_settings.performance > 3) m_settings.performance = 2;
    if (m_settings.cpuProfile < kCpuProfileAuto || m_settings.cpuProfile > kCpuProfileCustom)
        m_settings.cpuProfile = kCpuProfileAuto;
    if (m_settings.customCpuWorkers < 1 || m_settings.customCpuWorkers > 32)
        m_settings.customCpuWorkers = 8;
    if (m_settings.receiveBatchFrames != 1 && m_settings.receiveBatchFrames != 2 &&
        m_settings.receiveBatchFrames != 4) m_settings.receiveBatchFrames = 1;
    if (m_settings.quickRecalcMode < 0 || m_settings.quickRecalcMode > 1)
        m_settings.quickRecalcMode = 0;
    const int validPrefetchValues[] = { 0, 8, 12, 16, 24, 32, 48, 64 };
    if (std::find(std::begin(validPrefetchValues), std::end(validPrefetchValues), m_settings.prefetchFrames) ==
        std::end(validPrefetchValues)) m_settings.prefetchFrames = 0;
    if (m_settings.grid != 0 && m_settings.grid != 1 && m_settings.grid != 2 && m_settings.grid != 4 && m_settings.grid != 8) {
        m_settings.grid = 4;
        m_settings.gridOverrideEnabled = false;
    }
    if (mv.superPel != 1 && mv.superPel != 2 && mv.superPel != 4) mv.superPel = 1;
    if (mv.superSharp < 0 || mv.superSharp > 2) mv.superSharp = 2;
    if (mv.superRFilter < 0 || mv.superRFilter > 4) mv.superRFilter = 2;
    if (mv.superLevels < 0 || mv.superLevels > 10) mv.superLevels = 0;
    auto validBlock = [](int value) { return value == 4 || value == 8 || value == 16 || value == 32; };
    if (!validBlock(mv.analyseBlockSize)) mv.analyseBlockSize = 32;
    if (!validBlock(mv.recalcBlockSize)) mv.recalcBlockSize = 8;
    if (mv.analyseSearch < 0 || mv.analyseSearch > 7) mv.analyseSearch = 3;
    if (mv.recalcSearch < 0 || mv.recalcSearch > 7) mv.recalcSearch = 3;
    if (mv.analyseSearchParam < 0 || mv.analyseSearchParam > 64) mv.analyseSearchParam = 3;
    if (mv.recalcSearchParam < 0 || mv.recalcSearchParam > 64) mv.recalcSearchParam = 0;
    if (mv.analyseBadRange < -128 || mv.analyseBadRange > 256) mv.analyseBadRange = -24;
    if (mv.analyseOverlap < 0 || mv.analyseOverlap > mv.analyseBlockSize / 2 || (mv.analyseOverlap & 1)) mv.analyseOverlap = 0;
    if (mv.recalcOverlap < 0 || mv.recalcOverlap > mv.recalcBlockSize / 2 || (mv.recalcOverlap & 1)) mv.recalcOverlap = 0;
    if (mv.blockFpsMode < 0 || mv.blockFpsMode > 8) mv.blockFpsMode = 2;
    if (!std::isfinite(mv.blockFpsMaskScale) || mv.blockFpsMaskScale < 0.0 || mv.blockFpsMaskScale > 1000.0) mv.blockFpsMaskScale = 100.0;
    if (mv.thSCD1 < 0 || mv.thSCD1 > 1000) mv.thSCD1 = 400;
    if (mv.thSCD2 < 0 || mv.thSCD2 > 1000) mv.thSCD2 = 130;
    DirectMotionTraySetInterpolationEnabled(m_settings.enabled);
    DirectMotionSetLoggingEnabled(m_settings.logEnabled);
}

HRESULT CDirectMotion::SaveSettings() const {
    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\directmotion", 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    const auto& mv = m_settings.mvtools;
    const struct SettingEntry { LPCWSTR name; DWORD value; } entries[] = {
        {L"Enabled", m_settings.enabled ? 1u : 0u},
        {L"RateMode", static_cast<DWORD>(m_settings.rateMode)},
        {L"CustomRateNum", static_cast<DWORD>(m_settings.customRateNum)},
        {L"CustomRateDen", static_cast<DWORD>(m_settings.customRateDen)},
        {L"Performance", static_cast<DWORD>(m_settings.performance)},
        {L"CpuProfile", static_cast<DWORD>(m_settings.cpuProfile)},
        {L"CustomCpuWorkers", static_cast<DWORD>(m_settings.customCpuWorkers)},
        {L"ReceiveBatchFrames", static_cast<DWORD>(m_settings.receiveBatchFrames)},
        {L"PrefetchFrames", static_cast<DWORD>(m_settings.prefetchFrames)},
        {L"QuickRecalcMode", static_cast<DWORD>(m_settings.quickRecalcMode)},
        {L"Grid", static_cast<DWORD>(m_settings.grid)},
        {L"GridOverrideEnabled", m_settings.gridOverrideEnabled ? 1u : 0u},
        {L"AdvancedMvTools", mv.advanced ? 1u : 0u},
        {L"SuperPel", static_cast<DWORD>(mv.superPel)},
        {L"SuperSharp", static_cast<DWORD>(mv.superSharp)},
        {L"SuperRFilter", static_cast<DWORD>(mv.superRFilter)},
        {L"SuperLevels", static_cast<DWORD>(mv.superLevels)},
        {L"AnalyseBlockSize", static_cast<DWORD>(mv.analyseBlockSize)},
        {L"AnalyseSearch", static_cast<DWORD>(mv.analyseSearch)},
        {L"AnalyseSearchParam", static_cast<DWORD>(mv.analyseSearchParam)},
        {L"AnalyseBadRange", static_cast<DWORD>(mv.analyseBadRange)},
        {L"AnalyseOverlap", static_cast<DWORD>(mv.analyseOverlap)},
        {L"AnalyseChroma", mv.analyseChroma ? 1u : 0u},
        {L"AnalyseTrueMotion", mv.analyseTrueMotion ? 1u : 0u},
        {L"RecalcBlockSize", static_cast<DWORD>(mv.recalcBlockSize)},
        {L"RecalcSearch", static_cast<DWORD>(mv.recalcSearch)},
        {L"RecalcSearchParam", static_cast<DWORD>(mv.recalcSearchParam)},
        {L"RecalcOverlap", static_cast<DWORD>(mv.recalcOverlap)},
        {L"BlockFpsMode", static_cast<DWORD>(mv.blockFpsMode)},
        {L"BlockFpsMaskScale100", static_cast<DWORD>(std::llround(mv.blockFpsMaskScale * 100.0))},
        {L"BlockFpsBlend", mv.blockFpsBlend ? 1u : 0u},
        {L"ThSCD1", static_cast<DWORD>(mv.thSCD1)},
        {L"ThSCD2", static_cast<DWORD>(mv.thSCD2)},
        {L"SaveFirstInputBmp", m_settings.saveFirstInputBmp ? 1u : 0u},
        {L"LogEnabled", m_settings.logEnabled ? 1u : 0u}
    };
    auto writeDword = [key](LPCWSTR name, DWORD value) -> LONG {
        return RegSetValueExW(key, name, 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    status = ERROR_SUCCESS;
    for (const auto& entry : entries) {
        status = writeDword(entry.name, entry.value);
        if (status != ERROR_SUCCESS) break;
    }
    if (status == ERROR_SUCCESS) (void)RegDeleteValueW(key, L"SeekHandoffDelayMs");
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT CDirectMotion::GetSettings(DirectMotionSettings* settings) {
    if (!settings) return E_POINTER;
    CAutoLock lock(&m_csReceive);
    *settings = m_settings;
    settings->seekHandoffDelayMs = 0; // Deprecated ABI slot; Immediate is the only supported handoff mode.
    settings->enabled = DirectMotionTrayInterpolationEnabled();
    settings->logEnabled = DirectMotionIsLoggingEnabled();
    return S_OK;
}

HRESULT CDirectMotion::SetSettings(const DirectMotionSettings* settings) {
    if (!settings) return E_POINTER;
    const DirectMotionSettings requested = *settings;
    const auto& mv = requested.mvtools;
    const auto validBlock = [](int value) { return value == 4 || value == 8 || value == 16 || value == 32; };
    if (!IsSupportedRateMode(requested.rateMode) ||
        requested.performance < 1 || requested.performance > 3 ||
        requested.cpuProfile < kCpuProfileAuto || requested.cpuProfile > kCpuProfileCustom ||
        requested.customCpuWorkers < 1 || requested.customCpuWorkers > 32 ||
        (requested.receiveBatchFrames != 1 && requested.receiveBatchFrames != 2 &&
         requested.receiveBatchFrames != 4) ||
        (requested.prefetchFrames != 0 && requested.prefetchFrames != 8 &&
         requested.prefetchFrames != 12 && requested.prefetchFrames != 16 &&
         requested.prefetchFrames != 24 && requested.prefetchFrames != 32 &&
         requested.prefetchFrames != 48 && requested.prefetchFrames != 64) ||
        requested.quickRecalcMode < 0 || requested.quickRecalcMode > 1 ||
        (requested.grid != 0 && requested.grid != 1 && requested.grid != 2 && requested.grid != 4 && requested.grid != 8) ||
        (mv.superPel != 1 && mv.superPel != 2 && mv.superPel != 4) ||
        mv.superSharp < 0 || mv.superSharp > 2 || mv.superRFilter < 0 || mv.superRFilter > 4 ||
        mv.superLevels < 0 || mv.superLevels > 10 ||
        !validBlock(mv.analyseBlockSize) || !validBlock(mv.recalcBlockSize) ||
        mv.analyseSearch < 0 || mv.analyseSearch > 7 || mv.recalcSearch < 0 || mv.recalcSearch > 7 ||
        mv.analyseSearchParam < 0 || mv.analyseSearchParam > 64 ||
        mv.recalcSearchParam < 0 || mv.recalcSearchParam > 64 ||
        mv.analyseBadRange < -128 || mv.analyseBadRange > 256 ||
        mv.analyseOverlap < 0 || mv.analyseOverlap > mv.analyseBlockSize / 2 || (mv.analyseOverlap & 1) ||
        mv.recalcOverlap < 0 || mv.recalcOverlap > mv.recalcBlockSize / 2 || (mv.recalcOverlap & 1) ||
        mv.blockFpsMode < 0 || mv.blockFpsMode > 8 ||
        !std::isfinite(mv.blockFpsMaskScale) || mv.blockFpsMaskScale < 0.0 || mv.blockFpsMaskScale > 1000.0 ||
        mv.thSCD1 < 0 || mv.thSCD1 > 1000 || mv.thSCD2 < 0 || mv.thSCD2 > 1000) {
        return E_INVALIDARG;
    }

    CAutoLock filterLock(&m_csFilter);
    const bool negotiationChanged = requested.enabled != m_settings.enabled ||
        requested.rateMode != m_settings.rateMode;
    const bool processingChanged = negotiationChanged ||
        requested.performance != m_settings.performance || requested.grid != m_settings.grid ||
        requested.gridOverrideEnabled != m_settings.gridOverrideEnabled ||
        requested.cpuProfile != m_settings.cpuProfile ||
        requested.customCpuWorkers != m_settings.customCpuWorkers ||
        requested.receiveBatchFrames != m_settings.receiveBatchFrames ||
        requested.prefetchFrames != m_settings.prefetchFrames ||
        requested.quickRecalcMode != m_settings.quickRecalcMode ||
        mv.advanced != m_settings.mvtools.advanced || mv.superPel != m_settings.mvtools.superPel ||
        mv.superSharp != m_settings.mvtools.superSharp || mv.superRFilter != m_settings.mvtools.superRFilter ||
        mv.superLevels != m_settings.mvtools.superLevels || mv.analyseBlockSize != m_settings.mvtools.analyseBlockSize ||
        mv.analyseSearch != m_settings.mvtools.analyseSearch || mv.analyseSearchParam != m_settings.mvtools.analyseSearchParam ||
        mv.analyseBadRange != m_settings.mvtools.analyseBadRange || mv.analyseOverlap != m_settings.mvtools.analyseOverlap ||
        mv.analyseChroma != m_settings.mvtools.analyseChroma || mv.analyseTrueMotion != m_settings.mvtools.analyseTrueMotion ||
        mv.recalcBlockSize != m_settings.mvtools.recalcBlockSize || mv.recalcSearch != m_settings.mvtools.recalcSearch ||
        mv.recalcSearchParam != m_settings.mvtools.recalcSearchParam || mv.recalcOverlap != m_settings.mvtools.recalcOverlap ||
        mv.blockFpsMode != m_settings.mvtools.blockFpsMode || mv.blockFpsMaskScale != m_settings.mvtools.blockFpsMaskScale ||
        mv.blockFpsBlend != m_settings.mvtools.blockFpsBlend || mv.thSCD1 != m_settings.mvtools.thSCD1 ||
        mv.thSCD2 != m_settings.mvtools.thSCD2;
    const bool anyChanged = processingChanged || requested.saveFirstInputBmp != m_settings.saveFirstInputBmp ||
        requested.logEnabled != m_settings.logEnabled;
    if (!anyChanged) {
        DirectMotionSetLoggingEnabled(requested.logEnabled);
        return S_OK;
    }
    if (m_State != State_Stopped) return VFW_E_NOT_STOPPED;
    CAutoLock receiveLock(&m_csReceive);

    const DirectMotionSettings oldSettings = m_settings;
    DirectMotionSetLoggingEnabled(requested.logEnabled);
    m_settings = requested;
    m_settings.seekHandoffDelayMs = 0; // Ignore values supplied through the deprecated ABI slot.
    if (processingChanged) ResetEngine();
    DirectMotionTraySetInterpolationEnabled(m_settings.enabled);
    DirectMotionTraySetStatus(DirectMotionStatus::Standby);

    if (negotiationChanged && m_pInput && m_pInput->IsConnected() && m_pOutput && m_pOutput->IsConnected()) {
        const HRESULT hr = UpdateOutputConnectionType();
        if (FAILED(hr)) {
            m_settings = oldSettings;
            DirectMotionTraySetInterpolationEnabled(oldSettings.enabled);
            DirectMotionSetLoggingEnabled(oldSettings.logEnabled);
            ResetEngine();
            m_sourceRate = SourceFrameRate();
            const FrucRational restoredTargetRate = m_settings.enabled ? TargetFrameRate() : m_sourceRate;
            const REFERENCE_TIME restoredFrameDuration = TargetFrameDuration();
            {
                std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
                m_targetRate = restoredTargetRate;
                m_targetFrameDuration = restoredFrameDuration;
            }
            if (m_pGraph && m_pOutput && m_pOutput->IsConnected()) (void)m_pGraph->Reconnect(m_pOutput);
            LogError("SetSettings/Reconnect", "Output reconnection failed, HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
            return hr;
        }
    }
    const HRESULT saveHr = SaveSettings();
    if (FAILED(saveHr)) {
        m_settings = oldSettings;
        DirectMotionTraySetInterpolationEnabled(oldSettings.enabled);
        DirectMotionSetLoggingEnabled(oldSettings.logEnabled);
        ResetEngine();
        m_sourceRate = SourceFrameRate();
        const FrucRational restoredTargetRate = m_settings.enabled ? TargetFrameRate() : m_sourceRate;
        const REFERENCE_TIME restoredFrameDuration = TargetFrameDuration();
        {
            std::lock_guard<std::mutex> timelineLock(m_outputTimelineMutex);
            m_targetRate = restoredTargetRate;
            m_targetFrameDuration = restoredFrameDuration;
        }
        if (negotiationChanged && m_pGraph && m_pOutput && m_pOutput->IsConnected()) (void)m_pGraph->Reconnect(m_pOutput);
        (void)SaveSettings();
        return saveHr;
    }
    LogInfo("SetSettings", "Settings updated. Enabled=" + std::to_string(m_settings.enabled ? 1 : 0) +
        ", rateMode=" + std::to_string(m_settings.rateMode) +
        ", outputRateMode=" + RateModeName(m_settings.rateMode) +
        ", resolvedTargetFPS=" + std::to_string(static_cast<double>(m_targetRate.num) / m_targetRate.den) +
        ", performance=" + std::to_string(m_settings.performance) + ", grid=" + std::to_string(m_settings.grid) +
        ", gridOverrideEnabled=" + std::to_string(m_settings.gridOverrideEnabled ? 1 : 0) +
        ", cpuProfile=" + std::to_string(m_settings.cpuProfile) +
        ", customCpuWorkers=" + std::to_string(m_settings.customCpuWorkers) +
        ", receiveBatchFrames=" + std::to_string(m_settings.receiveBatchFrames) +
        ", prefetchFrames=" + std::to_string(m_settings.prefetchFrames) +
        ", advancedMvTools=" + std::to_string(m_settings.mvtools.advanced ? 1 : 0) +
        ", saveFirstInputBmp=" + std::to_string(m_settings.saveFirstInputBmp ? 1 : 0) +
        ", logEnabled=" + std::to_string(m_settings.logEnabled ? 1 : 0));
    return S_OK;
}

HRESULT CDirectMotion::UpdateOutputConnectionType() {
    if (!m_pOutput || !m_pOutput->IsConnected()) return S_FALSE;
    if (!m_pGraph) return E_UNEXPECTED;
    // Let DirectShow renegotiate the output type through the graph.
    // Calling ReceiveConnection on an already-connected downstream pin is invalid.
    return m_pGraph->Reconnect(m_pOutput);
}

STDMETHODIMP CDirectMotion::GetPages(CAUUID* pages) {
    if (!pages) return E_POINTER;
    pages->cElems = 1;
    pages->pElems = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
    if (!pages->pElems) { pages->cElems = 0; return E_OUTOFMEMORY; }
    pages->pElems[0] = CLSID_directmotionSettings;
    return S_OK;
}
