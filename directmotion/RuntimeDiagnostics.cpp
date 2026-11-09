#include "RuntimeDiagnostics.h"
#include <shellapi.h>
#include <windowsx.h>
#include <winreg.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <limits>
#include <cmath>
#include <map>
#include <utility>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
constexpr UINT kTrayRefreshMessage = WM_APP + 0x4D1;
constexpr UINT kTrayCallbackMessage = WM_APP + 0x4D2;
constexpr UINT kTrayIconId = 0x44524631;
constexpr UINT_PTR kTrayRetryTimerId = 0x4452;
constexpr UINT kTrayRetryIntervalMs = 2000;
constexpr UINT_PTR kTrayStatusTimerId = 0x4454;
constexpr UINT kTrayStatusIntervalMs = 500;
constexpr UINT_PTR kTrayMenuRefreshTimerId = 0x4455;
constexpr UINT kTrayMenuRefreshIntervalMs = 500;
constexpr UINT kTrayToggleInterpolationCommand = 0x4453;
constexpr UINT kTrayActivityFirstPosition = 2; // toggle item + separator
constexpr size_t kTrayActivityRowCount = 7;
constexpr ULONGLONG kWorkPhaseIconThresholdMs = 500;
constexpr ULONGLONG kSourceWaitIconThresholdMs = 750;
constexpr ULONGLONG kNoInputThresholdMs = 1000;
constexpr ULONGLONG kNoOutputThresholdMs = 1000;
constexpr ULONGLONG kThroughputWindowMs = 1000;
constexpr ULONGLONG kThroughputSustainMs = 1000;
constexpr double kThroughputWarningRatio = 0.85;
constexpr wchar_t kWindowClass[] = L"directmotion.Tray.Window.v1";
UINT g_taskbarCreatedMessage = 0;

std::mutex g_logMutex;
std::mutex g_trayLifecycleMutex;
std::mutex g_runtimeInfoMutex;
std::mutex g_workContextMutex;
std::vector<std::wstring> g_runtimeInfo;
std::once_flag g_logPreferenceOnce;
std::atomic<bool> g_logEnabled{ false };
std::atomic<bool> g_interpolationEnabled{ true };
std::atomic<bool> g_graphRunning{ false };
std::atomic<int> g_status{ static_cast<int>(DirectMotionStatus::Standby) };
std::atomic<uint64_t> g_seekHandoffStatePacked{ 0 };

struct ActiveWorkRecord {
    DirectMotionWorkPhase phase = DirectMotionWorkPhase::Idle;
    std::wstring operation;
    long long inputFrame = -1;
    long long outputFrame = -1;
    DWORD threadId = 0;
    ULONGLONG startedTick = 0;
    unsigned long long sequence = 0;
    bool watchdogReported = false;
};

struct ActiveWorkSnapshot {
    bool hasPrimary = false;
    bool backendActive = false;
    size_t activeCount = 0;
    ActiveWorkRecord primary;
};

std::map<DWORD, std::vector<ActiveWorkRecord>> g_activeWorkByThread;
unsigned long long g_workSequence = 0;
std::atomic<ULONGLONG> g_lastInputTick{ 0 };
std::atomic<ULONGLONG> g_lastOutputTick{ 0 };
std::atomic<long long> g_lastInputFrame{ -1 };
std::atomic<long long> g_lastOutputFrame{ -1 };
std::atomic<double> g_targetOutputFps{ 0.0 };
std::atomic<double> g_measuredOutputFps{ 0.0 };
std::atomic<ULONGLONG> g_throughputWindowStartTick{ 0 };
std::atomic<long long> g_throughputWindowStartFrame{ 0 };
std::atomic<ULONGLONG> g_throughputBelowSinceTick{ 0 };
std::atomic<int> g_sourceWaiterCount{ 0 };
std::atomic<long long> g_sourceWaitLowestRequested{ -1 };
std::atomic<long long> g_sourceWaitHighestRequested{ -1 };
std::atomic<long long> g_sourceWaitCacheBase{ 0 };
std::atomic<long long> g_sourceWaitCacheEnd{ -1 };
std::atomic<unsigned long long> g_sourceWaitGeneration{ 0 };
std::atomic<ULONGLONG> g_sourceWaitStartedTick{ 0 };
std::atomic<bool> g_sourceFrameExpired{ false };
std::atomic<bool> g_sourceCacheProducerWaiting{ false };
std::wstring g_lastOperation;
ULONGLONG g_lastOperationDurationMs = 0;
DWORD g_lastOperationThreadId = 0;
ULONGLONG g_lastOperationCompletedTick = 0;
COLORREF g_lastIconColor = CLR_INVALID;
std::wstring g_lastTooltip;
LONG g_trayReferences = 0;
HANDLE g_trayThread = nullptr;
HANDLE g_trayReadyEvent = nullptr;
DWORD g_trayThreadId = 0;
std::atomic<HWND> g_trayWindow{ nullptr };
NOTIFYICONDATAW g_nid{};
bool g_iconAdded = false;
HICON g_currentIcon = nullptr;
HMENU g_openTrayMenu = nullptr; // owned and accessed only by the tray thread
UINT_PTR g_openTrayMenuTimer = 0; // thread timer; continues firing in TrackPopupMenu's modal loop
bool g_menuRefreshFailureLogged = false;

void EnsureLogPreferenceLoaded() noexcept {
    try {
        std::call_once(g_logPreferenceOnce, []() {
            HKEY key = nullptr;
            DWORD value = 0, bytes = sizeof(value), type = 0;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\directmotion", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
                if (RegQueryValueExW(key, L"LogEnabled", nullptr, &type,
                    reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
                    type != REG_DWORD || bytes != sizeof(value)) value = 0;
                RegCloseKey(key);
            }
            g_logEnabled.store(value != 0);
        });
    } catch (...) {
    }
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return "(message conversion failed)";
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), count, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const char* value) noexcept {
    if (!value || !*value) return L"(unspecified operation)";
    try {
        const int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
        if (count <= 1) return L"(operation text unavailable)";
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count) != count)
            return L"(operation conversion failed)";
        result.resize(static_cast<size_t>(count - 1));
        return result;
    } catch (...) {
        return L"(operation conversion failed)";
    }
}

const wchar_t* StatusText(DirectMotionStatus status) noexcept {
    const bool interpolation = g_interpolationEnabled.load();
    switch (status) {
    case DirectMotionStatus::Active:
        return interpolation ? L"directmotion: ACTIVE - AviSynth+ / MVTools2 delivering frames" : L"directmotion: STANDBY - interpolation disabled";
    case DirectMotionStatus::Error:
        return interpolation ? L"directmotion: ERROR - interpolation stopped; see directmotion.log" : L"directmotion: STANDBY - interpolation disabled";
    default:
        return interpolation ? L"directmotion: STANDBY / interpolation enabled / awaiting frame" : L"directmotion: STANDBY / interpolation disabled";
    }
}

const wchar_t* WorkPhaseName(DirectMotionWorkPhase phase) noexcept {
    switch (phase) {
    case DirectMotionWorkPhase::Backend: return L"AviSynth+ / MVTools2 GetFrame";
    case DirectMotionWorkPhase::Allocator: return L"DirectShow GetDeliveryBuffer";
    case DirectMotionWorkPhase::Downstream: return L"downstream renderer Receive";
    case DirectMotionWorkPhase::InputCopy: return L"NV12/NV21 input normalization";
    case DirectMotionWorkPhase::SourceIngest: return L"AviSynth live-source ingestion";
    case DirectMotionWorkPhase::ReceiveLock: return L"DirectShow Receive / m_csReceive lock";
    case DirectMotionWorkPhase::OutputQueue: return L"output FIFO capacity wait";
    default: return L"no filter stage tagged as active";
    }
}

