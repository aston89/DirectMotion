#pragma once

// Read-only threshold policy shared by output-stage diagnostics and portable tests.
// This does not influence queue predicates, worker scheduling or delivery retries.
namespace directmotion_diagnostics {

constexpr double kSlowOutputStageWarningMs = 25.0;

inline bool ShouldWarnSlowOutputStage(double elapsedMs) noexcept {
    return elapsedMs >= kSlowOutputStageWarningMs;
}

} // namespace directmotion_diagnostics
