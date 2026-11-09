#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include "SeekHandoffStatusPolicy.h"
#include <string>
#include <vector>

enum class DirectMotionStatus : int {
    Standby = 0,
    Active = 1,
    Error = 2
};

enum class DirectMotionWorkPhase : int {
    Idle = 0,
    Backend = 1,
    Allocator = 2,
    Downstream = 3,
    InputCopy = 4,
    SourceIngest = 5,
    ReceiveLock = 6,
    OutputQueue = 7
};

std::wstring DirectMotionModuleDirectory() noexcept;
void DirectMotionLog(const char* level, const char* component, const std::string& message) noexcept;
void DirectMotionLogW(const char* level, const char* component, const std::wstring& message) noexcept;
void DirectMotionSetLoggingEnabled(bool enabled) noexcept;
bool DirectMotionIsLoggingEnabled() noexcept;
void DirectMotionTrayAcquire() noexcept;
void DirectMotionTrayRelease() noexcept;
void DirectMotionTraySetStatus(DirectMotionStatus status) noexcept;
void DirectMotionTraySetSeekHandoffState(DirectMotionSeekHandoffStatus::State state,
                                         uint64_t generation) noexcept;
void DirectMotionTraySetInterpolationEnabled(bool enabled) noexcept;
bool DirectMotionTrayInterpolationEnabled() noexcept;
void DirectMotionTraySetRuntimeInfo(const std::vector<std::wstring>& lines) noexcept;
void DirectMotionTraySetRuntimeInfoRows(size_t startIndex, const std::vector<std::wstring>& lines) noexcept;
unsigned long long DirectMotionTrayGetActiveWorkCount() noexcept;
// Bounded, read-only snapshot of active tagged operations and source-cache waiters.
// Intended for the independent diagnostic worker; never used by the media path.
std::string DirectMotionTrayGetDiagnosticContext() noexcept;
int DirectMotionTrayGetSourceWaiterCount() noexcept;
void DirectMotionTraySetSourceFrameExpired(bool expired) noexcept;
bool DirectMotionTraySourceFrameExpired() noexcept;
void DirectMotionTraySetSourceCacheProducerWaiting(bool waiting) noexcept;
bool DirectMotionTraySourceCacheProducerWaiting() noexcept;
void DirectMotionTrayClearCurrentThreadWork() noexcept;
void DirectMotionTraySetWorkPhase(DirectMotionWorkPhase phase) noexcept;
void DirectMotionTraySetWorkPhase(DirectMotionWorkPhase phase, const char* operation,
                                  long long inputFrame, long long outputFrame) noexcept;
void DirectMotionTrayRecordInput(long long inputFrame) noexcept;
void DirectMotionTrayRecordOutput(long long outputFrame) noexcept;
void DirectMotionTraySetTargetFps(double targetFps) noexcept;
void DirectMotionTraySetSourceWaitInfo(int waiters, long long lowestRequested,
                                      long long highestRequested, long long cacheBase,
                                      long long cacheEnd, unsigned long long generation) noexcept;
void DirectMotionTrayResetThroughput() noexcept;
void DirectMotionTraySetGraphRunning(bool running) noexcept;