const wchar_t* WorkPhaseShortName(DirectMotionWorkPhase phase) noexcept {
    switch (phase) {
    case DirectMotionWorkPhase::Backend: return L"AviSynth GetFrame";
    case DirectMotionWorkPhase::Allocator: return L"GetDeliveryBuffer";
    case DirectMotionWorkPhase::Downstream: return L"renderer Receive";
    case DirectMotionWorkPhase::InputCopy: return L"NV12 input copy";
    case DirectMotionWorkPhase::SourceIngest: return L"source ingest";
    case DirectMotionWorkPhase::ReceiveLock: return L"Receive lock";
    case DirectMotionWorkPhase::OutputQueue: return L"output FIFO wait";
    default: return L"DirectMotion";
    }
}

struct TrayVisualState {
    COLORREF color = RGB(135, 140, 148);
    std::wstring tooltip;
};

ActiveWorkSnapshot SnapshotActiveWork() noexcept {
    ActiveWorkSnapshot snapshot;
    try {
        std::lock_guard<std::mutex> lock(g_workContextMutex);
        for (const auto& item : g_activeWorkByThread) {
            const std::vector<ActiveWorkRecord>& stack = item.second;
            if (stack.empty()) continue;
            ++snapshot.activeCount;

            const ActiveWorkRecord& record = stack.back();
            for (const ActiveWorkRecord& nested : stack) {
                if (nested.phase == DirectMotionWorkPhase::Backend) {
                    snapshot.backendActive = true;
                    break;
                }
            }
            if (!snapshot.hasPrimary || record.startedTick < snapshot.primary.startedTick) {
                snapshot.primary = record;
                snapshot.hasPrimary = true;
            }
        }
    } catch (...) {
    }
    return snapshot;
}

ULONGLONG WorkPhaseWatchdogThreshold(DirectMotionWorkPhase phase) noexcept {
    switch (phase) {
    case DirectMotionWorkPhase::Allocator:
    case DirectMotionWorkPhase::Downstream:
    case DirectMotionWorkPhase::OutputQueue:
    case DirectMotionWorkPhase::ReceiveLock: return 750;
    case DirectMotionWorkPhase::Backend: return 2500;
    case DirectMotionWorkPhase::InputCopy:
    case DirectMotionWorkPhase::SourceIngest: return 1000;
    default: return 1500;
    }
}

TrayVisualState BuildTrayVisualState() noexcept {
    TrayVisualState visual;
    try {
        const auto status = static_cast<DirectMotionStatus>(g_status.load(std::memory_order_acquire));
        const bool interpolation = g_interpolationEnabled.load(std::memory_order_acquire);
        const ActiveWorkSnapshot activeWork = SnapshotActiveWork();
        const auto phase = activeWork.hasPrimary ? activeWork.primary.phase : DirectMotionWorkPhase::Idle;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG workStart = activeWork.hasPrimary ? activeWork.primary.startedTick : 0;
        const ULONGLONG workAge = workStart && now >= workStart ? now - workStart : 0;
        const ULONGLONG lastInput = g_lastInputTick.load(std::memory_order_acquire);
        const ULONGLONG lastOutput = g_lastOutputTick.load(std::memory_order_acquire);
        const ULONGLONG inputAge = lastInput && now >= lastInput ? now - lastInput : (std::numeric_limits<ULONGLONG>::max)();
        const ULONGLONG outputAge = lastOutput && now >= lastOutput ? now - lastOutput : (std::numeric_limits<ULONGLONG>::max)();
        const long long inputFrame = activeWork.hasPrimary ? activeWork.primary.inputFrame : -1;
        const long long outputFrame = activeWork.hasPrimary ? activeWork.primary.outputFrame : -1;
        const DWORD threadId = activeWork.hasPrimary ? activeWork.primary.threadId : 0;
        const double targetFps = g_targetOutputFps.load(std::memory_order_acquire);
        const double measuredFps = g_measuredOutputFps.load(std::memory_order_acquire);
        const ULONGLONG belowSince = g_throughputBelowSinceTick.load(std::memory_order_acquire);
        const int sourceWaiters = g_sourceWaiterCount.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitStart = g_sourceWaitStartedTick.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitAge = sourceWaitStart && now >= sourceWaitStart ? now - sourceWaitStart : 0;
        const bool throughputBelowTarget = targetFps > 1.0 && measuredFps > 0.0 &&
            measuredFps < targetFps * kThroughputWarningRatio && belowSince != 0 &&
            now >= belowSince && now - belowSince >= kThroughputSustainMs;

        if (status == DirectMotionStatus::Error) {
            visual.color = RGB(222, 48, 48);
            visual.tooltip = L"directmotion: ERROR - interpolation stopped; inspect directmotion.log";
        } else if (activeWork.backendActive && sourceWaiters > 0 &&
                   sourceWaitAge >= kSourceWaitIconThresholdMs) {
            visual.color = RGB(225, 150, 35);
            wchar_t detail[180]{};
            swprintf_s(detail, L"directmotion: SOURCE WAIT %d workers %.1fs | req %lld..%lld | cache %lld..%lld",
                sourceWaiters, static_cast<double>(sourceWaitAge) / 1000.0,
                g_sourceWaitLowestRequested.load(std::memory_order_acquire),
                g_sourceWaitHighestRequested.load(std::memory_order_acquire),
                g_sourceWaitCacheBase.load(std::memory_order_acquire),
                g_sourceWaitCacheEnd.load(std::memory_order_acquire));
            visual.tooltip = detail;
        } else if (phase != DirectMotionWorkPhase::Idle) {
            const bool allocatorOrRenderer = phase == DirectMotionWorkPhase::Allocator ||
                phase == DirectMotionWorkPhase::Downstream || phase == DirectMotionWorkPhase::ReceiveLock ||
                phase == DirectMotionWorkPhase::OutputQueue;
            visual.color = workAge >= kWorkPhaseIconThresholdMs
                ? (allocatorOrRenderer ? RGB(235, 145, 35) : RGB(45, 135, 220))
                : RGB(28, 190, 78);
            const wchar_t* verb = allocatorOrRenderer ? L"WAITING" : L"PROCESSING";
            wchar_t detail[180]{};
            swprintf_s(detail, L"directmotion: %ls - %ls %.1fs | in %lld out %lld tid %lu | active %zu",
                verb, WorkPhaseShortName(phase), static_cast<double>(workAge) / 1000.0,
                inputFrame, outputFrame, threadId, activeWork.activeCount);
            visual.tooltip = detail;
        } else if (!interpolation) {
            visual.color = RGB(135, 140, 148);
            visual.tooltip = L"directmotion: STANDBY - interpolation disabled";
        } else if (!g_graphRunning.load(std::memory_order_acquire)) {
            visual.color = RGB(135, 140, 148);
            visual.tooltip = L"directmotion: STANDBY - graph paused or stopped";
        } else if (lastInput == 0) {
            visual.color = RGB(225, 170, 45);
            visual.tooltip = L"directmotion: WAITING FOR FIRST UPSTREAM Receive";
        } else if (inputAge > kNoInputThresholdMs && lastInput != 0) {
            visual.color = RGB(225, 170, 45);
            wchar_t detail[160]{};
            swprintf_s(detail, L"directmotion: NO UPSTREAM Receive %.1fs | last in %lld out %lld",
                static_cast<double>(inputAge) / 1000.0, inputFrame, outputFrame);
            visual.tooltip = detail;
        } else if (lastOutput == 0 && lastInput != 0) {
            visual.color = RGB(225, 170, 45);
            wchar_t detail[160]{};
            swprintf_s(detail, L"directmotion: WARMING UP / awaiting first output | input %lld",
                inputFrame);
            visual.tooltip = detail;
        } else if (outputAge > kNoOutputThresholdMs && lastInput != 0) {
            visual.color = RGB(225, 170, 45);
            wchar_t detail[160]{};
            swprintf_s(detail, L"directmotion: BUFFERING / no output %.1fs | input %lld out %lld",
                static_cast<double>(outputAge) / 1000.0, inputFrame, outputFrame);
            visual.tooltip = detail;
        } else if (throughputBelowTarget && interpolation &&
                   g_graphRunning.load(std::memory_order_acquire)) {
            visual.color = RGB(225, 150, 35);
            wchar_t detail[160]{};
            swprintf_s(detail, L"directmotion: LOW THROUGHPUT %.1f/%.1f FPS (%.0f%%) | out %lld",
                measuredFps, targetFps, (measuredFps / targetFps) * 100.0, outputFrame);
            visual.tooltip = detail;
        } else if (status == DirectMotionStatus::Standby) {
            visual.color = RGB(225, 170, 45);
            visual.tooltip = L"directmotion: WARMING UP - input received, awaiting first output";
        } else {
            visual.color = RGB(28, 190, 78);
            wchar_t detail[160]{};
            swprintf_s(detail, L"directmotion: ACTIVE | input %lld / output %lld",
                inputFrame, outputFrame);
            visual.tooltip = detail;
        }
        if (visual.tooltip.size() >= 127) visual.tooltip.resize(126);
    } catch (...) {
        visual.color = RGB(222, 48, 48);
        visual.tooltip = L"directmotion: DIAGNOSTICS ERROR";
    }
    return visual;
}

std::vector<std::wstring> BuildActivityDetails() noexcept {
    std::vector<std::wstring> lines;
    try {
        lines.reserve(kTrayActivityRowCount);
        const ULONGLONG now = GetTickCount64();
        lines.emplace_back(g_graphRunning.load(std::memory_order_acquire)
            ? L"GRAPH: Running" : L"GRAPH: Paused / stopped");

        const uint64_t packedSeekState = g_seekHandoffStatePacked.load(std::memory_order_acquire);
        const uint64_t seekGeneration = DirectMotionSeekHandoffStatus::GenerationOf(packedSeekState);
        const auto seekState = DirectMotionSeekHandoffStatus::StateOf(packedSeekState);
        wchar_t seekHandoff[220]{};
        switch (seekState) {
        case DirectMotionSeekHandoffStatus::State::SourceFallback:
            swprintf_s(seekHandoff, L"SEEK HANDOFF: source fallback active | gen %llu",
                static_cast<unsigned long long>(seekGeneration));
            break;
        case DirectMotionSeekHandoffStatus::State::PtsWatermarkDrain:
            swprintf_s(seekHandoff, L"SEEK HANDOFF: PTS watermark drain; source admission sealed | gen %llu",
                static_cast<unsigned long long>(seekGeneration));
            break;
        case DirectMotionSeekHandoffStatus::State::VisualBlend:
            swprintf_s(seekHandoff, L"SEEK HANDOFF: six-frame source-to-MVTools blend | gen %llu",
                static_cast<unsigned long long>(seekGeneration));
            break;
        default:
            swprintf_s(seekHandoff, L"SEEK HANDOFF: idle / normal MVTools output | gen %llu",
                static_cast<unsigned long long>(seekGeneration));
            break;
        }
        lines.emplace_back(seekHandoff);

        const double targetFps = g_targetOutputFps.load(std::memory_order_acquire);
        const double measuredFps = g_measuredOutputFps.load(std::memory_order_acquire);
        wchar_t throughput[160]{};
        if (targetFps > 1.0) {
            swprintf_s(throughput, L"THROUGHPUT: %.1f / %.1f FPS (%.0f%% of target)",
                measuredFps, targetFps,
                measuredFps > 0.0 ? (measuredFps / targetFps) * 100.0 : 0.0);
        } else {
            swprintf_s(throughput, L"THROUGHPUT: waiting for negotiated target FPS");
        }
        lines.emplace_back(throughput);

        const ActiveWorkSnapshot activeWork = SnapshotActiveWork();
        if (activeWork.hasPrimary) {
            const ActiveWorkRecord& primary = activeWork.primary;
            const ULONGLONG workAge = primary.startedTick && now >= primary.startedTick
                ? now - primary.startedTick : 0;
            const std::wstring operation = primary.operation.empty()
                ? WorkPhaseName(primary.phase) : primary.operation;
            wchar_t current[300]{};
            swprintf_s(current,
                L"CURRENT: %zu active | %ls | %.1f s | input #%lld | output #%lld | thread %lu",
                activeWork.activeCount, operation.c_str(), static_cast<double>(workAge) / 1000.0,
                primary.inputFrame, primary.outputFrame, primary.threadId);
            lines.emplace_back(current);
        } else {
            lines.emplace_back(L"CURRENT: no tagged operation active on tracked threads");
        }

        const ULONGLONG lastInput = g_lastInputTick.load(std::memory_order_acquire);
        const ULONGLONG lastOutput = g_lastOutputTick.load(std::memory_order_acquire);
        const ULONGLONG inputAge = lastInput && now >= lastInput ? now - lastInput : (std::numeric_limits<ULONGLONG>::max)();
        const ULONGLONG outputAge = lastOutput && now >= lastOutput ? now - lastOutput : (std::numeric_limits<ULONGLONG>::max)();
        const std::wstring inputAgeText = inputAge == (std::numeric_limits<ULONGLONG>::max)()
            ? L"never" : std::to_wstring(inputAge / 1000) + L"s ago";
        const std::wstring outputAgeText = outputAge == (std::numeric_limits<ULONGLONG>::max)()
            ? L"never" : std::to_wstring(outputAge / 1000) + L"s ago";
        wchar_t progress[220]{};
        swprintf_s(progress, L"PROGRESS: input #%lld %ls; output #%lld %ls",
            g_lastInputFrame.load(std::memory_order_acquire), inputAgeText.c_str(),
            g_lastOutputFrame.load(std::memory_order_acquire), outputAgeText.c_str());
        lines.emplace_back(progress);

        const int sourceWaiters = g_sourceWaiterCount.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitStart = g_sourceWaitStartedTick.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitAge = sourceWaitStart && now >= sourceWaitStart ? now - sourceWaitStart : 0;
        const long long lowestRequested = g_sourceWaitLowestRequested.load(std::memory_order_acquire);
        const long long highestRequested = g_sourceWaitHighestRequested.load(std::memory_order_acquire);
        const long long cacheBase = g_sourceWaitCacheBase.load(std::memory_order_acquire);
        const long long cacheEnd = g_sourceWaitCacheEnd.load(std::memory_order_acquire);
        const unsigned long long generation = g_sourceWaitGeneration.load(std::memory_order_acquire);
        wchar_t sourceWait[240]{};
        if (sourceWaiters > 0) {
            swprintf_s(sourceWait, L"SOURCE WAIT: %d worker(s) | requested %lld..%lld | cache %lld..%lld | %.1fs | gen %llu",
                sourceWaiters, lowestRequested, highestRequested, cacheBase, cacheEnd,
                static_cast<double>(sourceWaitAge) / 1000.0, generation);
        } else {
            swprintf_s(sourceWait, L"SOURCE WAIT: none | cache %lld..%lld | gen %llu",
                cacheBase, cacheEnd, generation);
        }
        lines.emplace_back(sourceWait);

        std::lock_guard<std::mutex> lock(g_workContextMutex);
        if (!g_lastOperation.empty()) {
            const ULONGLONG completedAgo = g_lastOperationCompletedTick && now >= g_lastOperationCompletedTick
                ? now - g_lastOperationCompletedTick : 0;
            wchar_t last[240]{};
            swprintf_s(last, L"LAST: %ls | %.1f s | completed %.1f s ago | thread %lu",
                g_lastOperation.c_str(), static_cast<double>(g_lastOperationDurationMs) / 1000.0,
                static_cast<double>(completedAgo) / 1000.0, g_lastOperationThreadId);
            lines.emplace_back(last);
        } else {
            lines.emplace_back(L"LAST: no completed operation recorded");
        }
    } catch (...) {
        // Keep the seven menu rows stable even when a diagnostic string allocation fails.
        lines.clear();
        try {
            lines = { L"GRAPH: details unavailable", L"SEEK HANDOFF: unavailable",
                L"THROUGHPUT: unavailable", L"CURRENT: details unavailable",
                L"PROGRESS: details unavailable", L"SOURCE WAIT: details unavailable",
                L"LAST: details unavailable" };
        } catch (...) {}
    }
    try {
        while (lines.size() < kTrayActivityRowCount) lines.emplace_back(L"Diagnostic row unavailable");
        if (lines.size() > kTrayActivityRowCount) lines.resize(kTrayActivityRowCount);
    } catch (...) {
        // RefreshOpenTrayMenu checks the row count and simply skips an incomplete snapshot.
        lines.clear();
    }
    return lines;
}

void RefreshOpenTrayMenu(HWND hwnd) noexcept {
    HMENU menu = g_openTrayMenu;
    if (!menu) return;
    try {
        const auto lines = BuildActivityDetails();
        if (lines.size() != kTrayActivityRowCount) return;
        for (size_t i = 0; i < kTrayActivityRowCount; ++i) {
            std::wstring clipped = lines[i].size() > 180
                ? lines[i].substr(0, 177) + L"..." : lines[i];
            MENUITEMINFOW row{};
            row.cbSize = sizeof(row);
            row.fMask = MIIM_STRING;
            row.dwTypeData = clipped.data();
            row.cch = static_cast<UINT>(clipped.size());
            if (!SetMenuItemInfoW(menu, kTrayActivityFirstPosition + static_cast<UINT>(i), TRUE, &row)) {
                if (!g_menuRefreshFailureLogged) {
                    g_menuRefreshFailureLogged = true;
                    DirectMotionLog("WARN", "Tray/Menu", "Live activity row refresh failed; Win32=" +
                        std::to_string(GetLastError()));
                }
                return;
            }
        }
        MENUITEMINFOW toggle{};
        toggle.cbSize = sizeof(toggle);
        toggle.fMask = MIIM_STATE;
        toggle.fState = g_interpolationEnabled.load(std::memory_order_acquire) ? MFS_CHECKED : MFS_UNCHECKED;
        SetMenuItemInfoW(menu, kTrayToggleInterpolationCommand, FALSE, &toggle);
        // Force a repaint of the currently tracked native popup when it belongs to
        // this tray thread. The owner-window DrawMenuBar call is retained as a safe
        // fallback for shell/menu implementations that route invalidation there.
        HWND popup = FindWindowW(L"#32768", nullptr);
        if (popup) {
            DWORD popupProcess = 0;
            const DWORD popupThread = GetWindowThreadProcessId(popup, &popupProcess);
            if (popupThread == g_trayThreadId && popupProcess == GetCurrentProcessId()) {
                (void)RedrawWindow(popup, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
        }
        if (hwnd) (void)DrawMenuBar(hwnd);
    } catch (...) {
        if (!g_menuRefreshFailureLogged) {
            g_menuRefreshFailureLogged = true;
            DirectMotionLog("WARN", "Tray/Menu", "Live activity row refresh raised an exception.");
        }
    }
}

void UpdateTrayIcon() noexcept;
void ReportLongRunningWorkOnce() noexcept;

VOID CALLBACK TrayMenuRefreshTimerProc(HWND /*timerWindow*/, UINT /*message*/,
                                       UINT_PTR /*timerId*/, DWORD /*tick*/) {
    // A NULL-HWND timer with a callback is dispatched by the thread's modal
    // message loop, including TrackPopupMenu. A normal window WM_TIMER can be
    // filtered by menu-loop message handling on some Windows configurations.
    const HWND hwnd = g_trayWindow.load(std::memory_order_acquire);
    ReportLongRunningWorkOnce();
    UpdateTrayIcon();
    RefreshOpenTrayMenu(hwnd);
}

void ReportLongRunningWorkOnce() noexcept {
    try {
        const ULONGLONG now = GetTickCount64();
        std::vector<ActiveWorkRecord> warnings;
        {
            std::lock_guard<std::mutex> lock(g_workContextMutex);
            for (auto& item : g_activeWorkByThread) {
                std::vector<ActiveWorkRecord>& stack = item.second;
                if (stack.empty()) continue;
                ActiveWorkRecord& record = stack.back();
                if (record.watchdogReported || !record.startedTick || now < record.startedTick)
                    continue;
                const ULONGLONG elapsed = now - record.startedTick;
                if (elapsed < WorkPhaseWatchdogThreshold(record.phase)) continue;
                // Copy first; if allocation fails, leave the record eligible for the
                // next watchdog tick rather than permanently suppressing its warning.
                warnings.push_back(record);
                record.watchdogReported = true;
            }
        }

        for (const ActiveWorkRecord& record : warnings) {
            const ULONGLONG elapsed = now >= record.startedTick ? now - record.startedTick : 0;
            const std::wstring operation = record.operation.empty()
                ? WorkPhaseName(record.phase) : record.operation;
            DirectMotionLog("WARN", "Tray/Watchdog",
                "A DirectMotion operation is still executing beyond its diagnostic threshold; phase=" +
                std::to_string(static_cast<int>(record.phase)) + ", operation=" + WideToUtf8(operation) +
                ", elapsedMs=" + std::to_string(elapsed) +
                ", sequence=" + std::to_string(record.sequence) +
                ", inputFrame=" + std::to_string(record.inputFrame) +
                ", outputFrame=" + std::to_string(record.outputFrame) +
                ", threadId=" + std::to_string(record.threadId) +
                ", activeTaggedThreads=" + std::to_string(SnapshotActiveWork().activeCount) +
                ", sourceWaiters=" + std::to_string(g_sourceWaiterCount.load(std::memory_order_acquire)) +
                ", sourceWaitRequest=" + std::to_string(g_sourceWaitLowestRequested.load(std::memory_order_acquire)) +
                ".." + std::to_string(g_sourceWaitHighestRequested.load(std::memory_order_acquire)) +
                ", sourceCache=" + std::to_string(g_sourceWaitCacheBase.load(std::memory_order_acquire)) +
                ".." + std::to_string(g_sourceWaitCacheEnd.load(std::memory_order_acquire)) +
                ", sourceGeneration=" + std::to_string(g_sourceWaitGeneration.load(std::memory_order_acquire)) +
                ". This is a long-running call, not proof of a deadlock.");
        }
    } catch (...) {
    }
}
HICON MakeStatusIcon(COLORREF color) noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 16;
    info.bmiHeader.biHeight = -16;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colorBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (screen) ReleaseDC(nullptr, screen);
    if (!colorBitmap || !bits) {
        if (colorBitmap) DeleteObject(colorBitmap);
        return nullptr;
    }

    auto* pixels = static_cast<DWORD*>(bits);
    const int cx = 7, cy = 7;
    const COLORREF outline = RGB(30, 30, 30);
    const int red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
    const int borderR = GetRValue(outline), borderG = GetGValue(outline), borderB = GetBValue(outline);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int dx = x - cx, dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            DWORD px = 0;
            if (d2 <= 36) {
                px = 0xFF000000u | (static_cast<DWORD>(red) << 16) |
                     (static_cast<DWORD>(green) << 8) | static_cast<DWORD>(blue);
            } else if (d2 <= 49) {
                px = 0xFF000000u | (static_cast<DWORD>(borderR) << 16) |
                     (static_cast<DWORD>(borderG) << 8) | static_cast<DWORD>(borderB);
            }
            pixels[y * 16 + x] = px;
        }
    }
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, nullptr);
    if (!mask) { DeleteObject(colorBitmap); return nullptr; }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(colorBitmap);
    DeleteObject(mask);
    return icon;
}

void UpdateTrayIcon() noexcept {
    HWND hwnd = g_trayWindow.load();
    if (!hwnd) return;
    const TrayVisualState visual = BuildTrayVisualState();
    const bool iconChanged = !g_iconAdded || visual.color != g_lastIconColor || !g_currentIcon;
    HICON replacement = iconChanged ? MakeStatusIcon(visual.color) : g_currentIcon;
    if (!replacement) {
        DirectMotionLog("ERROR", "Tray", "MakeStatusIcon failed while updating tray state.");
        return;
    }
    if (g_iconAdded && !iconChanged && visual.tooltip == g_lastTooltip) return;
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayIconId;
    g_nid.uFlags = NIF_MESSAGE | NIF_TIP | (iconChanged ? NIF_ICON : 0);
    g_nid.uCallbackMessage = kTrayCallbackMessage;
    g_nid.hIcon = replacement;
    wcsncpy_s(g_nid.szTip, _countof(g_nid.szTip), visual.tooltip.c_str(), _TRUNCATE);
    const BOOL ok = Shell_NotifyIconW(g_iconAdded ? NIM_MODIFY : NIM_ADD, &g_nid);
    if (ok) {
        const bool wasAdded = g_iconAdded;
        g_iconAdded = true;
        if (hwnd) KillTimer(hwnd, kTrayRetryTimerId);
        if (!wasAdded) {
            g_nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        }
        if (iconChanged) {
            if (g_currentIcon) DestroyIcon(g_currentIcon);
            g_currentIcon = replacement;
            g_lastIconColor = visual.color;
        }
        g_lastTooltip = visual.tooltip;
        if (!wasAdded) DirectMotionLog("INFO", "Tray", "Tray icon added: " + WideToUtf8(visual.tooltip));
    } else {
        const DWORD error = GetLastError();
        if (iconChanged) DestroyIcon(replacement);
        // Explorer may not be ready when the filter is constructed, or may have
        // restarted. Retry on the tray thread instead of silently losing the icon.
        if (hwnd && !g_iconAdded) {
            if (!SetTimer(hwnd, kTrayRetryTimerId, kTrayRetryIntervalMs, nullptr)) {
                DirectMotionLog("ERROR", "Tray", "SetTimer failed while scheduling tray icon retry; Win32=" +
                    std::to_string(GetLastError()));
            }
        }
        DirectMotionLog("ERROR", "Tray", "Shell_NotifyIcon failed while updating/adding the status icon; Win32=" +
            std::to_string(error));
    }
}

void PersistInterpolationSetting(bool enabled) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\directmotion", 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        DirectMotionLog("WARN", "Tray/Interpolation", "Could not open the settings registry key to persist Enabled.");
        return;
    }
    const DWORD value = enabled ? 1u : 0u;
    const LONG result = RegSetValueExW(key, L"Enabled", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        DirectMotionLog("WARN", "Tray/Interpolation", "Could not persist Enabled registry value; Win32=" + std::to_string(result));
    }
}

void ShowTrayMenu(HWND hwnd) noexcept {
    if (g_openTrayMenu) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        DirectMotionLog("ERROR", "Tray/Menu", "CreatePopupMenu failed; Win32=" + std::to_string(GetLastError()));
        return;
    }
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
    item.wID = kTrayToggleInterpolationCommand;
    item.dwTypeData = const_cast<LPWSTR>(L"Enable interpolation");
    item.fState = g_interpolationEnabled.load() ? MFS_CHECKED : MFS_UNCHECKED;
    if (!InsertMenuItemW(menu, 0, TRUE, &item)) {
        DirectMotionLog("ERROR", "Tray/Menu", "InsertMenuItem failed; Win32=" + std::to_string(GetLastError()));
        DestroyMenu(menu);
        return;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::vector<std::wstring> runtimeInfo;
    {
        std::lock_guard<std::mutex> lock(g_runtimeInfoMutex);
        runtimeInfo = g_runtimeInfo;
    }
    if (runtimeInfo.empty()) runtimeInfo.emplace_back(L"Runtime: waiting for first stream sample");
    const auto activityDetails = BuildActivityDetails();
    for (size_t i = 0; i < kTrayActivityRowCount; ++i) {
        const std::wstring& line = activityDetails[i];
        std::wstring clipped = line.size() > 180 ? line.substr(0, 177) + L"..." : line;
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, clipped.c_str());
    }
    for (const auto& line : runtimeInfo) {
        std::wstring clipped = line.size() > 180 ? line.substr(0, 177) + L"..." : line;
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, clipped.c_str());
    }

    g_openTrayMenu = menu;
    g_menuRefreshFailureLogged = false;
    g_openTrayMenuTimer = SetTimer(nullptr, kTrayMenuRefreshTimerId,
        kTrayMenuRefreshIntervalMs, TrayMenuRefreshTimerProc);
    if (!g_openTrayMenuTimer) {
        DirectMotionLog("WARN", "Tray/Menu", "Could not start the modal-loop live refresh timer; falling back to the tray window timer. Win32=" +
            std::to_string(GetLastError()));
    }
    SetForegroundWindow(hwnd);
    POINT cursor{};
    GetCursorPos(&cursor);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, hwnd, nullptr);
    if (g_openTrayMenuTimer) {
        KillTimer(nullptr, g_openTrayMenuTimer);
        g_openTrayMenuTimer = 0;
    }
    g_openTrayMenu = nullptr;
    if (command == kTrayToggleInterpolationCommand) {
        const bool enabled = !g_interpolationEnabled.load();
        DirectMotionTraySetInterpolationEnabled(enabled);
        PersistInterpolationSetting(enabled);
        DirectMotionTraySetStatus(DirectMotionStatus::Standby);
        DirectMotionLog("INFO", "Tray/Interpolation", std::string("Interpolation preference changed from tray to ") +
            (enabled ? "enabled" : "disabled") + "; it is applied at the next media sample boundary.");
    }
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
        DirectMotionLog("INFO", "Tray", "Explorer TaskbarCreated message received; re-adding status icon.");
        g_iconAdded = false;
        SetTimer(hwnd, kTrayRetryTimerId, kTrayRetryIntervalMs, nullptr);
        UpdateTrayIcon();
        return 0;
    }
    if (message == WM_TIMER && wParam == kTrayRetryTimerId) {
        if (!g_iconAdded) {
            DirectMotionLog("INFO", "Tray", "Retrying Shell_NotifyIcon NIM_ADD after a previous failure.");
            UpdateTrayIcon();
        } else {
            KillTimer(hwnd, kTrayRetryTimerId);
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kTrayStatusTimerId) {
        ReportLongRunningWorkOnce();
        UpdateTrayIcon();
        RefreshOpenTrayMenu(hwnd);
        return 0;
    }
    if (message == kTrayCallbackMessage) {
        // With NOTIFYICON_VERSION_4 the event code is packed into LOWORD(lParam).
        const UINT eventCode = (g_nid.uVersion == NOTIFYICON_VERSION_4)
            ? LOWORD(lParam) : static_cast<UINT>(lParam);
        if (eventCode == WM_RBUTTONUP || eventCode == WM_CONTEXTMENU ||
            eventCode == WM_LBUTTONUP || eventCode == WM_LBUTTONDBLCLK ||
            eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    }
    if (message == WM_DESTROY) return 0;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

DWORD WINAPI TrayThreadProc(void*) noexcept {
    // Force creation of the thread message queue before the owner can post WM_QUIT.
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(reinterpret_cast<ULONG_PTR>(&TrayThreadProc)), &module);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWindowProc;
    wc.hInstance = module;
    wc.lpszClassName = kWindowClass;
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    const ATOM classAtom = RegisterClassExW(&wc);
    if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DirectMotionLog("ERROR", "Tray", "RegisterClassExW failed; Win32=" + std::to_string(GetLastError()));
    }
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"directmotion status", 0,
                                0, 0, 0, 0, nullptr, nullptr, module, nullptr);
    g_trayWindow.store(hwnd);
    if (!hwnd) DirectMotionLog("ERROR", "Tray", "CreateWindowExW failed; Win32=" + std::to_string(GetLastError()));
    if (hwnd) {
        g_nid = {};
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = kTrayIconId;
        g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        g_nid.uCallbackMessage = kTrayCallbackMessage;
        DirectMotionLog("INFO", "Tray", "Attempting initial tray icon registration.");
        if (!SetTimer(hwnd, kTrayRetryTimerId, kTrayRetryIntervalMs, nullptr)) {
            DirectMotionLog("ERROR", "Tray/Timer", "Could not schedule initial tray icon retry timer; Win32=" +
                std::to_string(GetLastError()));
        }
        if (!SetTimer(hwnd, kTrayStatusTimerId, kTrayStatusIntervalMs, nullptr)) {
            DirectMotionLog("ERROR", "Tray/Timer", "Could not schedule phase/status refresh timer; tray details may not refresh; Win32=" +
                std::to_string(GetLastError()));
        }
        UpdateTrayIcon();
    }
    if (g_trayReadyEvent) SetEvent(g_trayReadyEvent);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kTrayRefreshMessage) {
            UpdateTrayIcon();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DirectMotionLog("INFO", "Tray", "Tray icon removed because the last filter instance released the tray service.");
    }
    g_iconAdded = false;
    if (g_currentIcon) { DestroyIcon(g_currentIcon); g_currentIcon = nullptr; }
    if (hwnd) DestroyWindow(hwnd);
    g_trayWindow.store(nullptr);
    UnregisterClassW(kWindowClass, module);
    return 0;
}
} // namespace

std::wstring DirectMotionModuleDirectory() noexcept {
    try {
        HMODULE module = nullptr;
        const auto address = reinterpret_cast<LPCWSTR>(
            reinterpret_cast<ULONG_PTR>(&DirectMotionModuleDirectory));
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                address, &module)) return {};
        std::wstring fullPath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, fullPath.data(),
                                                static_cast<DWORD>(fullPath.size()));
        if (length == 0 || length >= fullPath.size()) return {};
        fullPath.resize(length);
        const size_t slash = fullPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        fullPath.resize(slash);
        return fullPath;
    } catch (...) {
        return {};
    }
}

void DirectMotionLog(const char* level, const char* component, const std::string& message) noexcept {
    EnsureLogPreferenceLoaded();
    if (!g_logEnabled.load()) return;
    try {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char prefix[256]{};
        std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] [%s] [%s] ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(), level ? level : "INFO", component ? component : "core");
        std::string line(prefix);
        line += message;
        line += "\r\n";
        const std::wstring directory = DirectMotionModuleDirectory();
        if (!directory.empty()) {
            const std::wstring path = directory + L"\\directmotion.log";
            std::lock_guard<std::mutex> lock(g_logMutex);
            HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const BOOL writeOk = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
                const DWORD writeError = writeOk ? ERROR_SUCCESS : GetLastError();
                CloseHandle(file);
                if (!writeOk || static_cast<size_t>(written) != line.size()) {
                    char failure[256]{};
                    std::snprintf(failure, sizeof(failure), "[directmotion] Could not fully append directmotion.log (error=%lu, bytes=%lu/%zu).\n",
                                  writeError, written, line.size());
                    if (g_logEnabled.load()) OutputDebugStringA(failure);
                }
            } else {
                const DWORD openError = GetLastError();
                wchar_t failure[1024]{};
                _snwprintf_s(failure, _countof(failure), _TRUNCATE,
                    L"[directmotion] Cannot open adjacent log %s (Win32 error %lu).\n",
                    path.c_str(), openError);
                if (g_logEnabled.load()) OutputDebugStringW(failure);
            }
        }
        if (g_logEnabled.load()) OutputDebugStringA(line.c_str());
    } catch (...) {
        if (g_logEnabled.load()) OutputDebugStringA("[directmotion] Logging failed.\n");
    }
}

void DirectMotionLogW(const char* level, const char* component, const std::wstring& message) noexcept {
    try { DirectMotionLog(level, component, WideToUtf8(message)); }
    catch (...) { if (DirectMotionIsLoggingEnabled()) OutputDebugStringA("[directmotion] Wide logging failed.\n"); }
}

void DirectMotionSetLoggingEnabled(bool enabled) noexcept {
    EnsureLogPreferenceLoaded();
    g_logEnabled.store(enabled);
}

bool DirectMotionIsLoggingEnabled() noexcept {
    EnsureLogPreferenceLoaded();
    return g_logEnabled.load();
}

void DirectMotionTraySetInterpolationEnabled(bool enabled) noexcept {
    const bool previous = g_interpolationEnabled.exchange(enabled);
    if (previous == enabled) return;
    HWND hwnd = g_trayWindow.load();
    if (hwnd) PostMessageW(hwnd, kTrayRefreshMessage, 0, 0);
}

bool DirectMotionTrayInterpolationEnabled() noexcept {
    return g_interpolationEnabled.load();
}

void DirectMotionTraySetRuntimeInfo(const std::vector<std::wstring>& lines) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_runtimeInfoMutex);
        g_runtimeInfo = lines;
        if (g_runtimeInfo.size() > 13) g_runtimeInfo.resize(13);
    } catch (...) {
    }
}

void DirectMotionTraySetRuntimeInfoRows(size_t startIndex, const std::vector<std::wstring>& lines) noexcept {
    try {
        constexpr size_t kMaxRuntimeRows = 13;
        if (startIndex >= kMaxRuntimeRows || lines.empty()) return;
        std::lock_guard<std::mutex> lock(g_runtimeInfoMutex);
        const size_t count = (std::min)(lines.size(), kMaxRuntimeRows - startIndex);
        const size_t required = startIndex + count;
        if (g_runtimeInfo.size() < required) g_runtimeInfo.resize(required);
        for (size_t i = 0; i < count; ++i) g_runtimeInfo[startIndex + i] = lines[i];
        if (g_runtimeInfo.size() > kMaxRuntimeRows) g_runtimeInfo.resize(kMaxRuntimeRows);
    } catch (...) {
    }
}

unsigned long long DirectMotionTrayGetActiveWorkCount() noexcept {
    return static_cast<unsigned long long>(SnapshotActiveWork().activeCount);
}

int DirectMotionTrayGetSourceWaiterCount() noexcept {
    return g_sourceWaiterCount.load(std::memory_order_acquire);
}

void DirectMotionTraySetSourceFrameExpired(bool expired) noexcept {
    g_sourceFrameExpired.store(expired, std::memory_order_release);
}

bool DirectMotionTraySourceFrameExpired() noexcept {
    return g_sourceFrameExpired.load(std::memory_order_acquire);
}

void DirectMotionTraySetSourceCacheProducerWaiting(bool waiting) noexcept {
    g_sourceCacheProducerWaiting.store(waiting, std::memory_order_release);
}

bool DirectMotionTraySourceCacheProducerWaiting() noexcept {
    return g_sourceCacheProducerWaiting.load(std::memory_order_acquire);
}

std::string DirectMotionTrayGetDiagnosticContext() noexcept {
    try {
        const ULONGLONG now = GetTickCount64();
        std::string result;
        result.reserve(1400);
        {
            std::lock_guard<std::mutex> lock(g_workContextMutex);
            for (const auto& item : g_activeWorkByThread) {
                if (item.second.empty()) continue;
                const ActiveWorkRecord& record = item.second.back();
                const ULONGLONG ageMs = record.startedTick && now >= record.startedTick
                    ? now - record.startedTick : 0;
                const std::string operation = WideToUtf8(record.operation);
                if (!result.empty()) result += " | ";
                result += "tid=" + std::to_string(record.threadId) +
                    ",phase=" + WideToUtf8(WorkPhaseName(record.phase)) +
                    ",ageMs=" + std::to_string(ageMs) +
                    ",input=" + std::to_string(record.inputFrame) +
                    ",output=" + std::to_string(record.outputFrame) +
                    ",operation=" + operation;
                if (result.size() >= 1200) {
                    result.resize(1200);
                    result += "...";
                    break;
                }
            }
        }

        const int sourceWaiters = g_sourceWaiterCount.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitStart = g_sourceWaitStartedTick.load(std::memory_order_acquire);
        const ULONGLONG sourceWaitAge = sourceWaitStart && now >= sourceWaitStart
            ? now - sourceWaitStart : 0;
        result += result.empty() ? "sourceWaiters=" : " | sourceWaiters=";
        result += std::to_string(sourceWaiters) +
            ",sourceWaitAgeMs=" + std::to_string(sourceWaitAge) +
            ",sourceRequested=" + std::to_string(g_sourceWaitLowestRequested.load(std::memory_order_acquire)) +
            ".." + std::to_string(g_sourceWaitHighestRequested.load(std::memory_order_acquire)) +
            ",sourceCache=" + std::to_string(g_sourceWaitCacheBase.load(std::memory_order_acquire)) +
            ".." + std::to_string(g_sourceWaitCacheEnd.load(std::memory_order_acquire)) +
            ",sourceGeneration=" + std::to_string(g_sourceWaitGeneration.load(std::memory_order_acquire));
        if (result.size() > 1800) result.resize(1800);
        return result;
    } catch (...) {
        return "diagnosticContext=unavailable";
    }
}

void DirectMotionTrayClearCurrentThreadWork() noexcept {
    try {
        const DWORD threadId = GetCurrentThreadId();
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_workContextMutex);
        const auto found = g_activeWorkByThread.find(threadId);
        if (found == g_activeWorkByThread.end() || found->second.empty()) return;
        ActiveWorkRecord completed = std::move(found->second.back());
        g_activeWorkByThread.erase(found);
        g_lastOperation = completed.operation.empty()
            ? WorkPhaseName(completed.phase) : std::move(completed.operation);
        g_lastOperationDurationMs = completed.startedTick && now >= completed.startedTick
            ? now - completed.startedTick : 0;
        g_lastOperationThreadId = completed.threadId;
        g_lastOperationCompletedTick = now;
    } catch (...) {
    }
}

void DirectMotionTraySetWorkPhase(DirectMotionWorkPhase phase) noexcept {
    DirectMotionTraySetWorkPhase(phase, phase == DirectMotionWorkPhase::Idle ? nullptr : "unspecified stage", -1, -1);
}

void DirectMotionTraySetWorkPhase(DirectMotionWorkPhase phase, const char* operation,
                                  long long inputFrame, long long outputFrame) noexcept {
    try {
        const DWORD threadId = GetCurrentThreadId();
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_workContextMutex);
        if (phase == DirectMotionWorkPhase::Idle) {
            const auto found = g_activeWorkByThread.find(threadId);
            if (found == g_activeWorkByThread.end() || found->second.empty()) return;

            std::vector<ActiveWorkRecord>& stack = found->second;
            ActiveWorkRecord completed = std::move(stack.back());
            stack.pop_back();
            if (stack.empty()) g_activeWorkByThread.erase(found);
            g_lastOperation = completed.operation.empty()
                ? WorkPhaseName(completed.phase) : std::move(completed.operation);
            g_lastOperationDurationMs = completed.startedTick && now >= completed.startedTick
                ? now - completed.startedTick : 0;
            g_lastOperationThreadId = completed.threadId;
            g_lastOperationCompletedTick = now;
            return;
        }

        ActiveWorkRecord record;
        record.phase = phase;
        record.operation = Utf8ToWide(operation);
        record.inputFrame = inputFrame;
        record.outputFrame = outputFrame;
        record.threadId = threadId;
        record.startedTick = now;
        record.sequence = ++g_workSequence;
        g_activeWorkByThread[threadId].push_back(std::move(record));
    } catch (...) {
    }
}

void DirectMotionTraySetSourceWaitInfo(int waiters, long long lowestRequested,
                                        long long highestRequested, long long cacheBase,
                                        long long cacheEnd, unsigned long long generation) noexcept {
    if (waiters < 0) waiters = 0;
    if (waiters == 0) {
        // Publish the zero count first so readers never interpret cleared ranges as
        // a live wait. The cache range/generation remain useful while idle.
        g_sourceWaiterCount.store(0, std::memory_order_release);
        g_sourceWaitLowestRequested.store(-1, std::memory_order_release);
        g_sourceWaitHighestRequested.store(-1, std::memory_order_release);
        g_sourceWaitStartedTick.store(0, std::memory_order_release);
        g_sourceWaitCacheBase.store(cacheBase, std::memory_order_release);
        g_sourceWaitCacheEnd.store(cacheEnd, std::memory_order_release);
        g_sourceWaitGeneration.store(generation, std::memory_order_release);
        return;
    }

    g_sourceWaitLowestRequested.store(lowestRequested, std::memory_order_release);
    g_sourceWaitHighestRequested.store(highestRequested, std::memory_order_release);
    g_sourceWaitCacheBase.store(cacheBase, std::memory_order_release);
    g_sourceWaitCacheEnd.store(cacheEnd, std::memory_order_release);
    g_sourceWaitGeneration.store(generation, std::memory_order_release);
    ULONGLONG expected = 0;
    (void)g_sourceWaitStartedTick.compare_exchange_strong(expected, GetTickCount64(),
        std::memory_order_acq_rel, std::memory_order_acquire);
    g_sourceWaiterCount.store(waiters, std::memory_order_release);
}

void DirectMotionTrayRecordInput(long long inputFrame) noexcept {
    g_lastInputFrame.store(inputFrame, std::memory_order_release);
    g_lastInputTick.store(GetTickCount64(), std::memory_order_release);
}

void DirectMotionTrayRecordOutput(long long outputFrame) noexcept {
    const ULONGLONG now = GetTickCount64();
    g_lastOutputFrame.store(outputFrame, std::memory_order_release);
    g_lastOutputTick.store(now, std::memory_order_release);

    if (g_targetOutputFps.load(std::memory_order_acquire) <= 1.0) return;
    const ULONGLONG windowStart = g_throughputWindowStartTick.load(std::memory_order_acquire);
    if (windowStart == 0) {
        g_throughputWindowStartFrame.store(outputFrame, std::memory_order_release);
        g_throughputWindowStartTick.store(now, std::memory_order_release);
        return;
    }
    if (now < windowStart || now - windowStart < kThroughputWindowMs) return;

    const long long startFrame = g_throughputWindowStartFrame.load(std::memory_order_acquire);
    const ULONGLONG elapsed = now - windowStart;
    const long long delivered = (std::max)(0ll, outputFrame - startFrame);
    const double measured = elapsed > 0
        ? static_cast<double>(delivered) * 1000.0 / static_cast<double>(elapsed) : 0.0;
    g_measuredOutputFps.store(measured, std::memory_order_release);
    g_throughputWindowStartFrame.store(outputFrame, std::memory_order_release);
    g_throughputWindowStartTick.store(now, std::memory_order_release);

    const double target = g_targetOutputFps.load(std::memory_order_acquire);
    DirectMotionLog("INFO", "Tray/Throughput", "Delivered output rate=" +
        std::to_string(measured) + " FPS, target=" + std::to_string(target) +
        " FPS, ratio=" + std::to_string(target > 0.0 ? measured / target : 0.0) +
        ", frames=" + std::to_string(outputFrame) +
        ", windowMs=" + std::to_string(elapsed));
    if (target > 1.0 && measured < target * kThroughputWarningRatio) {
        ULONGLONG expected = 0;
        g_throughputBelowSinceTick.compare_exchange_strong(expected, now,
            std::memory_order_acq_rel, std::memory_order_acquire);
    } else {
        g_throughputBelowSinceTick.store(0, std::memory_order_release);
    }
}

void DirectMotionTraySetTargetFps(double targetFps) noexcept {
    if (!(targetFps > 1.0) || !std::isfinite(targetFps)) targetFps = 0.0;
    g_targetOutputFps.store(targetFps, std::memory_order_release);
    g_measuredOutputFps.store(0.0, std::memory_order_release);
    g_throughputBelowSinceTick.store(0, std::memory_order_release);
    g_throughputWindowStartFrame.store(g_lastOutputFrame.load(std::memory_order_acquire),
        std::memory_order_release);
    g_throughputWindowStartTick.store(0, std::memory_order_release);
}

void DirectMotionTrayResetThroughput() noexcept {
    g_measuredOutputFps.store(0.0, std::memory_order_release);
    g_throughputBelowSinceTick.store(0, std::memory_order_release);
    g_throughputWindowStartFrame.store(g_lastOutputFrame.load(std::memory_order_acquire),
        std::memory_order_release);
    g_throughputWindowStartTick.store(0, std::memory_order_release);
}

void DirectMotionTraySetGraphRunning(bool running) noexcept {
    const bool previous = g_graphRunning.exchange(running, std::memory_order_acq_rel);
    if (previous == running) return;
    g_measuredOutputFps.store(0.0, std::memory_order_release);
    g_throughputBelowSinceTick.store(0, std::memory_order_release);
    g_throughputWindowStartFrame.store(g_lastOutputFrame.load(std::memory_order_acquire),
        std::memory_order_release);
    g_throughputWindowStartTick.store(0, std::memory_order_release);
    HWND hwnd = g_trayWindow.load();
    if (hwnd) PostMessageW(hwnd, kTrayRefreshMessage, 0, 0);
}

void DirectMotionTrayAcquire() noexcept {
    std::lock_guard<std::mutex> lock(g_trayLifecycleMutex);
    if (g_trayReferences++ > 0) {
        DirectMotionLog("INFO", "Tray", "Tray service reference acquired; references=" + std::to_string(g_trayReferences));
        return;
    }
    DirectMotionLog("INFO", "Tray", "Starting in-process tray service thread.");
    g_trayReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_trayReadyEvent) {
        DirectMotionLog("ERROR", "Tray", "CreateEventW failed; Win32=" + std::to_string(GetLastError()));
        g_trayReferences = 0; return;
    }
    g_trayThread = CreateThread(nullptr, 0, TrayThreadProc, nullptr, 0, &g_trayThreadId);
    if (!g_trayThread) {
        DirectMotionLog("ERROR", "Tray", "CreateThread failed; Win32=" + std::to_string(GetLastError()));
        CloseHandle(g_trayReadyEvent);
        g_trayReadyEvent = nullptr;
        g_trayReferences = 0;
        return;
    }
    const DWORD waitResult = WaitForSingleObject(g_trayReadyEvent, 5000);
    DirectMotionLog("INFO", "Tray", "Tray thread readiness wait returned=" + std::to_string(waitResult) +
        ", windowCreated=" + std::to_string(g_trayWindow.load() ? 1 : 0) +
        ", iconAdded=" + std::to_string(g_iconAdded ? 1 : 0));
}

void DirectMotionTrayRelease() noexcept {
    std::lock_guard<std::mutex> lock(g_trayLifecycleMutex);
    if (g_trayReferences <= 0) return;
    if (--g_trayReferences > 0) {
        DirectMotionLog("INFO", "Tray", "Tray service reference released; references=" + std::to_string(g_trayReferences));
        return;
    }
    DirectMotionLog("INFO", "Tray", "Last filter instance releasing tray service.");
    if (g_trayThread && g_trayThreadId) PostThreadMessageW(g_trayThreadId, WM_QUIT, 0, 0);
    if (g_trayThread) {
        WaitForSingleObject(g_trayThread, INFINITE);
        CloseHandle(g_trayThread);
        g_trayThread = nullptr;
    }
    if (g_trayReadyEvent) {
        CloseHandle(g_trayReadyEvent);
        g_trayReadyEvent = nullptr;
    }
    g_trayThreadId = 0;
}

void DirectMotionTraySetStatus(DirectMotionStatus status) noexcept {
    const int previous = g_status.exchange(static_cast<int>(status));
    if (previous == static_cast<int>(status)) return;
    const wchar_t* text = StatusText(status);
    DirectMotionLog("INFO", "Tray", "Status changed: " + WideToUtf8(text));
    HWND hwnd = g_trayWindow.load();
    if (hwnd) PostMessageW(hwnd, kTrayRefreshMessage, 0, 0);
}

void DirectMotionTraySetSeekHandoffState(
    DirectMotionSeekHandoffStatus::State state, uint64_t generation) noexcept {
    uint64_t observed = g_seekHandoffStatePacked.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t desired = DirectMotionSeekHandoffStatus::Update(
            observed, generation, state);
        if (desired == observed) return;
        if (g_seekHandoffStatePacked.compare_exchange_weak(
                observed, desired, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}
