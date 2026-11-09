#!/usr/bin/env python3
"""Current-source static validation for DirectMotion. Historical reports/logs and archive-presence checks are kept in the development checkpoint, not release CI."""
from __future__ import annotations
import re
import sys
import math
from fractions import Fraction
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import yaml
except Exception:
    yaml = None

ROOT = Path(__file__).resolve().parents[1]
checks: list[tuple[str, bool, str]] = []

def check(name: str, condition: bool, detail: str = "") -> None:
    checks.append((name, bool(condition), detail))

def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")

def balanced_cpp(source: str) -> bool:
    stack: list[str] = []
    pairs = {")": "(", "]": "[", "}": "{"}
    state = "code"
    i = 0
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "line":
            if c == "\n": state = "code"
            i += 1; continue
        if state == "block":
            if c == "*" and n == "/": state = "code"; i += 2; continue
            i += 1; continue
        if state in ("string", "char"):
            if c == "\\": i += 2; continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
            i += 1; continue
        if c == "/" and n == "/": state = "line"; i += 2; continue
        if c == "/" and n == "*": state = "block"; i += 2; continue
        if c == '"': state = "string"; i += 1; continue
        if c == "'": state = "char"; i += 1; continue
        if c in "([{" : stack.append(c)
        elif c in ")]}":
            if not stack or stack[-1] != pairs[c]: return False
            stack.pop()
        i += 1
    return not stack and state not in ("block", "string", "char")

ver_h = read("directmotion/version.h")
ver_rc = read("directmotion/Version.rc")
cpp = read("directmotion/directmotion.cpp")
hdr = read("directmotion/directmotion.h")
engine = read("directmotion/AviSynthMvToolsEngine.cpp")
engine_h = read("directmotion/AviSynthMvToolsEngine.h")
workflow = read(".github/workflows/build-directmotion.yml")
sample_time_policy = read("directmotion/SampleTimePolicy.h")
sample_time_policy_test = read("tools/test_sample_time_policy.cpp")
workflow_upload_steps = []
try:
    workflow_document = yaml.safe_load(workflow) if yaml is not None else {}
    for job in (workflow_document or {}).get("jobs", {}).values():
        workflow_upload_steps.extend(
            step for step in job.get("steps", [])
            if str(step.get("uses", "")).startswith("actions/upload-artifact@")
        )
except Exception:
    workflow_upload_steps = []
check("AviSynth+ SDK checkout is pinned to the official v3.7.5 release tag", all(x in workflow for x in ["$avisynthTag = 'v3.7.5'", "--branch $avisynthTag", "AviSynthPlus.git"]))
check("AviSynth+ release tag is guarded by the verified 6c7c266 commit prefix", all(x in workflow for x in ["$expectedCommitPrefix = '6c7c266'", "rev-parse HEAD", "$actualCommit.StartsWith($expectedCommitPrefix", "AviSynth+ pin mismatch"]))
check("AviSynth+ header checkout is detached and validates supported avisynth.h paths", all(x in workflow for x in ["rev-parse --abbrev-ref HEAD", "$headName -ne 'HEAD'", "include\\avisynth.h", "avs_core\\include\\avisynth.h", "AVISYNTH_ROOT=$root"]))
check("GitHub Actions use the reviewed full immutable checkout v5.1.0 commit", "actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09 # v5.1.0" in workflow)
check("GitHub Actions use the reviewed full immutable setup-msbuild v3.0.0 commit", "microsoft/setup-msbuild@30375c66a4eea26614e0d39710365f22f8b0af57 # v3.0.0" in workflow)
check("GitHub Actions use the reviewed full immutable upload-artifact v7.0.2 commit", "actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2" in workflow)
uses_values = re.findall(r"^\s*uses:\s*([^\s#]+)", workflow, re.MULTILINE)
check("Every external GitHub Action reference is pinned to a full 40-character commit SHA", bool(uses_values) and all(re.fullmatch(r"[^@\s]+@[0-9a-f]{40}", value) for value in uses_values))
release_artifact_only_ax = (
    len(workflow_upload_steps) == 1
    and workflow_upload_steps[0].get("with", {}).get("path") == "bin_x64/lib/directmotion.ax"
    and workflow_upload_steps[0].get("with", {}).get("name") == "DirectMotion_x64"
)
check("GitHub Actions uploads only directmotion.ax under the DirectMotion x64 artifact name", release_artifact_only_ax, "DirectMotion_x64.zip")
base_transfrm_cpp = read("common/baseclasses/transfrm.cpp")
base_transfrm_h = read("common/baseclasses/transfrm.h")
wxutil_h = read("common/baseclasses/wxutil.h")
pause_receive_h = read("directmotion/PauseReceiveBackpressure.h")
pause_receive_test = read("tools/test_pause_receive_backpressure.cpp")
resume_warm_start_h = read("directmotion/ResumeWarmStart.h")
resume_warm_start_test = read("tools/test_resume_warm_start.cpp")
settings_cpp = read("directmotion/directmotionSettings.cpp")
runtime_diag = read("directmotion/RuntimeDiagnostics.cpp")
async_state_h = read("directmotion/AsyncPipelineState.h")
async_state_cpp = read("directmotion/AsyncPipelineState.cpp")
async_state_test = read("tools/test_async_pipeline_state.cpp")
stall_diag_h = read("directmotion/PipelineStallDiagnostics.h")
stall_diag_test = read("tools/test_pipeline_stall_diagnostics.cpp")
retention_h = read("directmotion/SourceCacheRetention.h")
retention_test = read("tools/test_source_cache_retention.cpp")
pause_fence_test = read("tools/test_pause_delivery_fence.cpp")
pause_frame_gate_h = read("directmotion/PauseFrameGate.h")
pause_frame_gate_test = read("tools/test_pause_frame_gate.cpp")
processing_continuation_h = read("directmotion/ProcessingContinuation.h")
processing_continuation_test = read("tools/test_processing_continuation.cpp")
processing_quiescence_h = read("directmotion/ProcessingQuiescence.h")
processing_quiescence_test = read("tools/test_processing_quiescence.cpp")
processing_generation_h = read("directmotion/ProcessingGenerationPolicy.h")
processing_generation_test = read("tools/test_processing_generation_policy.cpp")
manalyse_policy_h = read("directmotion/MAnalyseInvocationPolicy.h")
manalyse_policy_test = read("tools/test_manalyse_invocation_policy.cpp")
bootstrap_queue_h = read("directmotion/BootstrapQueueState.h")
bootstrap_queue_test = read("tools/test_bootstrap_queue_state.cpp")
delivery_latency_h = read("directmotion/DeliveryLatencyDiagnostics.h")
delivery_latency_test = read("tools/test_delivery_latency_diagnostics.cpp")
bootstrap_eval_h = read("directmotion/BootstrapEvaluationPolicy.h")
bootstrap_eval_test = read("tools/test_bootstrap_evaluation_policy.cpp")
initial_preroll_policy_h = read("directmotion/InitialPrerollPolicy.h")
initial_preroll_policy_test = read("tools/test_initial_preroll_policy.cpp")
initial_segment_graph_policy_h = read("directmotion/InitialSegmentGraphPolicy.h")
initial_segment_graph_policy_test = read("tools/test_initial_segment_graph_policy.cpp")
initial_segment_staging_policy_h = read("directmotion/InitialSegmentStagingPolicy.h")
initial_segment_staging_policy_test = read("tools/test_initial_segment_staging_policy.cpp")
initial_source_rebind_policy_h = read("directmotion/InitialSourceGenerationRebindPolicy.h")
initial_source_rebind_policy_test = read("tools/test_initial_source_generation_rebind_policy.cpp")
receive_commit_policy_h = read("directmotion/ReceiveCommitPolicy.h")
receive_commit_policy_test = read("tools/test_receive_generation_policy.cpp")
source_bootstrap_queue_h = read("directmotion/SourceBootstrapQueue.h")
source_bootstrap_queue_test = read("tools/test_source_bootstrap_queue.cpp")
engine_bootstrap_commit_policy_h = read("directmotion/EngineBootstrapCommitPolicy.h")
engine_bootstrap_commit_policy_test = read("tools/test_engine_bootstrap_commit_policy.cpp")
lifecycle_transition_h = read("directmotion/LifecycleTransitionGate.h")
lifecycle_transition_test = read("tools/test_lifecycle_transition_gate.cpp")
seek_fallback_generation_h = read("directmotion/SeekFallbackGenerationPolicy.h")
seek_fallback_generation_test = read("tools/test_seek_fallback_generation_policy.cpp")
seek_reset_generation_h = read("directmotion/SeekResetGenerationPolicy.h")
seek_reset_generation_test = read("tools/test_seek_reset_generation_policy.cpp")
seek_transition_blend_h = read("directmotion/SeekTransitionBlend.h")
seek_transition_blend_test = read("tools/test_seek_transition_blend.cpp")
seek_handoff_status_h = read("directmotion/SeekHandoffStatusPolicy.h")
seek_handoff_status_test = read("tools/test_seek_handoff_status.cpp")

# Version/build identity and CI artifact wiring.
check("VERSION_MAJOR/MINOR/PATCH/BUILD identify DirectMotion 3.0.0.0", all(bool(re.search(pattern, ver_h)) for pattern in [r"#define\s+VERSION_MAJOR\s+3\b", r"#define\s+VERSION_MINOR\s+0\b", r"#define\s+VERSION_PATCH\s+0\b", r"#define\s+VERSION_BUILD\s+0\b"]))
check("VERSION_STRING matches DirectMotion 3.0.0", 'L"3.0.0"' in ver_h)
check("VERSIONINFO matches DirectMotion 3.0.0.0", '"3.0.0.0 (DirectMotion 3.0)' in ver_rc)
check("ProductVersion matches DirectMotion 3.0.0", '"ProductVersion", "3.0.0\\0"' in ver_rc)
check("BuildIdentity identifies DirectMotion 3.0.0, .28 baseline lineage and AviSynth/MVTools runtime policy", "directmotion 3.0.0 (baseline=0.2.58.28; lineage=source-generation-rebinding; parent=0.2.58-dev-initial-segment-preroll-order-independent; backend=avisynth-mvtools; runtime-dlls=module-directory-only)" in cpp)
check("DM-25.1 prepared initial-graph policy requires every lifecycle precondition", all(x in initial_segment_graph_policy_h for x in ["awaitingInitialSegment", "graphInitialized", "graphGenerationMatches", "noSourceInputAccepted", "noOutputEmitted", "seekResetIdle", "bootstrapQueueInactive", "noFlushTransition", "notStopping"]) and "return awaitingInitialSegment && graphInitialized && graphGenerationMatches" in initial_segment_graph_policy_h)
check("DM-25.1 policy regression rejects each stale/active lifecycle condition", all(x in initial_segment_graph_policy_test for x in ["fresh StartStreaming graph is adopted", "older delivery generation", "already accepted source input", "already emitted output", "active or pending seek reset", "source-bootstrap queue", "overlapping flush", "filter never adopts"]))
check("DM-25.1 prepared graph marker is atomic and remains armed while first-segment Receive samples are staged", "std::atomic<bool> m_preparedInitialGraphAwaitingSegment" in hdr and "m_preparedInitialGraphAwaitingSegment.store(true, std::memory_order_release);" in cpp and "if (!initialSegmentStaging)\n        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);" in cpp[cpp.find("HRESULT CDirectMotion::Receive"):cpp.find("HRESULT CDirectMotion::EndOfStream")])
check("DM-25.1 StartStreaming and StopStreaming clear prior segment metadata", all(x in cpp[cpp.find("HRESULT CDirectMotion::StartStreaming"):cpp.find("HRESULT CDirectMotion::Receive")] for x in ["m_currentSegmentMetadataValid = false", "m_currentSegmentStart = 0", "m_currentSegmentRate = 1.0"]) and all(x in cpp[cpp.find("HRESULT CDirectMotion::StopStreaming"):cpp.find("HRESULT CDirectMotion::Receive")] for x in ["m_currentSegmentMetadataValid = false", "m_currentSegmentStart = 0"]))
new_segment_block = cpp[cpp.find("HRESULT CDirectMotion::NewSegment"):cpp.find("void CDirectMotion::LogInfo")]
check("DM-25.1 first NewSegment adopts only a pristine current-generation graph and rebinds its owner", "CanAdoptPreparedInitialGraph(" in new_segment_block and "m_engineGeneration.load(std::memory_order_acquire) == previousDeliveryGeneration" in new_segment_block and "ResetEngineGenerationState(false, nextDeliveryGeneration);" in new_segment_block and "m_engineGeneration.store(nextDeliveryGeneration" in new_segment_block and "Adopted the pristine StartStreaming" in new_segment_block)
check("DM-25.1 ineligible first NewSegment preserves the established deferred-seek or reset paths", "BeginDeferredSeekEngineReset(nextDeliveryGeneration, false)" in new_segment_block and "ResetEngine();" in new_segment_block and new_segment_block.find("adoptPreparedInitialGraph") < new_segment_block.find("previousSegmentValid &&"))
check("DM-25.1 helper is listed in Visual Studio and its C++17 regression runs in Windows CI", "InitialSegmentGraphPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run initial-segment prepared-graph adoption policy tests", "test_initial_segment_graph_policy.cpp", "INITIAL_SEGMENT_GRAPH_POLICY_TEST_RUN_0.2.58.26.txt"]))
check("DM-26.1 initial-segment policy distinguishes dormant pre-segment input from ready or stale queue generations", all(x in initial_segment_staging_policy_h for x in ["ShouldStageBeforeFirstSegment", "awaitingInitialSegment", "queueActiveForGeneration", "workerStillGated", "graphGenerationMatches"]) and all(x in initial_segment_staging_policy_test for x in ["Receive-before-NewSegment", "inactive staging generation", "ready worker", "stale graph generation"]))
check("DM-26.1 staged graph adoption is restricted to a pristine graph, dormant FIFO and first segment", all(x in initial_segment_staging_policy_h for x in ["CanAdoptStagedInitialGraph", "stagingQueueActive", "stagingWorkerGated", "noSourceFramesPublished", "noPreviousSegment"]) and all(x in initial_segment_staging_policy_test for x in ["queued initial preroll", "empty dormant queue can still rebind", "published source frames", "active seek reset", "flush transition", "later segment"]))
check("DM-26.1 SourceBootstrapQueue can rebind a dormant generation without losing staged frame ownership", all(x in source_bootstrap_queue_h for x in ["RebindDormantGeneration", "m_generationOutstandingFrames != m_frames.size()", "frame.generation = nextGeneration", "frame.epoch = m_epoch"]) and "SetWorkerReadyForGeneration" in source_bootstrap_queue_h)
check("DM-26.1 Receive stages pre-segment samples, preserves the adoption marker and suppresses false timestamp discontinuity resets", all(x in cpp for x in ["ShouldStageBeforeFirstSegment(", "if (!initialSegmentStaging)\n        m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);", "firstSampleAfterSegment || awaitingNewSegment || initialSegmentStaging"]) and "!initialSegmentStaging && m_lastInputTimestampValid" in cpp and "!initialSegmentStaging &&\n        discontinuity" in cpp)
check("DM-26.1 in-flight initial preroll Receive may finish only into the still-dormant old-generation queue", "CanCommitInFlightInitialStage" in initial_segment_staging_policy_h and "m_segmentTransition.Active()" in cpp and "!m_flushTransition.Active()" in cpp and "!m_sourceBootstrapQueue.IsWorkerReadyForGeneration(deliveryGeneration)" in cpp and cpp.count("if (!isCurrentOrInFlightInitialStage())") >= 4)
check("DM-26.1 in-flight stage exception has portable coverage for transition, flush, Stop, failure, marker, queue and generation races", all(x in initial_segment_staging_policy_test for x in ["may finish into dormant queue after NewSegment retires generation", "requires an active segment transition", "flush transition always rejects", "Stop always rejects", "runtime failure rejects", "cleared initial marker rejects", "cancelled queue rejects", "ready worker rejects", "stale graph generation rejects"]))
check("DM-26.1 NewSegment rebinds staged generation and preserves pre-segment source indices and timeline", all(x in cpp for x in ["CanAdoptStagedInitialGraph(", "RebindDormantGeneration(previousDeliveryGeneration, nextDeliveryGeneration)", "source indices and pre-roll timeline state are preserved", "if (!reboundInitialStagedGraph)"]))
check("DM-26.1 bootstrap worker is released only after downstream accepts NewSegment", "CTransformFilter::NewSegment(start, stop, rate)" in cpp and cpp.find("segmentTransitionLease.Release();", cpp.find("CTransformFilter::NewSegment(start, stop, rate)")) < cpp.find("SetWorkerReadyForGeneration(nextDeliveryGeneration, true)") and "downstream accepted the first NewSegment" in cpp)
check("DM-26.1 real BeginFlush invalidates initial-segment staging ownership", "m_preparedInitialGraphAwaitingSegment.store(false, std::memory_order_release);" in cpp[cpp.find("HRESULT CDirectMotion::BeginFlush"):cpp.find("HRESULT CDirectMotion::EndFlush")])
check("DM-26.1 queue regression proves staged FIFO rebinding plus NewSegment-before-Receive empty-queue startup", all(x in source_bootstrap_queue_test for x in ["initial-segment staging begins active with its worker gated", "cannot publish initial preroll before NewSegment", "rebinds the complete dormant queue", "original source index", "second staged preroll frame retains FIFO order", "NewSegment-before-Receive rebinds an empty dormant initial queue", "first post-segment Receive enters the rebound initial staging generation"]))
check("DM-26.1 policy helper is listed in Visual Studio and compiled in Windows CI", "InitialSegmentStagingPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run initial-segment preroll staging and callback-order policy tests", "test_initial_segment_staging_policy.cpp", "INITIAL_SEGMENT_STAGING_POLICY_TEST_RUN_0.2.58.27.txt"]))
check("DM-27.1 policy requires a pristine graph/cache/waiter snapshot and the exact one-step NewSegment epoch", all(x in initial_source_rebind_policy_h for x in ["graphInitialized", "pipelineGenerationMatches", "noFilterInputPublished", "noFilterOutputEmitted", "noEngineInputAccepted", "noEngineOutputEmitted", "cacheEmpty", "noActiveGetFrames", "noSourceWaiters", "notEndOfStream", "notLocallyInterrupted", "newSourceInterruptEpoch == state.expectedPreviousEpoch + 1", "currentGateEpoch == state.newSourceInterruptEpoch", "newCacheGeneration != state.currentCacheGeneration"]))
check("DM-27.1 portable regression covers the exact epoch 4-to-5 reproduction and rejects competing lifecycle/state changes", all(x in initial_source_rebind_policy_test for x in ["epoch 4 to the exact NewSegment epoch 5", "intervening lifecycle cancellation", "active GetFrame calls", "registered source waiters", "nonempty source cache", "output", "epoch wraparound", "cache generation must be refreshed"]))
check("DM-27.1 InputClip rebind is exclusive against GetFrame and atomic with the shared source-wait epoch", all(x in engine for x in ["std::shared_lock<std::shared_mutex> generationLock(m_generationMutex);", "std::unique_lock<std::shared_mutex> generationLock(m_generationMutex);", "m_evaluationGate->SourceWaitMutex()", "m_sourceInterruptEpoch.store(newSourceInterruptEpoch", "m_generation.store(nextCacheGeneration", "TryRebindPristineInitialGeneration"]))
check("DM-27.1 engine-local counters are read only after the active ProcessingWorker pass exits", all(x in cpp for x in ["std::unique_lock<std::mutex> processingStateLock(m_processingWorkerMutex)", "m_processingWorkerChanged.wait(processingStateLock", "return !m_processingWorkerActive;"]))
check("DM-27.1 engine refuses to rebind after source input, output, Prefetch activation or finalization", all(x in engine for x in ["TryRebindInitialSourceGeneration", "m_inputFrameCount.load(std::memory_order_acquire) == 0", "m_expectedOutputCount.load(std::memory_order_acquire) == 0", "m_nextOutputIndex == 0", "!m_bootstrapEmitted", "!m_flowClip", "!m_flushed.load(std::memory_order_acquire)"]))
check("DM-27.1 NewSegment captures the exact epoch transition and rebinds both staging and no-staging initial graph paths before worker release", all(x in cpp for x in ["sourceInterruptEpochBeforeSegment", "sourceInterruptEpochAfterSegment", "InterruptPendingSourceRequests(\"NewSegment/seek\")", "tryRebindInitialSourceGeneration(sourceRebindError)", "m_sourceBootstrapQueue.Cancel();", "The dormant queue generation was rebound", "Adopted the pristine StartStreaming AviSynth/MVTools graph and rebound its InputClip"]))
check("DM-27.1 policy helper is listed in Visual Studio and compiled in Windows CI", "InitialSourceGenerationRebindPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run initial InputClip source-generation rebinding policy tests", "test_initial_source_generation_rebind_policy.cpp", "INITIAL_SOURCE_GENERATION_REBIND_POLICY_TEST_RUN_0.2.58.28.txt"]))
check("DM-15 Flush lifecycle state is a counted transition gate rather than a boolean", "DirectMotionLifecycle::TransitionGate m_flushTransition;" in hdr and "m_flushTransition.Acquire();" in cpp and "m_flushTransition.Release();" in cpp and "m_flushTransition.Active()" in cpp and "m_flushInProgress" not in hdr + cpp)
check("DM-15 NewSegment, runtime-toggle and local discontinuity callbacks own independent scope leases", all(x in cpp for x in ["TransitionLease segmentTransitionLease(m_segmentTransition)", "TransitionLease toggleTransitionLease(m_segmentTransition)", "TransitionLease timestampTransitionLease(m_segmentTransition)", "TransitionLease discontinuityTransitionLease(m_segmentTransition)"]) and "m_segmentTransition.store" not in cpp)
check("DM-15 transition leases release automatically on early returns and can end before delivery resumes", all(x in cpp for x in ["segmentTransitionLease.Release();", "toggleTransitionLease.Release();", "timestampTransitionLease.Release();", "discontinuityTransitionLease.Release();"]))
check("DM-15 counted transition gate prevents an older callback from clearing an overlapping newer callback", all(x in lifecycle_transition_h for x in ["class TransitionGate", "m_active.compare_exchange_weak", "bool Active() const noexcept", "void Release() noexcept"]) and all(x in lifecycle_transition_test for x in ["completion of an older callback cannot clear the newer callback", "first concurrent callback exit leaves second transition active", "duplicate release is harmless"]))
check("DM-15 transition gate test is included in Windows CI and the Visual Studio project", "LifecycleTransitionGate.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run overlapping lifecycle transition gate tests", "test_lifecycle_transition_gate.cpp", "LIFECYCLE_TRANSITION_GATE_TEST_RUN_0.2.58.15.txt"]))
check("DM-15 SourceBootstrapQueue frame completion is idempotent", "bool completed = false;" in source_bootstrap_queue_h and "if (frame.completed) return;" in source_bootstrap_queue_h and "frame.completed = true;" in source_bootstrap_queue_h)
check("DM-15 staged-frame scope guard itself performs at most one completion", "bool completed = false;" in cpp and "if (completed) return;" in cpp and "~StagedFrameLease() noexcept { Complete(); }" in cpp)
check("DM-15 queue regression proves a duplicate completion cannot drain a generation with an in-flight sibling", "repeated completion of one staged frame is idempotent" in source_bootstrap_queue_test and "duplicate completion cannot retire a generation while a sibling frame is still in flight" in source_bootstrap_queue_test)
check("DM-16 fallback state records the generation that armed it", "m_seekFallbackGeneration" in hdr and "m_seekFallbackGeneration = generation;" in cpp and "OwnsFallbackGeneration(" in seek_fallback_generation_h)
check("DM-16 interpolation handoff revalidates generation ownership under the fallback mutex", "OwnsFallbackGeneration(\n                    m_seekFallbackGeneration, generation)" in cpp and "std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);" in cpp)
fallback_payload_block = cpp[cpp.find("HRESULT CDirectMotion::QueueSeekFallbackPayloadForDelivery"):cpp.find("void CDirectMotion::ArmSeekFallback")]
check("DM-16 source fallback admission validates the owner both before payload work and immediately before enqueue", fallback_payload_block.count("OwnsFallbackGeneration(") >= 2 and "m_asyncPipeline.IsCurrentGeneration(generation)" in fallback_payload_block)
check("DM-16 arm refuses stale callback generations and rechecks after acquiring the state mutex", "CanArmFallbackGeneration(\n                generation, m_asyncPipeline.CurrentGeneration())" in cpp[cpp.find("void CDirectMotion::ArmSeekFallback"):cpp.find("void CDirectMotion::ClearSeekFallback")])
check("DM-16 stale clear/reset cannot alter a newer fallback owner", "OwnsFallbackGeneration(\n                    m_seekFallbackGeneration, expectedGeneration)" in cpp and "ResetSeekFallbackProgress(\n    AsyncPipelineState::Generation expectedGeneration)" in cpp)
check("DM-16 regression models a late old-generation handoff and clear after a replacement seek arms", all(x in seek_fallback_generation_test for x in ["late old-generation handoff cannot seal the newly armed fallback", "late old-generation clear cannot retire the replacement fallback", "fallback handoff preserves generation ownership until explicit clear"]))
check("DM-16 generation-owner policy test is included in Visual Studio and Windows CI", "SeekFallbackGenerationPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run generation-owned seek fallback tests", "test_seek_fallback_generation_policy.cpp", "SEEK_FALLBACK_GENERATION_POLICY_TEST_RUN_0.2.58.16.txt"]))
delivery_worker_block = cpp[cpp.find("void CDirectMotion::DeliveryWorkerMain()"):cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery")]
check("DM-16 delivery worker catches exceptions per iteration instead of terminating the long-lived worker", "while (!m_deliveryWorkerStop.load(std::memory_order_acquire))" in delivery_worker_block and "Recoverable delivery iteration exception" in delivery_worker_block and "Recoverable unknown delivery iteration exception" in delivery_worker_block and "Delivery worker terminated after exception" not in delivery_worker_block)
check("DM-16 delivery exceptions complete the current waiter and clear busy state without failing a replacement generation", "activeCompletion->Complete(static_cast<int32_t>(0x8000FFFFu))" in delivery_worker_block and "SetWorkerBusy(activeGeneration, AsyncWorkerKind::Delivery, false)" in delivery_worker_block and "FailGeneration(failedGeneration" not in delivery_worker_block)
check("DM-16 one-packet delivery exceptions do not fail the entire runtime", "bool deliveryException = false;" in delivery_worker_block and "deliveryException = true;" in delivery_worker_block and "if (deliveryException)" in delivery_worker_block)

check("DM-17 deferred seek reset uses explicit generation-owned phases rather than reset booleans", all(x in seek_reset_generation_h for x in ["enum class Phase", "AwaitingSegment", "AwaitingBootstrap", "struct State", "uint64_t generation", "deferredResetPending", "BeginDeferredReset", "RebindForNewSegment", "OwnsBootstrap", "Complete"]) and "DirectMotionSeekReset::State m_seekResetState;" in hdr and "m_engineResetAwaitingSegment" not in hdr + cpp and "m_deferredSeekEngineResetPending" not in hdr + cpp)
check("DM-17.2 paused-frame preservation policy requires exact deferred generation ownership and an active source queue", all(x in seek_reset_generation_h for x in ["ShouldPreserveDeferredBootstrapForPausedFrame", "sourceBootstrapQueueActive", "state.deferredResetPending", "Phase::AwaitingSegment", "Phase::AwaitingBootstrap"]))
check("DM-17.2 policy regression covers both deferred phases, stale generations, inactive queue and ordinary paused stepping", all(x in seek_reset_generation_test for x in ["paused one-shot preserves an active bootstrap", "paused one-shot in the EndFlush-to-NewSegment window", "ordinary paused frame-step without a deferred seek", "next source Receive can allocate a shared NV12 payload", "next source Receive stages into the preserved final seek generation"]))
check("DM-18.1 transition policy is generation-owned and bounded to six ordered NV12 blend steps", all(x in seek_transition_blend_h for x in ["kBlendFrameCount = 6", "struct State", "uint64_t generation", "TryAcquire", "Commit", "BlendNv12InPlace", "sourceWeight", "interpolatedWeight"]))
check("DM-18.1 transition state uses a lock-free inactive fast path outside the short handoff", "m_seekTransitionActivePublished" in hdr and "if (fallbackActivePublished || transitionActivePublished)" in cpp and "m_seekTransitionActivePublished.load(std::memory_order_acquire)" in cpp)
check("DM-18.1 retains only the newest successfully queued source image and transfers it at the existing PTS handoff", "m_seekFallbackLastSourceFrame = sharedNv12;" in cpp and "m_seekTransitionSourceFrame = m_seekFallbackLastSourceFrame;" in cpp and "InterpolationDecision::HandoffToInterpolation" in cpp)
check("DM-18.1 transition index commits only after bounded output FIFO admission", cpp.find("m_asyncPipeline.WaitEnqueueOutput(std::move(packet))") < cpp.find("DirectMotionSeekTransition::Commit(\n                m_seekTransitionState, generation, transitionBlendIndex)") and "enqueueStatus != AsyncQueueStatus::Queued" in cpp)
check("DM-18.1 blends the dequeued DeliveryWorker packet in-place and makes Pause retry idempotent", all(x in cpp for x in ["HRESULT CDirectMotion::DeliverInterpolated(AsyncOutputFrame& packet", "!packet.transitionBlendApplied", "BlendNv12InPlace(", "packet.transitionBlendApplied = true", "SeekFallback/BlendComplete"]))
check("DM-18.1 Arm, Clear and fallback-progress reset release stale transition ownership", cpp.count("m_seekTransitionActivePublished.store(false, std::memory_order_release);") >= 4 and cpp.count("DirectMotionSeekTransition::Reset(m_seekTransitionState);") >= 4)
reset_fallback_progress = cpp[cpp.find("void CDirectMotion::ResetSeekFallbackProgress("):cpp.find("HRESULT CDirectMotion::QueueDeliveryDrainBarrier(")]
check("DM-18.1 same-generation progress reset retires a completed-handoff transition before the inactive-fallback early return", reset_fallback_progress.find("DirectMotionSeekTransition::Reset(m_seekTransitionState);") >= 0 and reset_fallback_progress.find("DirectMotionSeekTransition::Reset(m_seekTransitionState);") < reset_fallback_progress.find("if (!m_seekFallbackActive) {"))
check("DM-18.1 tests verify ordered generation commits and progressive NV12 blending endpoints", all(x in seek_transition_blend_test for x in ["late completion from an old seek cannot mutate the new transition", "matching queued output commits exactly one blend slot", "first step keeps five-sixths original", "middle step advances the linear blend", "last blend step is exactly the unmodified interpolated frame", "out-of-range blend index is rejected"]))
check("DM-18.1 policy header and regression test are compiled in Visual Studio and Windows CI", "SeekTransitionBlend.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run generation-owned seek transition blend tests", "test_seek_transition_blend.cpp", "SEEK_TRANSITION_BLEND_TEST_RUN_0.2.58.19.txt"]))
check("DM-21.1 phase policy packs immediate handoff states with generation and rejects stale, reserved or invalid updates", all(x in seek_handoff_status_h for x in ["enum class State", "ReservedLegacyWarmup", "PtsWatermarkDrain", "VisualBlend", "GenerationOf", "StateOf", "generation < GenerationOf(current)", "kMaximumGeneration", "state == State::PtsWatermarkDrain"]))
check("DM-21.1 tray menu reports fallback, PTS drain and blend without redefining icon colors or exposing a retired timer phase", all(x in runtime_diag for x in ["SEEK HANDOFF: source fallback active", "PTS watermark drain", "six-frame source-to-MVTools blend", "idle / normal MVTools output"]) and "minimum timer wait" not in runtime_diag and "kTrayActivityRowCount = 7" in runtime_diag)
check("DM-21.1 phase publication follows Arm, immediate PTS seal, handoff, successful blend delivery and Clear", all(x in cpp for x in ["DirectMotionSeekHandoffStatus::State::SourceFallback", "DirectMotionSeekHandoffStatus::State::PtsWatermarkDrain", "DirectMotionSeekHandoffStatus::State::VisualBlend", "DirectMotionSeekHandoffStatus::State::Idle"]) and "MinimumWarmupWait" not in cpp and "m_asyncPipeline.IsCurrentGeneration(packet.generation)" in cpp[cpp.find("if (packet.transitionBlendFrames > 0"):cpp.find("DirectMotionTrayRecordOutput(m_outputFrameCount")])
check("DM-21.1 portable policy regression covers direct fallback-to-PTS progression, retired timer-state rejection and stale-generation immunity", all(x in seek_handoff_status_test for x in ["source fallback state is published", "first visible MVTools output advances diagnostics to PTS-watermark drain", "visual-blend phase", "late delivery completion from an older seek cannot clear a newer seek", "retired timer phase remains reserved and cannot be published", "invalid phase values are rejected"]))
check("DM-24.1 removes both timer gates and all delay-worker synchronization from the active runtime", all(x not in cpp + hdr for x in ["WaitForSeekFallbackHandoffDelay", "NotifySeekFallbackHandoffDelay", "m_seekFallbackDelayMutex", "m_seekFallbackDelayChanged", "RemainingDelayNanoseconds", "MinimumWarmupWait", "MinimumWarmupElapsed"]))
check("DM-24.1 settings expose no delay UI or persisted setting while retaining an ignored ABI slot", "seekHandoffDelayMs = 0" in hdr and "settings->seekHandoffDelayMs = 0;" in cpp and "m_settings.seekHandoffDelayMs = 0;" in cpp and "SeekHandoffDelayMs" not in cpp[cpp.find("HRESULT CDirectMotion::LoadSettings"):cpp.find("HRESULT CDirectMotion::GetSettings")] and "IDC_SEEK_HANDOFF_DELAY" not in read("directmotion/directmotionSettings.cpp") + read("directmotion/directmotion.rc") + read("directmotion/resource.h"))
check("DM-24.1 obsolete registry value is only deleted after successful save and is never read or written", "RegDeleteValueW(key, L\"SeekHandoffDelayMs\")" in cpp and "readDword(L\"SeekHandoffDelayMs\"" not in cpp and "{L\"SeekHandoffDelayMs\"" not in cpp)
check("DM-24.1 retired timer policy/test and CI compile step are removed", not (ROOT / "directmotion/SeekHandoffDelayPolicy.h").exists() and not (ROOT / "tools/test_seek_handoff_delay_policy.cpp").exists() and "Run configurable seek handoff delay policy tests" not in workflow and "SeekHandoffDelayPolicy.h" not in read("directmotion/directmotion.vcxproj"))
check("DM-21.1 policy header is listed in Visual Studio and compiled/executed by Windows CI", "SeekHandoffStatusPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run generation-owned seek handoff status policy tests", "test_seek_handoff_status.cpp", "SEEK_HANDOFF_STATUS_TEST_RUN_0.2.58.25.txt"]))
check("DM-17 BeginFlush and EndFlush hold source bootstrap until NewSegment", "BeginDeferredSeekEngineReset(nextDeliveryGeneration, true)" in cpp[cpp.find("HRESULT CDirectMotion::BeginFlush"):cpp.find("HRESULT CDirectMotion::EndFlush")] and "BeginDeferredSeekEngineReset(resetGeneration, true)" in cpp[cpp.find("HRESULT CDirectMotion::EndFlush"):cpp.find("HRESULT CDirectMotion::NewSegment")])
check("DM-17 NewSegment rebinds deferred and synchronous reset ownership to its final output generation", "m_seekResetState.deferredResetPending" in cpp[cpp.find("HRESULT CDirectMotion::NewSegment"):cpp.find("void CDirectMotion::LogInfo")] and "BeginDeferredSeekEngineReset(nextDeliveryGeneration, false)" in cpp[cpp.find("HRESULT CDirectMotion::NewSegment"):cpp.find("void CDirectMotion::LogInfo")] and "DirectMotionSeekReset::RebindForNewSegment(m_seekResetState, nextDeliveryGeneration)" in cpp and "DirectMotionSeekReset::RebindForNewSegment(m_seekResetState, expectedGeneration)" in cpp)
check("DM-17 old bootstrap completion cannot clear or finalize a different generation", "DirectMotionSeekReset::CanComplete(m_seekResetState, resetGeneration)" in cpp and "DirectMotionSeekReset::Complete(m_seekResetState, resetGeneration)" in cpp and "DirectMotionSeekReset::Reset(m_seekResetState)" in cpp)
check("DM-17 Receive treats pre-NewSegment discontinuity as expected and never builds an intermediate engine", "DirectMotionSeekReset::IsAwaitingSegment(m_seekResetState)" in cpp and "Receive/PreSegmentHold" in cpp and "const bool expectedSegmentDiscontinuity = discontinuity" in cpp)
check("DM-17 source bootstrap queue separates Receive admission from worker readiness", "bool BeginGeneration(uint64_t generation, bool workerReady = true)" not in source_bootstrap_queue_h and "m_workerReady = generation != 0 && workerReady;" in source_bootstrap_queue_h and "stopping() || (m_workerReady && !m_frames.empty())" in source_bootstrap_queue_h and "IsWorkerReadyForGeneration" in source_bootstrap_queue_h)
check("DM-17 source bootstrap readiness regression reproduces EndFlush/Receive/NewSegment ordering", all(x in seek_reset_generation_test for x in ["pre-NewSegment generation admits Receive but keeps bootstrap worker gated", "Receive can stage the intermediate pre-segment sample without blocking", "bootstrap worker cannot consume pre-segment frame before NewSegment", "NewSegment opens the worker only for the final segment generation", "worker wakes and consumes the final segment frame after NewSegment"]))
check("DM-17 generation-owned reset policy regression covers overlapping flushes and synchronous resets", all(x in seek_reset_generation_test for x in ["pre-segment generation cannot complete", "overlapping flush transfers ownership", "NewSegment after overlapping flush", "synchronous non-seek flush", "late old completion cannot clear a later reset"]))
check("DM-17 reset policy and DM-17.2 paused-seek regression are included in Visual Studio and Windows CI", "SeekResetGenerationPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run segment-owned deferred seek reset tests", "test_seek_reset_generation_policy.cpp", "SEEK_RESET_GENERATION_POLICY_TEST_RUN_0.2.58.18.txt"]))


# Diagnostics-only conversion and deferred-reset timing. These checks ensure
# the measurements remain observational and are attached to the intended stages.
check("Diagnostic timer uses steady_clock and monotonic nanosecond accounting", all(x in cpp for x in ["class ScopedDiagnosticTimer final", "std::chrono::steady_clock::now()", "duration_cast<std::chrono::nanoseconds>", "memory_order_relaxed"]))
check("Diagnostic timer does no clock read or atomic update when logging is disabled", "m_startedNs(enabled ? MonotonicTickNanoseconds() : 0)" in cpp and "if (!m_enabled) return;" in cpp)
check("Input normalization timing wraps ConvertInputToNv12", "bool CDirectMotion::ConvertInputToNv12" in cpp and "m_diagInputNormalizeSamples" in cpp)
check("Output copy timing wraps ConvertNv12ToOutput", "bool CDirectMotion::ConvertNv12ToOutput" in cpp and "m_diagOutputCopySamples" in cpp)
check("Deferred reset timing includes the complete reset operation", "CompleteDeferredSeekEngineReset()" in cpp and "m_diagDeferredResetSamples" in cpp)
check("Quiescence timing wraps SuspendProcessingWorkerForEngineReset", "workerQuiescent = SuspendProcessingWorkerForEngineReset();" in cpp and "m_diagDeferredQuiescenceSamples" in cpp)
check("Engine-destruction timing wraps retired engine Reset and release outside m_csReceive", "retiredEngine->Reset();\n                retiredEngine.reset();" in cpp and "m_diagDeferredDestroySamples" in cpp)
check("Five diagnostic timing groups are appended to existing telemetry", all(x in cpp for x in ['timingSummary("inputNormalize"', 'timingSummary("outputCopy"', 'timingSummary("deferredReset"', 'timingSummary("deferredQuiescence"', 'timingSummary("deferredEngineDestroy"']))
check("Diagnostic aggregates reset at StartStreaming and remain intact across engine-generation reset", "m_diagInputNormalizeSamples.store(0" in cpp and "m_diagDeferredDestroySamples.store(0" in cpp and cpp.find("m_diagInputNormalizeSamples.store(0") > cpp.find("HRESULT CDirectMotion::StartStreaming()") and "m_diagInputNormalizeSamples.store(0" not in cpp[cpp.find("ResetEngineGenerationState") : cpp.find("HRESULT CDirectMotion::StartStreaming()")])
check("Cold-start preroll policy admits only initial Paused samples and gives Run/strict hold precedence", all(x in initial_preroll_policy_h for x in ["initialPrerollActive && !runActive && !pauseHoldActive"]))
check("Cold-start preroll policy has portable lifecycle truth-table regression", all(x in initial_preroll_policy_test for x in ["Stopped-to-Paused cold start admits source samples", "ordinary paused state continues", "Run disables the cold-start", "strict Running-to-Paused frame hold", "initial seek remains eligible", "repeated paused NewSegment callbacks"]))
check("Initial preroll policy is part of the DirectMotion Visual Studio project", "InitialPrerollPolicy.h" in read("directmotion/directmotion.vcxproj") and '#include "InitialPrerollPolicy.h"' in hdr)
initial_wait_start = cpp.find("HRESULT CDirectMotion::WaitForReceiveAdmission(")
initial_wait_end = cpp.find("bool CDirectMotion::ShouldWaitForReceiveAdmission", initial_wait_start)
initial_wait_block = cpp[initial_wait_start:initial_wait_end] if initial_wait_start >= 0 and initial_wait_end > initial_wait_start else ""
post_lock_start = cpp.find("bool CDirectMotion::ShouldWaitForReceiveAdmission")
post_lock_end = cpp.find("STDMETHODIMP CDirectMotion::Pause()", post_lock_start)
post_lock_block = cpp[post_lock_start:post_lock_end] if post_lock_start >= 0 and post_lock_end > post_lock_start else ""
check("Initial Paused preroll bypasses the pre-lock Receive backpressure wait", "AllowColdStartPreroll" in initial_wait_block and initial_wait_block.find("AllowColdStartPreroll") < initial_wait_block.find("WaitForChange(token)"))
check("Initial Paused preroll bypasses pause-hold admission while retaining seek-staging byte backpressure", "AllowColdStartPreroll" in post_lock_block and "if (pauseBackpressure) return true;" in post_lock_block and "return m_sourceBootstrapQueue.ShouldBackpressure" in post_lock_block)
pause_start = cpp.find("STDMETHODIMP CDirectMotion::Pause()")
pause_end = cpp.find("STDMETHODIMP CDirectMotion::Stop()", pause_start)
pause_block = cpp[pause_start:pause_end] if pause_start >= 0 and pause_end > pause_start else ""
check("Stopped-to-Paused enables cold-start preroll while Running-to-Paused disables it", all(x in pause_block for x in ["if (previousState == State_Stopped)", "m_initialPrerollActive.store(true", "else if (runningToPaused)", "m_initialPrerollActive.store(false"]))
check("Failed initial Pause clears cold-start state instead of leaving stale admission enabled", "else if (previousState == State_Stopped)" in pause_block and "m_initialPrerollActive.store(false" in pause_block[pause_block.find("else if (previousState == State_Stopped)"):])
run_start = cpp.find("HRESULT CDirectMotion::Run(")
run_end = cpp.find("HRESULT CDirectMotion::BeginFlush()", run_start)
run_block = cpp[run_start:run_end] if run_start >= 0 and run_end > run_start else ""
check("Run publishes Running before disabling cold-start admission", run_block.find("m_runActive.store(true") >= 0 and run_block.find("m_runActive.store(true") < run_block.find("m_initialPrerollActive.store(false"))
failed_run_policy = run_block[run_block.find("if (FAILED(hr))"):run_block.find("} else {", run_block.find("if (FAILED(hr))"))]
check("Failed base Run restores paused admission policy before publishing runActive=false", failed_run_policy.find("m_initialPrerollActive.store(true") < failed_run_policy.find("m_runActive.store(false") and failed_run_policy.find("m_pauseHoldActive.store(true") < failed_run_policy.find("m_runActive.store(false"))
check("StartStreaming explicitly arms cold-start preroll before accepting source samples", "HRESULT CDirectMotion::StartStreaming()" in cpp and "m_initialPrerollActive.store(true" in cpp[cpp.find("HRESULT CDirectMotion::StartStreaming()"):cpp.find("HRESULT CDirectMotion::StopStreaming()")])
check("Stop and StopStreaming always clear cold-start admission", "m_initialPrerollActive.store(false" in cpp[cpp.find("STDMETHODIMP CDirectMotion::Stop()"):cpp.find("STDMETHODIMP CDirectMotion::GetClassID()")] and "m_initialPrerollActive.store(false" in cpp[cpp.find("HRESULT CDirectMotion::StopStreaming()"):cpp.find("HRESULT CDirectMotion::Receive(")])
receive_start = cpp.find("HRESULT CDirectMotion::Receive(IMediaSample* sample)")
receive_end = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(", receive_start)
receive_block = cpp[receive_start:receive_end] if receive_start >= 0 and receive_end > receive_start else ""
check("Cold-start Receive avoids the paused one-shot and continues through normal input ingest", "const bool coldStartPreroll = DirectMotionInitialPrerollPolicy::AllowColdStartPreroll" in receive_block and "if (!m_runActive.load(std::memory_order_acquire) && !coldStartPreroll)" in receive_block and "PushSourceNv12" in receive_block)
check("Cold-start source ingest is explicitly diagnosed", '"Receive/InitialPreroll"' in receive_block)
check("FIX-5.4 uses one portable 25ms threshold for output-stage slow warnings", "kSlowOutputStageWarningMs = 25.0" in delivery_latency_h and "return elapsedMs >= kSlowOutputStageWarningMs;" in delivery_latency_h)
check("Allocator slow-wait warning uses the shared latency policy", "ShouldWarnSlowOutputStage(allocatorElapsedMs)" in cpp and "allocatorElapsedMs >= 25.0" not in cpp)
slow_receive_start = cpp.find('if (SUCCEEDED(hr) && !traceThisFrame &&', cpp.find('HRESULT CDirectMotion::DeliverInterpolated('))
slow_receive_end = cpp.find('if (FAILED(hr)) {', slow_receive_start)
slow_receive_block = cpp[slow_receive_start:slow_receive_end] if slow_receive_start >= 0 and slow_receive_end > slow_receive_start else ""
check("Slow downstream Receive is warned only after successful completed calls and with logging enabled", all(x in slow_receive_block for x in ["ShouldWarnSlowOutputStage(downstreamElapsedMs)", "DirectMotionIsLoggingEnabled()", "!IsGraphTransitioning()", "!IsPauseTransitionPending()", 'DirectMotionLog("WARN", "Output/Downstream"']))
check("Slow downstream warning correlates packet, generation, PTS and allocator wait", all(x in slow_receive_block for x in ["packet.generation", "packet.sequence", "outputPts", "downstreamElapsedMs", "allocatorElapsedMs", "frameBudgetMs=", "targetFps=", "HRESULT="]))
check("Slow downstream warning captures post-call FIFO occupancy and producer backpressure metrics", all(x in slow_receive_block for x in ["m_asyncPipeline.Snapshot()", "queue.queued", "queue.capacity", "queue.highWater", "queue.producerWaits", "queue.producerWaitMaxUs", "queue.enqueued", "queue.dequeued"]))
check("Slow downstream diagnostic snapshot runs after releasing allocator sample", slow_receive_start > 0 and cpp.rfind("output->Release();", 0, slow_receive_start) > cpp.find("const double downstreamElapsedMs", cpp.find("HRESULT CDirectMotion::DeliverInterpolated(")))
check("Slow downstream diagnostic cannot replace delivery HRESULT when snapshot construction throws", "catch (...) {\n            // Diagnostics must never turn a successful DirectShow delivery into failure." in slow_receive_block)
check("Portable latency test covers below/at/above threshold and non-finite input", all(x in delivery_latency_test for x in ["just below threshold", "exact threshold", "ordinary slow call", "NaN", "positive infinity"]))
check("Delivery latency policy header is included in the Visual Studio project", "DeliveryLatencyDiagnostics.h" in read("directmotion/directmotion.vcxproj"))
check("FIX-5.4 does not alter the interpolation FIFO capacity or add allocator retry behavior", "kDefaultOutputCapacity = 16" in async_state_h and "m_impl->output.WaitPush(std::move(frame))" in async_state_cpp and "NOWAIT" not in cpp[cpp.find("HRESULT CDirectMotion::DeliverInterpolated("):cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(")])
check("Bootstrap state models FIFO acceptance rather than renderer acknowledgement", all(x in bootstrap_queue_h for x in ["class BootstrapQueueState", "bool HasBeenEnqueued()", "void MarkEnqueued()", "void Reset()", "accepted by the output FIFO", "does not claim that DeliveryWorker"]))
bootstrap_start = cpp.find("if (!m_bootstrapQueueState.HasBeenEnqueued())")
bootstrap_end = cpp.find("constexpr unsigned kMaxFramesPerPass", bootstrap_start)
bootstrap_block = cpp[bootstrap_start:bootstrap_end] if bootstrap_start >= 0 and bootstrap_end > bootstrap_start else ""
check("DM-24.1 bootstrap seals the PTS frontier without a residence wait and still primes bounded lookahead", "m_seekFallbackAcceptingSource = false;" in bootstrap_block and "PrimeSourceCacheForOutputIndex" in bootstrap_block and "CanEmitOutputIndex(bootstrapStartOutputIndex)" in bootstrap_block and "seekHandoffDelayMs" not in bootstrap_block)
check("Bootstrap state commits only after QueueInterpolatedForDelivery accepts the selected bootstrap packet", "QueueInterpolatedForDelivery(" in bootstrap_block and "bootstrapIsPreroll, generation, false" in bootstrap_block and bootstrap_block.find("if (FAILED(deliveryHr))") < bootstrap_block.find("m_bootstrapQueueState.MarkEnqueued()"))
check("Bootstrap setup handles terminal EOS with an immediate frontier seal and no artificial residence timer", "if (!m_bootstrapQueueState.HasBeenEnqueued())" in bootstrap_block and "seekHandoffDelayMs" not in bootstrap_block and "QueueInterpolatedForDelivery(" in bootstrap_block)
check("Terminal EOS skips interpolation only when sealed fallback covers the full finalized output range", "FallbackCoversFinalizedOutput" in bootstrap_block and "IsSourceFinalized()" in bootstrap_block and "ExpectedOutputCount()" in bootstrap_block and "return QueueDeliveryDrainBarrier(generation);" in bootstrap_block)
processing_fn_start = cpp.find("HRESULT CDirectMotion::ProcessAvailableFrames(")
processing_fn_end = cpp.find("void CDirectMotion::ProcessingWorkerMain()", processing_fn_start)
processing_fn = cpp[processing_fn_start:processing_fn_end] if processing_fn_start >= 0 and processing_fn_end > processing_fn_start else ""
check("Terminal EOS with zero expected output appends the FIFO drain barrier directly", "terminalDrain && m_engine->IsSourceFinalized()" in processing_fn and "m_engine->ExpectedOutputCount() == 0" in processing_fn and "return QueueDeliveryDrainBarrier(generation);" in processing_fn)
reset_state_start = cpp.find("void CDirectMotion::ResetEngineGenerationState(")
reset_state_end = cpp.find("void CDirectMotion::ResetEngine(bool prepareResumeWarmStart)", reset_state_start)
reset_state_block = cpp[reset_state_start:reset_state_end] if reset_state_start >= 0 and reset_state_end > reset_state_start else ""
check("ResetEngineGenerationState resets bootstrap state using the caller-owned fallback generation token", "AsyncPipelineState::Generation expectedFallbackGeneration" in reset_state_block and "ResetSeekFallbackProgress(expectedFallbackGeneration);" in reset_state_block and "m_asyncPipeline.CurrentGeneration()" not in reset_state_block and "m_bootstrapQueueState.Reset();" in reset_state_block)
check("All lifecycle diagnostics identify bootstrapQueued rather than bootstrapDelivered", "bootstrapQueued=" in cpp and "bootstrapDelivered=" not in cpp and "m_bootstrapDelivered" not in hdr + cpp)
check("Portable R-004 regression covers paused preroll, slow downstream delivery and FIFO order", all(x in bootstrap_queue_test for x in ["paused preroll is accepted", "delivery is stalled", "cannot trigger a duplicate bootstrap", "first FIFO item", "follows bootstrap in FIFO order", "generation reset clears"]))
check("R-004 regression drives the real bounded AsyncPipelineState output FIFO", "AsyncPipelineState pipeline(2, 2)" in bootstrap_queue_test and "WaitEnqueueOutput" in bootstrap_queue_test and "WaitDequeueOutput" in bootstrap_queue_test)
check("Visual Studio project includes the production bootstrap queue-state header", "BootstrapQueueState.h" in read("directmotion/directmotion.vcxproj"))
check("No runtime or diagnostic path retains the misleading delivered-state name", "m_bootstrapDelivered" not in hdr + cpp and "bootstrapDelivered=" not in cpp)
check("Reset-quiescence policy treats any active pass as unsafe regardless of Stop/Suspend", "if (state.activePass) return false;" in processing_quiescence_h and "return !state.workerStarted || state.stopRequested || state.suspendRequested;" in processing_quiescence_h)
check("Reset-quiescence policy permits destruction only after pass exit and a closed worker wake predicate", all(x in processing_quiescence_test for x in ["a stop request does not authorize engine reset while a pass is still active", "an idle but unsuspended worker is not quiescent", "concurrent Stop and Suspend are quiescent only after the active pass exits"]))
reset_suspend = cpp[cpp.find("bool CDirectMotion::SuspendProcessingWorkerForEngineReset() noexcept"):cpp.find("void CDirectMotion::ResumeProcessingWorkerAfterEngineReset() noexcept")]
reset_engine = cpp[cpp.find("void CDirectMotion::ResetEngine(bool prepareResumeWarmStart)"):cpp.find("HRESULT CDirectMotion::StartStreaming()")]
check("ProcessingWorker reset suspension no longer returns early merely because Stop was requested", "if (m_processingWorkerStop || !m_processingWorkerStarted" not in reset_suspend and "m_processingWorkerSuspend = true;" in reset_suspend and "wait_until(stateLock, nextReport, isQuiescent)" in reset_suspend)
check("Reset-quiescence barrier reports begin, periodic progress and successful completion", all(x in reset_suspend for x in ["WAIT_BEGIN engine-reset quiescence barrier", "WAIT_PROGRESS engine-reset quiescence still pending", "WAIT_END engine-reset quiescence acquired"]))
check("Reset progress snapshot reads tagged diagnostics outside the worker-state mutex", reset_suspend.find("stateLock.unlock();") < reset_suspend.find("DirectMotionTrayGetDiagnosticContext()") < reset_suspend.find("stateLock.lock();"))
check("ResetEngine fails closed and retains the engine when quiescence cannot be proven", "if (!SuspendProcessingWorkerForEngineReset())" in reset_engine and "the current AviSynth/MVTools engine is being retained" in reset_engine and reset_engine.find("if (!SuspendProcessingWorkerForEngineReset())") < reset_engine.find("m_engine.reset()"))
check("ResetEngine serializes worker start/join for the full quiescence and engine-destruction window", "std::unique_lock<std::mutex> processingLifecycleFence(m_processingWorkerLifecycleMutex);" in reset_engine and reset_engine.find("processingLifecycleFence") < reset_engine.find("SuspendProcessingWorkerForEngineReset()") < reset_engine.find("m_engine.reset()"))
check("Only ResetEngine and deferred-seek completion wait for worker quiescence under the lifecycle fence", cpp.count("SuspendProcessingWorkerForEngineReset()") == 3 and "Called only while m_processingWorkerLifecycleMutex is held by ResetEngine or deferred-seek completion." in cpp and "std::unique_lock<std::mutex> processingLifecycleFence(m_processingWorkerLifecycleMutex);" in cpp[cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset(") : cpp.find("HRESULT CDirectMotion::StartStreaming()")])
check("DWORD processor fallback uses a consistent std::max type", "(std::max)(static_cast<DWORD>(1), systemInfo.dwNumberOfProcessors)" in engine and "(std::max)(1u, systemInfo.dwNumberOfProcessors)" not in engine)
check("Physical-core and half-logical fallbacks use DWORD-typed bounds", all(x in engine for x in ["(std::max)(static_cast<DWORD>(1), (logicalProcessors + static_cast<DWORD>(1)) / static_cast<DWORD>(2))", "(std::min)(physicalCores, static_cast<DWORD>(32))", "(std::min)(logicalProcessors, static_cast<DWORD>(32))"]))
check("All CPU mode limits avoid DWORD/unsigned-int min/max deduction conflicts", "(std::min)(logicalProcessors, 32u)" not in engine and "(std::min)(physicalCores, 32u)" not in engine and "(std::max)(1u, (logicalProcessors + 1u) / 2u)" not in engine)

# Resolution/stride contract: visible picture, physical surface and pitch are separate.
geometry_start = cpp.find("bool GetSurfaceSize(")
geometry_end = cpp.find("int GetImageSize(", geometry_start)
geometry_helpers = cpp[geometry_start:geometry_end] if geometry_start >= 0 and geometry_end > geometry_start else ""
check("Geometry helpers distinguish physical surface from rcTarget placement", "GetSurfaceSize" in geometry_helpers and "GetVisibleRect" in geometry_helpers and "biWidth" in geometry_helpers and "rcTarget" in geometry_helpers)
visible_start = cpp.find("bool GetVisibleRect(")
visible_end = cpp.find("bool InputMediaTypeRequiresScaling(", visible_start)
visible_fn = cpp[visible_start:visible_end] if visible_start >= 0 and visible_end > visible_start else ""
check("Received input pixels are addressed by rcTarget, not upstream rcSource", all(x in visible_fn for x in ["const RECT target = vi2 ? vi2->rcTarget : vi->rcTarget", "visible = emptyTarget ? RECT{ 0, 0, surfaceWidth, surfaceHeight } : target", "visible.right <= surfaceWidth", "visible.bottom <= surfaceHeight"]) and "const RECT source" not in visible_fn)
scaling_start = cpp.find("bool InputMediaTypeRequiresScaling(")
scaling_end = cpp.find("bool GetFrameSize(", scaling_start)
scaling_fn = cpp[scaling_start:scaling_end] if scaling_start >= 0 and scaling_end > scaling_start else ""
check("Input source/target extents match when an explicit crop is present; no spatial scaling path exists", all(x in scaling_fn for x in ["const RECT source = vi2 ? vi2->rcSource : vi->rcSource", "if (emptySource)", "return false;", "const int64_t sourceWidth", "const int64_t sourceHeight", "return sourceWidth != targetWidth || sourceHeight != targetHeight;"]))
check("Empty rcTarget addresses the complete physical sample surface", "visible = emptyTarget ? RECT{ 0, 0, surfaceWidth, surfaceHeight } : target" in cpp and "RECT{ 0, 0, outSurfaceWidth, outSurfaceHeight }" in cpp)
parse_start = cpp.find("bool CDirectMotion::ParseInputMediaType(")
parse_end = cpp.find("HRESULT CDirectMotion::ApplyDynamicInputSampleType(", parse_start)
parse_fn = cpp[parse_start:parse_end] if parse_start >= 0 and parse_end > parse_start else ""
check("YUV biHeight sign is accepted without reversing top-down rows", "regardless of biHeight's sign" in parse_fn and "if (bmi.biHeight < 0) return false" not in parse_fn)
check("Input parser stores visible crop and physical surface height separately", all(x in parse_fn for x in ["m_inputSurfaceHeight = surfaceHeight", "m_inputCropLeft = visible.left", "m_inputCropTop = visible.top", "m_inputStride = inputStride"]))
check("Input parser detects interlaced two-field samples from VIDEOINFOHEADER2", all(x in parse_fn for x in ["AMINTERLACE_IsInterlaced", "AMINTERLACE_1FieldPerSample", "m_inputInterlacedTwoFieldsPerSample.store"]))
convert_start = cpp.find("bool CDirectMotion::ConvertInputToNv12(")
convert_end = cpp.find("bool CDirectMotion::ConvertNv12ToOutput(", convert_start)
convert_fn = cpp[convert_start:convert_end] if convert_start >= 0 and convert_end > convert_start else ""
check("NV12 UV-plane offset uses physical surface height", "m_inputStride) * m_inputSurfaceHeight" in convert_fn)
check("NV12 crop applies even luma/chroma origin", "m_inputCropTop / 2" in convert_fn and "m_inputCropLeft" in convert_fn)
dynamic_start = cpp.find("HRESULT CDirectMotion::ApplyDynamicInputSampleType(")
dynamic_end = cpp.find("bool CDirectMotion::ConvertInputToNv12(", dynamic_start)
dynamic_fn = cpp[dynamic_start:dynamic_end] if dynamic_start >= 0 and dynamic_end > dynamic_start else ""
check("Per-sample dynamic input validates visible size/subtype/cadence", all(x in dynamic_fn for x in ["dynamicType.subtype", "visibleWidth != m_width", "GetAvgTimePerFrame(dynamicType)"]))
check("Per-sample dynamic input applies pitch changes only after actual-length validation", dynamic_fn.find("GetActualDataLength()") >= 0 and dynamic_fn.find("m_inputStride = stride") > dynamic_fn.find("GetActualDataLength()"))
original_start = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(")
original_end = cpp.find("HRESULT CDirectMotion::", original_start + 1)
original_fn = cpp[original_start:original_end] if original_start >= 0 and original_end > original_start else ""
check("Paused/pass-through delivery normalizes source crop/stride before output", "ConvertInputToNv12(sourceBytes" in original_fn and "ConvertNv12ToOutput(normalizedFrame.data()" in original_fn)
check("Source dynamic type is not attached raw to output sample", "output->SetMediaType(&normalizedDynamicType)" not in original_fn)
check("Output media types are rebased to zero-origin visible picture", "vi2->rcSource = RECT{ 0, 0, width, height }" in cpp and "vi->rcSource = RECT{ 0, 0, width, height }" in cpp)
check("Output pitch and physical height are read independently from the media type", "GetSurfaceSize(*mediaType, outputSurfaceWidth, outputSurfaceHeight)" in cpp and "m_outputSurfaceHeight = outputSurfaceHeight" in cpp)
output_set_start = cpp.find("} else if (direction == PINDIR_OUTPUT) {")
output_set_end = cpp.find("return S_OK;", output_set_start)
output_set = cpp[output_set_start:output_set_end] if output_set_start >= 0 and output_set_end > output_set_start else ""
check("Initial output SetMediaType accepts padded physical surface with an explicit visible target", "emptyRect(rawSource) ? RECT{ 0, 0, m_width, m_height }" in output_set and "RECT{ 0, 0, outputSurfaceWidth, outputSurfaceHeight }" in output_set and "m_outputSurfaceHeight = outputSurfaceHeight" in output_set)
check("Output candidate enumeration independently aligns pitch and height", "AlignNv12Stride(width)" in cpp and "AlignNv12SurfaceHeight(height)" in cpp and "rawCandidates" in cpp and "position > 3" in cpp)
# Arithmetic smoke tests for the declared alignment rules and NV12 physical payload.
def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)
def nv12_bytes(stride: int, height: int) -> int:
    return stride * height * 3 // 2
check("720p tight NV12 surface remains 1280x720", align_up(1280, 256) == 1280 and align_up(720, 16) == 720 and nv12_bytes(1280, 720) == 1_382_400)
check("1080p padded output surface is 2048x1088 with safe UV-plane size", align_up(1920, 256) == 2048 and align_up(1080, 16) == 1088 and nv12_bytes(2048, 1088) == 3_342_336)
check("4K native output surface remains 3840x2160 within DirectShow LONG sample-size range", align_up(3840, 256) == 3840 and align_up(2160, 16) == 2160 and 0 < nv12_bytes(3840, 2160) == 12_441_600 and nv12_bytes(3840, 2160) < 2_147_483_647)
check("Output UV-plane base uses physical output surface height", "m_outputStride) * m_outputSurfaceHeight" in cpp)
check("Output Y and UV copy honor destination target origin", "m_outputTargetTop + y" in cpp and "m_outputTargetTop / 2 + y" in cpp and "m_outputTargetLeft" in cpp)
check("Renderer dynamic output validates and preserves padded surface height/target rectangle", "outputSurfaceHeight < targetRect.bottom" in cpp and "vi2->rcTarget = targetRect" in cpp and "vi->rcTarget = targetRect" in cpp)
check("Output allocator sizing uses physical surface payload", "Nv12BytesForStride(outputSurfaceWidth, outputSurfaceHeight)" in cpp and "m_outputBufferBytes = chosenBytes" in output_set)
check("Tray runtime info shows visible geometry and physical input/output surfaces", "Picture %dx%d | Input pitch %d / height %d | Output surface %dx%d" in cpp)
runtime_diag = read("directmotion/RuntimeDiagnostics.cpp")
runtime_hdr = read("directmotion/RuntimeDiagnostics.h")
check("Tray work phases distinguish backend, allocator, renderer, input copy, source ingest and Receive lock", all(x in runtime_hdr for x in ["Backend = 1", "Allocator = 2", "Downstream = 3", "InputCopy = 4", "SourceIngest = 5", "ReceiveLock = 6"]))
check("Receive-lock wait is tagged before acquiring m_csReceive", cpp.find('"Receive / waiting to acquire DirectShow streaming lock"') < cpp.find("CAutoLock receiveLock(&m_csReceive);", cpp.find("HRESULT CDirectMotion::Receive(IMediaSample* sample)")))
check("AviSynth Prefetch bootstrap frame evaluation is visible to processing-worker telemetry", '"ProcessingWorker / raw MBlockFps direct bootstrap GetFrame"' in cpp)
check("Steady-state EmitNextAvailable / Prefetch / MVTools GetFrame is visible to processing-worker telemetry", '"ProcessingWorker / AviSynth Prefetch MBlockFps GetFrame"' in cpp and cpp.find('"ProcessingWorker / AviSynth Prefetch MBlockFps GetFrame"') < cpp.find("m_engine->EmitNextAvailable(frame, available, finished, error)"))
check("Engine initialization and input normalization expose separate tray stages", '"EnsureEngine / initialize AviSynth+ and MVTools2 graph after early seek-fallback admission"' in cpp and '"ConvertInputToNv12 / crop and NV12-NV21 normalize"' in cpp)
check("Allocator wait names the exact GetDeliveryBuffer operation and frame context", '"GetDeliveryBuffer / wait for free renderer sample"' in cpp and "DirectMotionWorkPhase::Allocator" in cpp)
check("Renderer Receive has a separate visible phase from allocator acquisition", '"Deliver / downstream renderer Receive"' in cpp and '"Deliver / downstream renderer Receive (paused-frame copy)"' in cpp)
check("Tray menu exposes active operation duration, frame indices and thread ID", all(x in runtime_diag for x in ["CURRENT:", "elapsedMs", "input #%lld", "output #%lld", "thread %lu"]))
check("Tray menu reports last operation and input/output recency when idle", all(x in runtime_diag for x in ["LAST:", "PROGRESS:", "input #", "output #", "s ago"]))
check("Tray tooltip distinguishes backend processing from allocator/downstream waits", all(x in runtime_diag for x in ["PROCESSING", "WAITING", "GetDeliveryBuffer", "NO UPSTREAM Receive", "BUFFERING"]))
check("Tray status icon refreshes periodically while a call is blocked", "kTrayStatusTimerId" in runtime_diag and "kTrayStatusIntervalMs" in runtime_diag and "ReportLongRunningWorkOnce();" in runtime_diag)
check("Tray status timer startup failure is logged instead of silently freezing the icon", 'Could not schedule phase/status refresh timer; tray details may not refresh' in runtime_diag)
check("Watchdog reports each long-running per-thread operation once and includes its sequence", all(x in runtime_diag for x in ["g_activeWorkByThread", "record.watchdogReported", "record.sequence", "Tray/Watchdog", "not proof of a deadlock"]))
check("Green active tooltip includes live input/output progress rather than stale generic ACTIVE", "ACTIVE | input %lld / output %lld" in runtime_diag and "g_lastOutputTick" in runtime_diag)
check("Successful input and output activity timestamps are recorded", "DirectMotionTrayRecordInput(" in cpp and "DirectMotionTrayRecordOutput(" in cpp)
check("Source IMediaSample2 flags are captured before output generation", "CaptureInterpolatedSampleFlags(sample);" in cpp and "properties.dwTypeSpecificFlags" in cpp and "AM_VIDEO_FLAG_WEAVE" in cpp)
delivery_start = cpp.find("HRESULT CDirectMotion::DeliverInterpolated(")
delivery_end = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(", delivery_start)
delivery_flags_fn = cpp[delivery_start:delivery_end] if delivery_start >= 0 and delivery_end > delivery_start else ""
check("Every interpolated allocator sample explicitly receives current type-specific flags", all(x in delivery_flags_fn for x in ["outputProps.dwTypeSpecificFlags = appliedTypeSpecificFlags", "outputSample2->SetProperties", "sample2OutputFlagsSet"]))
check("Interlaced WEAVE is propagated only for two-field source samples that request it", "m_inputInterlacedTwoFieldsPerSample.load" in cpp and "haveSample2Flags && (sourceFlags & AM_VIDEO_FLAG_WEAVE)" in cpp)
check("Tray knows whether DirectShow graph is Running versus Paused/Stopped", "DirectMotionTraySetGraphRunning(true)" in cpp and "DirectMotionTraySetGraphRunning(false)" in cpp and "GRAPH: Running" in runtime_diag and "GRAPH: Paused / stopped" in runtime_diag)
check("Active blocking work remains visible even when interpolation is toggled off", runtime_diag.find("else if (phase != DirectMotionWorkPhase::Idle)") < runtime_diag.find("else if (!interpolation)"))
check("No-input and no-output warnings appear after one second, not a stale multi-second green window", "kNoInputThresholdMs = 1000" in runtime_diag and "kNoOutputThresholdMs = 1000" in runtime_diag)
check("Startup distinguishes awaiting first output from stalled output after progress", "WARMING UP / awaiting first output" in runtime_diag and "BUFFERING / no output" in runtime_diag)
check("Tray computes delivered FPS against negotiated target over one-second windows", all(x in runtime_diag for x in ["kThroughputWindowMs = 1000", "g_targetOutputFps", "g_measuredOutputFps", "DirectMotionTraySetTargetFps"]))
check("Throughput measurement resets when a new seek/flush engine generation resets output numbering", "DirectMotionTrayResetThroughput" in runtime_diag and "DirectMotionTrayResetThroughput();" in cpp)
check("Sustained under-target throughput is orange and reports measured/target FPS", "kThroughputWarningRatio = 0.85" in runtime_diag and "LOW THROUGHPUT %.1f/%.1f FPS" in runtime_diag)
check("Throughput is logged once per measurement window", "Tray/Throughput" in runtime_diag and "windowMs=" in runtime_diag)
check("Renderer IQualProp probe runs sparsely without verbose tracing", "if (!DirectShowTraceEnabled() && !DirectShowFullTraceEnabled()) return;" not in cpp[cpp.find("void CDirectMotion::AuditDownstreamRenderer("):cpp.find("void CDirectMotion::CaptureInterpolatedSampleFlags(")] and "deliveredFrames % 120" in cpp)
check("EndOfStream records a Receive-lock wait before acquiring m_csReceive", '"EndOfStream / waiting to acquire DirectShow streaming lock"' in cpp and cpp.find('"EndOfStream / waiting to acquire DirectShow streaming lock"') < cpp.find("CAutoLock receiveLock(&m_csReceive);", cpp.find("HRESULT CDirectMotion::EndOfStream()")))
opaque_start = cpp.find('LogInfo(site, "Explicit interpolation-off: source sample is not CPU-readable; using direct delivery.')
opaque_end = cpp.find("return S_OK;", opaque_start)
opaque_delivery = cpp[opaque_start:opaque_end] if opaque_start >= 0 and opaque_end > opaque_start else ""
check("Opaque pass-through success updates tray output progress", "DirectMotionTrayRecordOutput(" in opaque_delivery)
check("Tray diagnostics leave allocator buffer count unchanged", "properties->cBuffers = std::max<LONG>(properties->cBuffers, 4);" in cpp)
check("Source cache budget scales with frame pixel count", "kReferenceFramePixels" in engine and "std::sqrt(" in engine and "resolutionScaledCacheBytes" in engine)
check("Physical RAM bounds a separate total working-set envelope", "GetPhysicallyInstalledSystemMemory" in engine and "totalWorkingSetBudgetBytes" in engine and "installedMemoryBytes / 3u" in engine)
check("Source ring has independent 384 MiB–2 GiB budget and 128-frame nominal hard cap", "kMinSourceCacheBudgetBytes = 384u * 1024u * 1024u" in engine and "kMaxSourceCacheBudgetBytes = 2u * 1024u * 1024u * 1024u" in engine and "kSourceCacheHardCapFrames = 128" in engine)
check("Prefetch cost scales continuously and reaches eight raw-frame equivalents at 4K", "3.0L + std::ceil(frameScale) + 2.0L" in engine and "prefetchFrameCostEquivalents" in engine)
check("3840x1600 scale is classified continuously without a UHD threshold branch", "3840ull * 2160ull" not in engine and "boundedScale" in engine)
check("Scheduler reduces Prefetch frame count against byte-budgeted retention and total working-set ceilings", "estimatedWorkingSetFrames(prefetchFrames) > totalWorkingMemoryFrames" in engine and "plannedCacheFrames(prefetchFrames) > sourceCacheRetentionFrames" in engine and "--prefetchFrames;" in engine)
check("Source retention ceiling is the lesser of byte-budgeted frames and the 128-frame hard cap", "MaxRetainedFrames(" in engine and "sourceCacheMemoryFrames, kSourceCacheHardCapFrames" in engine and "sourceCacheRetentionLimitFrames=" in engine and "retentionLimitFrames=" in engine)
check("Working-set estimate includes the complete possible source-ring retention window", "return sourceCacheRetentionFrames + prefetchCost" in engine and "sourceCacheRetentionFrames == 0" in engine)
check("AviSynth shared frame-pool ceiling is dynamically derived and capped at 4.5 GiB", "requestedAviSynthMemoryMaxMiB" in engine and "kAviSynthMemoryMaxMiB = 4608" in engine and "totalWorkingSetBudgetBytes / (1024u * 1024u)" in engine)
check("Scheduler log reports pixel scale, frame bytes, installed RAM and active budgets", all(x in engine for x in ["frameScale=", "frameBytesMiB=", "installedMemoryMiB=", "totalWorkingSetBudgetMiB=", "sourceCacheBudgetMiB=", "estimatedWorkingSetMiB="]))

# Keep the proven 0.2.38 dynamic media-type cadence correction.
allocator_start = cpp.find("HRESULT CDirectMotion::ApplyAllocatorMediaType(")
allocator_end = cpp.find("HRESULT CDirectMotion::DecideBufferSize(", allocator_start)
allocator = cpp[allocator_start:allocator_end] if allocator_start >= 0 and allocator_end > allocator_start else ""
check("Allocator dynamic media type normalized to TargetFrameDuration", "const REFERENCE_TIME negotiatedFrameDuration = TargetFrameDuration();" in allocator and "SetAvgTimePerFrame(dynamicType, negotiatedFrameDuration);" in allocator)
set_cadence = allocator.find("SetAvgTimePerFrame(dynamicType, negotiatedFrameDuration);")
set_type = allocator.find("output->SetMediaType(&dynamicType)")
check("Cadence normalization precedes applying media type", set_cadence >= 0 and set_type > set_cadence)
check("Allocator trace records original and corrected cadence", all(x in allocator for x in ["rendererAvgTimePerFrame=", "negotiatedAvgTimePerFrame=", "correctedCadence=", "outputFPS="]))

# Verify the actual AviSynth+ scheduler from the earlier working design is restored.
build_start = engine.find("bool AviSynthMvToolsEngine::BuildGraph(")
build_end = engine.find("bool AviSynthMvToolsEngine::Initialize(", build_start)
graph_build = engine[build_start:build_end] if build_start >= 0 and build_end > build_start else ""
set_mode_pos = graph_build.find('Invoke("SetFilterMTMode", AVSValue(mtModeArgs, 2))')
source_create_pos = graph_build.find("source clip creation")
prefetch_pos = graph_build.find('Invoke("Prefetch", AVSValue(prefetchArgs, 3))')
prefetch_builder_start = engine.find("bool AviSynthMvToolsEngine::BuildPrefetchFromOutputStart(")
prefetch_builder_end = engine.find("bool AviSynthMvToolsEngine::CanEmitOutputIndex(", prefetch_builder_start)
prefetch_builder = engine[prefetch_builder_start:prefetch_builder_end] if prefetch_builder_start >= 0 and prefetch_builder_end > prefetch_builder_start else ""
check("Graph configures AviSynth+ SetFilterMTMode DEFAULT_MT_MODE=2", 'AVSValue("DEFAULT_MT_MODE"), AVSValue(2)' in graph_build and set_mode_pos >= 0)
check("SetFilterMTMode runs before source/filter graph construction", set_mode_pos >= 0 and source_create_pos > set_mode_pos)
check("Prefetch is created lazily over Trim(raw MBlockFps, frontier successor, end)", 'Invoke("Trim", AVSValue(trimArgs, 3))' in prefetch_builder and 'AVSValue(static_cast<int>(startOutputIndex + 1))' in prefetch_builder and 'Invoke("Prefetch", AVSValue(prefetchArgs, 3))' in prefetch_builder)
check("Prefetch wrapper becomes the steady-state output clip", "m_flowClip = std::make_unique<PClip>(prefetchResult.AsClip());" in prefetch_builder)
check("Raw MBlockFps is retained separately from the frontier-relative Prefetch wrapper", "m_rawFlowClip = std::make_unique<PClip>(fpsResult.AsClip());" in graph_build and "std::unique_ptr<PClip> m_rawFlowClip" in engine_h and "m_flowClip = std::make_unique<PClip>(prefetchResult.AsClip());" in prefetch_builder)
check("First uncovered output uses raw MBlockFps and its successor starts at Prefetch-relative frame zero", "UsesPrefetchForOutput(outputIndex, m_firstPrefetchOutputIndex)" in engine and "PrefetchRelativeIndex(outputIndex, m_firstPrefetchOutputIndex)" in engine and "return outputIndex >= firstPrefetchOutputIndex;" in bootstrap_eval_h)
check("Cold and resumed bootstrap gates require only MVTools temporal lookahead", "RequiredSourceFramesForFirstOutput(kMotionLookaheadFrames)" in graph_build and "bootstrapInputFrames=" in engine and "bootstrapPolicy=raw-direct-first-uncovered-output" in engine and "ColdBootstrapInputFramesRequired" not in graph_build)
check("Prefetch scheduler creation is deferred until after fallback watermark sealing", "prefetchActivation=lazy-after-fallback-watermark-seal-frame-zero-is-first-successor" in engine and "BuildPrefetchFromOutputStart" in engine and "firstPrefetchOutputIndex" in engine_h and "m_firstPrefetchOutputIndex = startOutputIndex + 1;" in engine)
check("Graph diagnostics remain safe before the lazy Prefetch wrapper exists", 'AVSValue(*m_rawFlowClip), AVSValue(graphPathUtf8.c_str()), AVSValue(2)' in engine and 'AVSValue(*m_flowClip), AVSValue(graphPathUtf8.c_str()), AVSValue(2)' not in engine)
check("Bootstrap evaluation policy is shared by runtime and portable tests", "BootstrapEvaluationPolicy.h" in engine and "RequiredSourceFramesForFirstOutput" in bootstrap_eval_test and "UsesPrefetchForOutput" in bootstrap_eval_test)
check("Bootstrap evaluation policy is included in the Visual Studio project", "BootstrapEvaluationPolicy.h" in read("directmotion/directmotion.vcxproj"))
check("Legacy cold/resume Prefetch lead math is no longer the production startup gate", "ColdBootstrapInputFramesRequired" not in engine and "ResumeBootstrapInputFramesRequired" not in engine and "BootstrapInputFramesRequired()" in engine_h)
check("Only strict Running-to-Paused Run reset enables resume warm-start", "ResetEngine(resumedAfterStrictPauseHold)" in cpp and "m_resumeWarmStartPending.store(prepareResumeWarmStart" in cpp and "const bool resumeWarmStart = m_resumeWarmStartPending.load" in cpp and "m_settings.mvtools, resumeWarmStart,\n                              m_avisynthRuntimeSession, m_sourceEvaluationGate" in cpp)
check("Resume warm-start regression covers cold-start preservation, rational ratios and integer overflow", all(x in resume_warm_start_test for x in ["cold-start", "2.5x", "4x", "64-output Prefetch", "invalid output ratio", "extreme cold-start", "extreme MVTools lookahead", "extreme motion lookahead"]))
check("Windows CI compiles and retains the resume warm-start policy regression suite", all(x in workflow for x in ["Run portable resume warm-start policy tests", "test_resume_warm_start.cpp", "RESUME_WARM_START_TEST_RUN_0.2.57_FIX3_6.txt"]))
check("Prefetch frame window is independent and capped at 64 output frames", "automaticPrefetchFrames = (std::min)(64, schedulerThreadBudget * 2)" in graph_build and "prefetchFrames = std::clamp(requestedPrefetchFrames, 1, 64)" in graph_build)
check("Selected CPU profile caps total Prefetch threads independently from frames", "schedulerThreadBudget = (std::min)(cpuWorkerBudget, 32)" in graph_build and "prefetchThreads = schedulerThreadBudget;" in graph_build)
check("Profile-specific read-ahead reduction has been removed", "usesDenseQuickRefinement" not in graph_build and "dense-half-workers" not in graph_build and "prefetchWindowPolicy=profile-independent-memory-bounded" in graph_build)
check("Prefetch requested/effective frame count and selected mode are logged", all(x in engine for x in ["framesRequested=", "framesEffective=", "framesMode=", "profile-independent-memory-bounded"]))
check("Scheduler retains the 32-worker cap and independent 64-frame prefetch cap", "schedulerThreadBudget = (std::min)(cpuWorkerBudget, 32)" in graph_build and "std::clamp(requestedPrefetchFrames, 1, 64)" in graph_build)
check("Scheduler cache uses 128-frame nominal hard cap and adaptive source budget", "kSourceCacheHardCapFrames = 128" in engine and "kBaseSourceCacheBudgetBytes" in engine and "kMaxSourceCacheBudgetBytes" in engine)
check("Scheduler reduces only the frame window to fit cache and memory", "while (prefetchFrames > 1" in graph_build and "--prefetchFrames;" in graph_build and "Not enough memory/cache budget" in graph_build)
check("Scheduler logs chosen worker/frame/cache/memory plan", all(x in engine for x in ["AviSynth/Scheduler", "Prefetch threads=", "framesRequested=", "framesEffective=", "sourceCacheFrames=", "logicalProcessors=", "physicalCores=", "cpuProfile=", "estimatedWorkingSetMiB="]))
check("CPU profile discovers active logical processors across groups", "GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)" in engine)
check("Physical-core profile uses Windows processor-core topology", "GetLogicalProcessorInformationEx(RelationProcessorCore" in engine and "RelationProcessorCore" in engine)
check("CPU modes include Auto, physical, half-logical and custom", all(x in engine_h for x in ["kCpuProfileAuto", "kCpuProfilePhysical", "kCpuProfileHalfLogical", "kCpuProfileCustom"]))
check("Physical-core topology fallback is explicitly logged", "physicalTopologyAvailable" in graph_build and "physicalTopologyDetected=" in engine)
check("CPU profiles cap total scheduler threads and do not force process affinity", "caps total scheduler threads and does not pin threads to specific logical processors" in engine)
check("Receive lead offers only 1, 2 or 4 source frames", "receiveBatchFrames != 1" in cpp and "receiveBatchFrames != 2" in cpp and "receiveBatchFrames != 4" in cpp)
check("Receive batch setting remains a compatibility preference without Receive-side graph pulls", all(x in cpp for x in ["ReceiveBatchFrames", "receiveBatchFrames != 1", "receiveBatchFrames != 2", "receiveBatchFrames != 4"]) and "EmitSynchronousAvailableFrames" not in cpp)
check("Receive publishes into the shared InputClip while an independent processing worker owns GetFrame", "Receive publishes normalized source surfaces into the shared InputClip cache" in engine and "ProcessingWorker owns MBlockFps GetFrame requests" in engine)
check("EOS finalization wakes Prefetch and terminal drain is performed by the processing worker", "m_engine->FinalizeSource(error)" in cpp and "ProcessAvailableFrames(generation, terminalDrain, continuationRequired)" in cpp and "terminal MBlockFps drain" in cpp)
check("CPU and Receive-batch settings persist in the registry", all(x in cpp for x in ["CpuProfile", "CustomCpuWorkers", "ReceiveBatchFrames"]))
check("Property page exposes CPU profile, custom cap, Receive batch and independent Prefetch frames", all(x in settings_cpp for x in ["IDC_CPU_PROFILE", "IDC_CPU_WORKERS", "IDC_RECEIVE_BATCH", "IDC_PREFETCH_FRAMES", "Auto (2x workers)"]))
check("Prefetch frame setting persists and triggers graph rebuild independently of motion profile", all(x in cpp for x in ["PrefetchFrames", "requested.prefetchFrames != m_settings.prefetchFrames"]) and "candidate.prefetchFrames = prefetchValues[prefetchIndex]" in settings_cpp)
check("AviSynth SetMemoryMax uses shared frame-pool budget with 4.5-GiB ceiling", "requestedAviSynthMemoryMaxMiB" in graph_build and "totalWorkingSetBudgetBytes" in graph_build and "kAviSynthMemoryMaxMiB = 4608" in engine)

# Thread safety and wait topology of the live push-driven source.
check("InputClip explicitly advertises re-entrant MT_NICE_FILTER to AviSynth+", "cachehints == CACHE_GET_MTMODE" in engine and "return MT_NICE_FILTER" in engine)
check("InputClip recognizes AviSynth+ V12 CACHE_INFORM_NUM_THREADS (514) from Prefetch", "kAviSynthCacheInformNumThreadsHintV12 = 514" in engine and "cachehints == kAviSynthCacheInformNumThreadsHintV12" in engine and "m_prefetchThreadsInformed.store" in engine)
get_frame_start = engine.find("PVideoFrame __stdcall GetFrame(int n")
get_frame_end = engine.find("const VideoInfo& __stdcall GetVideoInfo", get_frame_start)
get_frame = engine[get_frame_start:get_frame_end] if get_frame_start >= 0 and get_frame_end > get_frame_start else ""
check("InputClip permits concurrent Prefetch source requests", "std::shared_lock<std::shared_mutex> generationLock(m_generationMutex)" in get_frame and "m_activeGetFrames.fetch_add" in get_frame and "SetActiveSourceRequest(n, true)" in get_frame)
check("Future input waits use condition_variable with unique_lock", "std::unique_lock<std::mutex> lock(m_mutex);" in get_frame and "m_framesChanged.wait(lock" in get_frame)
check("Shared source-wait channel sleeps without retaining the cache mutex", "std::unique_lock<std::mutex> sourceWaitLock(m_evaluationGate->SourceWaitMutex())" in get_frame and "m_evaluationGate->SourceChangedCondition().wait(sourceWaitLock, sourceReadyOrInterrupted)" in get_frame and "std::lock_guard<std::mutex> cacheLock(m_mutex)" in get_frame)
check("Source cache source sample copy is still bounded", "m_cacheCapacity" in get_frame and "m_framesChanged.notify_all()" in engine)
# 0.2.51 shared source surfaces and bounded worker pinning.
push_start = engine.find("DirectMotionSourceCache::PushResult PushNv12(")
push_end = engine.find("bool CompleteEndOfStream(", push_start)
push_fn = engine[push_start:push_end] if push_start >= 0 and push_end > push_start else ""
check("Input ingest materializes one planar surface and publishes its shared frame handle", "env->NewVideoFrame(m_vi)" in push_fn and "CopyPlane(planar->GetWritePtr(PLANAR_Y)" in push_fn and "DeinterleaveNv12ChromaRow" in push_fn and "m_frames.emplace_back(planar)" in push_fn)
check("Concurrent InputClip GetFrame returns a shared PVideoFrame without per-worker allocation/copy", "PVideoFrame result = source" in get_frame and "source = m_frames[static_cast<size_t>(static_cast<int64_t>(n) - base)]" in get_frame and "NewVideoFrame" not in get_frame and "CopyPlane" not in get_frame)
check("Source ring retains AviSynth frame handles rather than raw planar byte vectors", "std::deque<PVideoFrame> m_frames" in engine and "std::deque<PlanarFrame>" not in engine)
check("Every active InputClip GetFrame request is leased before checking source readiness", get_frame.find("SetActiveSourceRequest(n, true)") >= 0 and get_frame.find("SetActiveSourceRequest(n, true)") < get_frame.find("sourceReadyOrInterrupted()"))
check("Active GetFrame leases survive until an immutable PVideoFrame reference is acquired", "struct ActiveSourceRequestGuard" in get_frame and get_frame.find("PVideoFrame result = source") < get_frame.find("SetActiveSourceRequest(n, false)"))
check("Historical slack is computed independently from the memory hard cap", all(x in retention_h for x in ["HistorySlackFrames", "minimumHistoryMarginFrames", "prefetchSourceLookaheadFrames", "motionLookaheadFrames", "safetyFrames"]))
check("Source-cache eviction uses a monotonic retention anchor and bounded history slack", "sourceRetentionAnchor" in retention_h and "safeAnchor = (std::min)(safeAnchor, oldestActiveRequest)" in retention_h and "historySlackFrames" in retention_h and "if (sourceRetentionAnchor < 0" in retention_h)
check("Far-future Prefetch requests cannot advance eviction beyond the established retention anchor", "safeAnchor = sourceRetentionAnchor" in retention_h and "safeAnchor = (std::min)(safeAnchor, oldestActiveRequest)" in retention_h and "RetentionFloor(0, 1000, 48, history32) == 24" in retention_test)
check("The 128-frame ceiling is not passed as the history-retention window", "m_historySlackFrames);" in engine and "m_sourceRetentionAnchor, m_maxRetainedFrames" not in engine)
check("A growing watermark releases slots before the hard cap blocks Receive", all(x in retention_test for x in ["cache.size() == 128", "base == 24", "CanAppend(cache.size(), max720p)", "cache.push_back(128)"]))
check("Prefetch and MVTools lookahead scale history slack beyond the 24-frame default when needed", "HistorySlackFrames(26, 2, 4, historyMargin)" in retention_test and "history64 == 32" in retention_test)
check("Receive applies bounded interruptible source-cache backpressure and re-notifies processing", all(x in push_fn for x in ["CanAppend(m_frames.size(), m_maxRetainedFrames)", "m_cacheSpaceChanged.wait_for", "pressureCallback(pressureContext)", "WAIT_BEGIN producer backpressure"]))
check("Lifecycle interruption wakes Receive blocked on source-cache capacity", "m_cacheSpaceChanged.notify_all()" in engine[engine.find("void InterruptSourceWaits()") : engine.find("void AdvanceConsumerSourceRetentionAnchor")])
emit_frame_start = engine.find("bool AviSynthMvToolsEngine::EmitFrame(")
emit_frame_end = engine.find("bool AviSynthMvToolsEngine::EmitNextAvailable(", emit_frame_start)
emit_frame_fn = engine[emit_frame_start:emit_frame_end] if emit_frame_start >= 0 and emit_frame_end > emit_frame_start else ""
check("Completed output advances the source-retention anchor only after frame conversion", "output.emplace_back(std::move(out))" in emit_frame_fn and emit_frame_fn.find("output.emplace_back(std::move(out))") < emit_frame_fn.find("AdvanceConsumerSourceRetentionAnchor"))
prime_retention = engine[engine.find("bool AviSynthMvToolsEngine::PrimeSourceCacheForOutputIndex") : engine.find("bool AviSynthMvToolsEngine::EmitFrame", engine.find("bool AviSynthMvToolsEngine::PrimeSourceCacheForOutputIndex"))]
check("Seek-frontier source-cache priming uses the rational output-to-source coordinate and preserves history slack", all(x in prime_retention for x in ["TrySourceFrameAnchorForOutputIndex", "AdvanceConsumerSourceRetentionAnchor", "m_sourceRetentionPrimedForOutputIndex"]))
check("Seek-frontier cache priming occurs after fallback sealing and before the lookahead wait", "PrimeSourceCacheForOutputIndex(" in bootstrap_block and bootstrap_block.find("m_seekFallbackAcceptingSource = false;") < bootstrap_block.find("PrimeSourceCacheForOutputIndex(") < bootstrap_block.find("CanEmitOutputIndex(bootstrapStartOutputIndex)"))
check("EOS finalization marks the live source complete without blocking on cache room", "bool CompleteEndOfStream(" in engine and "m_eos = true" in engine[engine.find("bool CompleteEndOfStream(") : engine.find("void InterruptSourceWaits()")])
check("EOS lookahead uses the logical terminal duplicate path", "else if (m_eos && !m_frames.empty())" in get_frame and "source = m_frames.back();" in get_frame and "AppendDuplicateLast" not in engine)
check("Source-cache expiry and producer backpressure are surfaced to the independent diagnostics worker", all(x in runtime_diag for x in ["DirectMotionTraySourceFrameExpired()", "DirectMotionTraySourceCacheProducerWaiting()"]) and all(x in cpp for x in ["observation.sourceFrameExpired =", "observation.sourceCacheProducerWaiting ="]))
check("Stall classifier distinguishes terminal source expiry and bounded cache backpressure", all(x in stall_diag_h for x in ["SourceFrameExpired", "SourceCacheBackpressure", "SOURCE_FRAME_EXPIRED", "SOURCE_CACHE_BACKPRESSURE"]))
check("Stall monitor stays active long enough to publish terminal source-frame expiry", "(runActive || sourceFrameExpired)" in cpp and "(!runtimeFailed || sourceFrameExpired)" in cpp and "const bool sourceFrameExpired = DirectMotionTraySourceFrameExpired();" in cpp)
check("Portable source-cache regression tests cover watermark advance and a delayed frame-23 lease", all(x in retention_test for x in ["late Prefetch GetFrame(23)", "watermark advance", "after the immutable frame reference is acquired", "Receive can publish the frame"]))
check("PushSourceNv12 distinguishes Published, Interrupted and Failed outcomes", all(x in engine_h for x in ["PushResult PushSourceNv12", "SourceCacheRetention.h"]) and all(x in engine for x in ["PushResult::Published", "PushResult::Interrupted", "PushResult::Failed"]))
check("Expected Pause/Stop/Flush/seek cancellation is benign and does not call FailRuntime", "IsExpectedLifecycleInterruption(" in cpp and "in-flight sample is discarded without marking the runtime failed" in cpp)
check("Unexpected source cancellation in the active generation still fails explicitly", "unexpected interruption" in retention_test and "pushFailure = pushResult" in cpp)
check("Stall classifier prioritizes explicit source-cache backpressure over normalization gap", stall_diag_h.find("state.sourceCacheProducerWaiting") < stall_diag_h.find("state.counters.received > state.counters.normalized") and "explicit source-cache producer wait outranks" in stall_diag_test)
check("Source-cache retention helper and test are part of the Visual Studio/CI source package", "SourceCacheRetention.h" in read("directmotion/directmotion.vcxproj") and "test_source_cache_retention.cpp" in workflow)
check("Windows CI executes and retains the portable source-cache retention test", all(x in workflow for x in ["Run portable source-cache retention tests", "SOURCE_CACHE_RETENTION_TEST_RUN_0.2.58.25.txt", "test_source_cache_retention.cpp"]))

# Pause must abort downstream allocator waits before it waits for the delivery mutex.
pause_start = cpp.find("STDMETHODIMP CDirectMotion::Pause()")
pause_end = cpp.find("STDMETHODIMP CDirectMotion::Stop()", pause_start)
pause_impl = cpp[pause_start:pause_end] if pause_start >= 0 and pause_end > pause_start else ""
pause_cancel = pause_impl.find('InterruptPendingSourceRequests("Pause/RunToPaused")')
pause_begin_flush = pause_impl.find("m_pOutput->DeliverBeginFlush()")
pause_base_call = pause_impl.find("const HRESULT hr = CTransformFilter::Pause();")
pause_fence = pause_impl.find("std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex)")
pause_end_flush = pause_impl.find("m_pOutput->DeliverEndFlush()", pause_fence)
check("Pause completes downstream BeginFlush before the base Pause and delivery fence without interrupting source history", pause_begin_flush >= 0 and pause_base_call > pause_begin_flush and pause_fence > pause_base_call and pause_cancel < 0)
check("Strict Pause keeps AviSynth source requests and graph history live while holding Receive upstream", "Do not interrupt AviSynth source requests on a strict Running -> Paused" in pause_impl and "releases the exact pending Receive" in pause_impl and pause_cancel < 0)
check("Pause balances its local downstream flush only after acquiring the delivery fence", pause_end_flush > pause_fence and "localPauseFlushStarted" in pause_impl)
check("Pause retains the current FIFO generation for both normal presentation hold and terminal EOS", "terminalEosActive = m_processingEosRequested && !m_processingEosCompleted" in pause_impl and "if (!terminalEosActive)" in pause_impl and "newGeneration = oldGeneration" in pause_impl and "m_asyncPipeline.BeginGeneration()" not in pause_impl)
check("Pause checks EOS before the base transition and preserves active terminal source requests", pause_impl.find("terminalEosActiveAtPauseEntry = m_processingEosRequested") >= 0 and pause_impl.find("terminalEosActiveAtPauseEntry = m_processingEosRequested") < pause_base_call and "preserving its source requests" in pause_impl)
check("Pause exposes separate base-Pause, downstream-flush and delivery-fence timing markers", all(x in pause_impl for x in ["Pause/Base", "Pause/LocalFlush", "Pause/DeliveryFence", "WAIT_BEGIN", "WAIT_END", "baseElapsedMs="]))
check("Pause BeginFlush failure rolls back to Running before calling the DirectShow base Pause", pause_impl.find("if (FAILED(beginFlushHr))") < pause_impl.find("CTransformFilter::Pause()") and "Pause was not committed" in pause_impl)
check("Portable Pause/delivery-fence test covers blocked allocator and Deliver, EOS preservation, external flush, failure rollback and idempotent Pause", all(x in pause_fence_test for x in ["allocator wait", "downstream Deliver", "terminal EOS", "source requests are not interrupted", "external flush", "BeginFlush fails", "idempotent Pause"]))
check("Windows CI compiles and executes the Pause/delivery-fence regression suite", all(x in workflow for x in ["Run portable Pause/delivery-fence lifecycle tests", "test_pause_delivery_fence.cpp", "PAUSE_DELIVERY_FENCE_TEST_RUN_0.2.57.txt"]))
check("Full trace records current/peak concurrent GetFrame calls and shared frame handoff time", all(x in get_frame for x in ["activeGetFrameCalls=", "peakConcurrentGetFrames=", "sharedFrameReturnMs="]))
check("Full trace correlates actual source concurrency with Prefetch-informed thread budget", "prefetchThreadsInformed=" in get_frame)
check("NV12/YV12 chroma conversions use SSE2 16-sample blocks with scalar tails", all(x in engine for x in ["InterleaveChromaRow", "DeinterleaveNv12ChromaRow", "_mm_unpacklo_epi8", "_mm_packus_epi16"]))
check("Full trace marks the start of a live-source worker wait", '"WAIT_BEGIN requested="' in get_frame and '"AviSynth/SourceWait"' in get_frame)
check("Full trace marks wait completion with synchronized interruption state", '"WAIT_END requested="' in get_frame and "sourceWaitInterrupted" in get_frame)
check("Source-wait diagnostics include frame, generation and thread identity", all(x in get_frame for x in ["requested=", "generation=", "GetCurrentThreadId()", "cacheRange="]))
check("Live source waiter set records concurrent requested-frame range", "std::multiset<int64_t> m_waitingSourceFrames" in engine and all(x in get_frame for x in ["SetSourceWaitRegistration(n, true)", "SetSourceWaitRegistration(n, false)"]))
check("Source wait/cache telemetry is published without changing wait predicates", all(x in engine for x in ["PublishSourceWaitInfoLocked", "PublishCacheRangeLocked", "DirectMotionTraySetSourceWaitInfo"]))


# Seek/flush cancellation must not depend on acquiring m_csReceive or dereferencing m_engine.
gate_has_epoch = "std::atomic<uint64_t> m_sourceInterruptEpoch{0}" in engine_h and "SourceInterruptEpoch() const noexcept" in engine_h
check("Shared source cancellation epoch exists", gate_has_epoch)
check("Shared source wait is synchronized against lost wakeups", "SourceWaitMutex()" in engine_h and "SourceChangedCondition()" in engine_h and "lock_guard<std::mutex> lock(m_sourceWaitMutex)" in engine_h)
check("Lifecycle cancellation advances epoch and opens evaluation gate", "m_sourceInterruptEpoch.fetch_add(1, std::memory_order_acq_rel)" in engine_h and "(void)SetEnabled(true)" in engine_h)
check("InputClip snapshots its graph cancellation epoch atomically for the one-shot initial rebind", "m_sourceInterruptEpoch(m_evaluationGate ? m_evaluationGate->SourceInterruptEpoch() : 0)" in engine and "std::atomic<uint64_t> m_sourceInterruptEpoch" in engine and "m_sourceInterruptEpoch.store(newSourceInterruptEpoch" in engine)
check("InputClip source wait exits for a stale graph generation", "SourceInterruptEpoch() !=\n                m_sourceInterruptEpoch.load(std::memory_order_acquire)" in get_frame and "stale graph generation" in get_frame)
check("Source frame appends notify shared Prefetch waiters", "m_evaluationGate->NotifySourceChanged()" in engine)
helper_start = cpp.find("uint64_t CDirectMotion::InterruptPendingSourceRequests(")
helper_end = cpp.find("STDMETHODIMP CDirectMotion::Pause()", helper_start)
helper = cpp[helper_start:helper_end] if helper_start >= 0 and helper_end > helper_start else ""
check("Lifecycle interrupt helper uses only shared gate, not engine pointer", "m_sourceEvaluationGate->InterruptSourceRequests()" in helper and "m_engine" not in helper)
stop_start = cpp.find("STDMETHODIMP CDirectMotion::Stop()")
stop_end = cpp.find("HRESULT CDirectMotion::Run(", stop_start)
stop_cb = cpp[stop_start:stop_end] if stop_start >= 0 and stop_end > stop_start else ""
check("Stop publishes cancellation before downstream/base stop", stop_cb.find('InterruptPendingSourceRequests("Stop")') >= 0 and stop_cb.find('InterruptPendingSourceRequests("Stop")') < stop_cb.find("CTransformFilter::Stop()"))
begin_start = cpp.find("HRESULT CDirectMotion::BeginFlush()")
begin_end = cpp.find("HRESULT CDirectMotion::EndFlush()", begin_start)
begin_cb = cpp[begin_start:begin_end] if begin_start >= 0 and begin_end > begin_start else ""
check("BeginFlush publishes cancellation before base flush and receive lock", begin_cb.find('InterruptPendingSourceRequests("BeginFlush/seek")') >= 0 and begin_cb.find('InterruptPendingSourceRequests("BeginFlush/seek")') < begin_cb.find("CTransformFilter::BeginFlush()") < begin_cb.find("CAutoLock receiveLock(&m_csReceive)"))
end_start = cpp.find("HRESULT CDirectMotion::EndFlush()")
end_end = cpp.find("HRESULT CDirectMotion::NewSegment(", end_start)
end_cb = cpp[end_start:end_end] if end_start >= 0 and end_end > end_start else ""
check("EndFlush publishes cancellation before receive lock", end_cb.find('InterruptPendingSourceRequests("EndFlush/seek")') >= 0 and end_cb.find('InterruptPendingSourceRequests("EndFlush/seek")') < end_cb.find("CAutoLock receiveLock(&m_csReceive)"))
segment_start = cpp.find("HRESULT CDirectMotion::NewSegment(")
segment_end = cpp.find("HRESULT CDirectMotion::", segment_start + 1)
segment_cb = cpp[segment_start:segment_end] if segment_start >= 0 and segment_end > segment_start else ""
check("NewSegment acquires its own transition lease before cancellation and the receive lock", segment_cb.find("TransitionLease segmentTransitionLease(m_segmentTransition)") >= 0 and segment_cb.find('InterruptPendingSourceRequests("NewSegment/seek")') > segment_cb.find("TransitionLease segmentTransitionLease(m_segmentTransition)") and segment_cb.find('InterruptPendingSourceRequests("NewSegment/seek")') < segment_cb.find("CAutoLock receiveLock(&m_csReceive)"))
fail_start = cpp.find("void CDirectMotion::FailRuntime(")
fail_end = cpp.find("HRESULT CDirectMotion::", fail_start + 1)
fail_cb = cpp[fail_start:fail_end] if fail_start >= 0 and fail_end > fail_start else ""
check("FailRuntime avoids unsynchronized engine-pointer interrupt", 'InterruptPendingSourceRequests(where ? where : "FailRuntime")' in fail_cb and "m_engine" not in fail_cb)

# AviSynth+ Prefetch is the only frame-level scheduler in the DirectMotion graph.
check("MSuper standard and advanced graphs explicitly disable MVTools internal threading", engine.count('AVSValue(false)') >= 2 and engine.count('"mt"') >= 2)
check("Both standard and advanced MAnalyse paths use shared mt=false and temporal=false policy values", graph_build.count("kMAnalyseInternalThreadingEnabled") == 2 and graph_build.count("kMAnalyseTemporalPredictorsEnabled") == 2 and "inline constexpr bool kMAnalyseInternalThreadingEnabled = false" in manalyse_policy_h and "inline constexpr bool kMAnalyseTemporalPredictorsEnabled = false" in manalyse_policy_h)
check("MAnalyse never retries a signature that omits mt", "retrying its legacy signature" not in graph_build and "GetMAnalyseInvocationContract(false)" in graph_build and "GetMAnalyseInvocationContract(true)" in graph_build)
check("MAnalyse NotFound handling fails graph construction with explicit compatibility error", graph_build.count("catch (const IScriptEnvironment::NotFound&)") >= 2 and graph_build.count("MVTools2 rejected the required") == 2 and graph_build.count("return AVSValue();") >= 2)
check("The shared MAnalyse policy exposes only standard 11-arg and advanced 13-arg mt=false/temporal=false contracts", all(x in manalyse_policy_h for x in [
    "kMAnalyseStandardMtFalseContract", "kMAnalyseAdvancedMtFalseContract", "GetMAnalyseInvocationContract"
]) and "Legacy" not in manalyse_policy_h and "kMAnalyseInternalThreadingEnabled = false" in manalyse_policy_h)
check("Portable MAnalyse test locks the production-shared false constant and both named-argument layouts", all(x in manalyse_policy_test for x in [
    "MVTools internal threading is disabled by the shared production policy",
    "linear temporal predictors are disabled by the shared seek-frontier policy",
    "standard MAnalyse requires eleven arguments including mt=false and temporal=false",
    "advanced MAnalyse requires thirteen arguments including mt=false and temporal=false",
    "standard profile's mt argument is explicitly false",
    "advanced profile's mt argument is explicitly false",
    "both MAnalyse paths expose an explicit temporal predictor argument"
]))
check("No mandatory AVSTP load/pinning or module-directory AVSTP requirement remains", "LoadLibraryExW(avstp" not in engine and "g_avstpRuntimeModule" not in engine and "AviSynth/AVSTP" not in engine)
check("MAnalyse invocation policy header is part of the Visual Studio project", "MAnalyseInvocationPolicy.h" in read("directmotion/directmotion.vcxproj"))

# Step 2: generated output is delivered by exactly one dedicated worker.
recv_start = cpp.find("HRESULT CDirectMotion::Receive(IMediaSample* sample)")
recv_end = cpp.find("HRESULT CDirectMotion::DeliverInterpolated(AsyncOutputFrame& packet, int64_t outputPts)", recv_start)
recv = cpp[recv_start:recv_end] if recv_start >= 0 and recv_end > recv_start else ""
check("Receive publishes decoded source into the shared AviSynth InputClip", "PushSourceNv12" in recv)
check("Receive signals processing and returns without AviSynth graph evaluation", "NotifyProcessingWorker();" in recv and "m_engine->EmitBootstrapFrame" not in recv and "m_engine->EmitNextAvailable" not in recv and "EmitSynchronousAvailableFrames" not in recv)
check("Receive does not wait for generated-output acknowledgement", "completion->Wait()" not in recv and "QueueInterpolatedForDelivery" not in recv)
check("Receive captures one active output generation under the receive lock", "m_asyncPipeline.CurrentGeneration()" in recv and "deliveryGeneration == 0" in recv)
check("DM-04 Receive generation policy rejects either a retired FIFO token or an announced graph transition", "generationIsCurrent && !graphTransitioning" in receive_commit_policy_h and "IsReceiveCommitCurrent" in hdr and "transitioningBefore || transitioningAfter" in cpp)
check("DM-04 Receive keeps a mutable generation token and adopts its own timestamp/discontinuity generation", "AsyncPipelineState::Generation deliveryGeneration = m_asyncPipeline.CurrentGeneration();" in recv and recv.count("deliveryGeneration = newGeneration;") == 4)
check("DM-04 Receive checks generation after normalization and before deferred reset/EnsureEngine", recv.find("return rejectRetiredReceive(\"seek fallback admission\")") > recv.find("ConvertInputToNv12(input") and recv.find("return rejectRetiredReceive(\"deferred engine reset\")") < recv.find("CompleteDeferredSeekEngineReset()") and recv.find("return rejectRetiredReceive(\"EnsureEngine\")") < recv.find("EnsureEngine(error, deliveryGeneration)"))
check("DM-04 EnsureEngine binds graph identity to the sample generation and discards an uncommitted stale graph", "expectedReceiveGeneration" in cpp[cpp.find("bool CDirectMotion::EnsureEngine("):cpp.find("HRESULT CDirectMotion::DeliverDownstreamTracked")] and "m_engineGeneration.store(engineGeneration" in cpp and "Discarded a newly initialized but uncommitted" in cpp)
check("DM-04 Receive revalidates after engine initialization and immediately before source-cache publication", recv.find("EnsureEngine(error, deliveryGeneration)") < recv.find("return rejectRetiredReceive(\"PushSourceNv12\")") < recv.find("m_engine->PushSourceNv12(") and recv.find("return rejectRetiredReceive(\"source-cache publication\")") < recv.find("m_engine->PushSourceNv12("))
check("DM-04 stale samples are retired with S_OK without marking the runtime failed", "return S_OK;" in recv[recv.find("const auto rejectRetiredReceive"):recv.find("const uint64_t receivedInputFrame")] and "The sample was already in Receive" in recv)
check("DM-04 portable policy tests cover stale-before-engine, transition-during-init and post-transition commit", all(x in receive_commit_policy_test for x in ["retired pipeline generation", "second seek between fallback admission and EnsureEngine", "generation retired during EnsureEngine", "lifecycle announcement rejects a sample"]))
step_start = recv.find("// Frame stepping is a presentation request")
step_end = recv.find("const int64_t inputFrameIndex", step_start)
paused_step = recv[step_start:step_end] if step_start >= 0 and step_end > step_start else ""
check("Strict Running-to-Paused hold suppresses a new visible source frame while preserving initial/seek frame stepping", "m_pauseHoldActive.load(std::memory_order_acquire)" in paused_step and "return S_FALSE;" in paused_step and 'DeliverOriginalSample(sample, "Receive/PausedFrameStep", pauseEpoch)' in paused_step and "m_pauseFrameGate.TryClaim(pauseEpoch)" in paused_step)
check("A racing sample after an already-closed paused-frame epoch is retired without runtime failure", "m_pauseFrameGate.WasPresented(pauseEpoch)" in paused_step and "retiring that sample without changing the renderer image" in paused_step and "return S_FALSE;" in paused_step and "return VFW_E_WRONG_STATE;" not in paused_step)
preserve_branch_start = paused_step.find("if (preserveDeferredSeekBootstrap)")
preserve_branch_else = paused_step.find("} else {", preserve_branch_start)
preserve_branch = paused_step[preserve_branch_start:preserve_branch_else] if preserve_branch_start >= 0 and preserve_branch_else > preserve_branch_start else ""
ordinary_step_branch = paused_step[preserve_branch_else:] if preserve_branch_else >= 0 else ""
check("DM-17.2 paused one-shot preserves an exact active deferred-seek bootstrap instead of cancelling its queue", all(x in paused_step for x in ["ShouldPreserveDeferredBootstrapForPausedFrame(", "m_seekResetState", "m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration)", "if (preserveDeferredSeekBootstrap)"]) and "ResetEngine();" not in preserve_branch and "m_sourceBootstrapQueue.Cancel()" not in preserve_branch and "without cancelling its deferred bootstrap" in preserve_branch.lower())
check("DM-17.2 ordinary paused frame-step still uses synchronous reset when no deferred seek owns the active queue", "Ordinary paused frame-step (no deferred seek owner)" in ordinary_step_branch and "ResetEngine();" in ordinary_step_branch)
check("DM-17.2 paused one-shot re-anchors only stream-time origin after preserving or resetting the engine", "m_streamTimeOrigin = 0;" in paused_step and "m_streamTimeOriginSet = false;" in paused_step and paused_step.find("m_streamTimeOriginSet = false;") < paused_step.find("m_pauseFrameGate.MarkPresented(pauseEpoch)") and "seek-owned deferred reset is preserved" in paused_step)

# FIX-3.4: hold the next upstream Receive outside m_csReceive until Run or cancellation.
base_receive_start = base_transfrm_cpp.find("CTransformInputPin::Receive(IMediaSample *pSample)")
base_receive_end = base_transfrm_cpp.find("// override to pass downstream", base_receive_start)
base_receive_fn = base_transfrm_cpp[base_receive_start:base_receive_end] if base_receive_start >= 0 and base_receive_end > base_receive_start else ""
check("Input pin runs the first admission wait before taking m_csReceive", base_receive_fn.find("WaitForReceiveAdmission(pSample)") >= 0 and base_receive_fn.find("WaitForReceiveAdmission(pSample)") < base_receive_fn.find("CAutoLock receiveLock"))
check("Input pin refreshes sample properties under lock, rechecks admission, unlocks and retries", base_receive_fn.find("CBaseInputPin::Receive(pSample)") < base_receive_fn.find("ShouldWaitForReceiveAdmission(pSample)") < base_receive_fn.find("receiveLock.Unlock()") < base_receive_fn.find("continue;"))
check("CAutoLock supports temporary Unlock while preserving destructor cleanup", "void Unlock()" in wxutil_h and "void Lock()" in wxutil_h and "if (m_bLocked) m_pLock->Unlock();" in wxutil_h)
check("Other CTransformFilter users retain no-op default receive-admission hooks", all(x in base_transfrm_h for x in ["virtual HRESULT WaitForReceiveAdmission", "return S_OK;", "virtual bool ShouldWaitForReceiveAdmission", "return false;"]))
check("PauseReceiveBackpressure uses monotonic Run/cancel epochs and a condition-variable predicate", all(x in pause_receive_h for x in ["m_runEpoch.fetch_add", "m_cancelEpoch.fetch_add", "m_changed.wait(lock", "runEpoch", "cancelEpoch", "WaitResult::Cancelled"]))
wait_admission_start = cpp.find("HRESULT CDirectMotion::WaitForReceiveAdmission(")
wait_admission_end = cpp.find("bool CDirectMotion::ShouldWaitForReceiveAdmission(", wait_admission_start)
wait_admission_fn = cpp[wait_admission_start:wait_admission_end] if wait_admission_start >= 0 and wait_admission_end > wait_admission_start else ""
should_wait_start = wait_admission_end
should_wait_end = cpp.find("STDMETHODIMP CDirectMotion::Pause()", should_wait_start)
should_wait_fn = cpp[should_wait_start:should_wait_end] if should_wait_start >= 0 and should_wait_end > should_wait_start else ""
check("Receive admission holds media samples immediately on Running-to-Paused and after a legacy paused frame-step", "dwStreamId != AM_STREAM_MEDIA" in wait_admission_fn and "m_pauseHoldActive.load" in wait_admission_fn and "m_pauseFrameGate.WasPresented(pauseEpoch)" in wait_admission_fn and "runningToPausedHold" in wait_admission_fn)
check("Pause backpressure waits without acquiring m_csReceive or the delivery fence", "WaitForChange(token)" in wait_admission_fn and "m_csReceive.Lock()" not in wait_admission_fn and "CAutoLock" not in wait_admission_fn and "m_deliveryMutex" not in wait_admission_fn)
check("Post-lock admission check catches strict pause hold and legacy one-shot presentation", "m_pInput->SampleProps()" in should_wait_fn and "m_pauseHoldActive.load" in should_wait_fn and "m_pauseFrameGate.WasPresented(m_pauseFrameGate.CurrentEpoch())" in should_wait_fn and "m_runActive.load" in should_wait_fn)
run_start = cpp.find("HRESULT CDirectMotion::Run(")
run_end = cpp.find("bool CDirectMotion::ParseInputMediaType", run_start)
run_fn = cpp[run_start:run_end] if run_start >= 0 and run_end > run_start else ""
check("Run publishes Running, clears strict hold, and then signals blocked source Receive before base Run", run_fn.find("m_runActive.store(true") >= 0 and run_fn.find("m_pauseHoldActive.store(false") > run_fn.find("m_runActive.store(true") and run_fn.find("m_pauseReceiveBackpressure.NotifyRun()") > run_fn.find("m_pauseHoldActive.store(false") and run_fn.find("m_pauseReceiveBackpressure.NotifyRun()") < run_fn.find("CTransformFilter::Run(start)"))
check("Stop, BeginFlush, NewSegment and FailRuntime clear strict hold and cancel blocked source Receive", all("m_pauseHoldActive.store(false" in fn and "m_pauseReceiveBackpressure.Cancel();" in fn for fn in [stop_cb, begin_cb, segment_cb, fail_cb]))
check("Portable source-backpressure tests cover exact resume, lock release, race and lifecycle cancellation", all(x in pause_receive_test for x in ["TestRunToPauseKeepsLastImageAndHoldsExactNextSample", "TestReceiveLockIsNotHeldDuringStrictPauseWait", "TestConcurrentReceiveRechecksAfterLock", "TestInitialPausedStartupStillAllowsFirstFrame", "TestLifecycleCancellationReleasesWaiter", "TestImmediatePauseAfterRunReholdsThePendingSample", "TestCancellationIsNotLostAcrossRestartStateReset"]))
check("PauseReceiveBackpressure header is included in the Visual Studio project", "PauseReceiveBackpressure.h" in read("directmotion/directmotion.vcxproj"))
check("Windows CI compiles and uploads the portable Pause/Run source-backpressure test", all(x in workflow for x in ["Run portable Pause/Run source-backpressure tests", "test_pause_receive_backpressure.cpp", "PAUSE_RECEIVE_BACKPRESSURE_TEST_RUN_0.2.57_FIX3_5.txt"]))
pause_start = cpp.find("STDMETHODIMP CDirectMotion::Pause()")
pause_end = cpp.find("STDMETHODIMP CDirectMotion::Stop()", pause_start)
pause_impl = cpp[pause_start:pause_end] if pause_start >= 0 and pause_end > pause_start else ""
check("Pause commits strict frame hold before invalidating epoch and before downstream local flush", pause_impl.find("m_pauseHoldActive.store(true") >= 0 and pause_impl.find("m_pauseHoldActive.store(true") < pause_impl.find("m_pauseFrameGate.Invalidate()") < pause_impl.find("DeliverBeginFlush"))
check("Pause failure paths clear strict hold, restore Running, and wake blocked Receive", pause_impl.count("m_pauseHoldActive.store(false") >= 2 and pause_impl.count("m_pauseReceiveBackpressure.NotifyRun()") >= 2)
check("Receive rejects only the in-flight sample that crossed the hold boundary and uses S_FALSE", "m_pauseHoldActive.load(std::memory_order_acquire)" in recv and "return S_FALSE;" in recv and "Receive/PausedHold" in recv)
check("Pause frame delivery validation cancels a request once strict hold starts", "!m_pauseHoldActive.load(std::memory_order_acquire)" in cpp[cpp.find("bool CDirectMotion::IsPauseFrameRequestCurrent"):cpp.find("HRESULT CDirectMotion::DeliverOriginalSample")])
check("PauseFrameGate provides monotonic lifecycle epochs and an atomic one-shot claim", all(x in pause_frame_gate_h for x in ["CurrentEpoch()", "Invalidate()", "TryClaim(Epoch epoch)", "compare_exchange_weak", "WasPresented(Epoch epoch)"]))
check("Run identifies an outstanding claimed paused-frame allocator request", "AdvanceForRun(&pendingPausedFrameRequest)" in cpp and "claimed != 0 && !framePresented" in pause_frame_gate_h)
check("Run flushes an outstanding paused-frame allocator/Deliver call before acquiring its delivery fence", "Run/PausedFrameFlush" in cpp and cpp.find("m_pOutput->DeliverBeginFlush()", cpp.find("HRESULT CDirectMotion::Run(")) < cpp.find("std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex)", cpp.find("HRESULT CDirectMotion::Run(")))
check("Run fails the runtime rather than resume if it cannot pair its Run-owned EndFlush", 'FailRuntime("Run/PausedFrameFlush"' in cpp and "Downstream EndFlush failed after cancelling an obsolete paused-frame allocator request" in cpp)
check("A paused downstream Deliver completion invalidated during Run is discarded before updating paused state", "Paused-frame downstream call completed after Run/Stop/Flush/seek" in cpp and "return S_FALSE;" in cpp[cpp.find("HRESULT CDirectMotion::DeliverOriginalSample"):cpp.find("void CDirectMotion::FailRuntime")])
check("Run consumes the prior paused-frame epoch and detects whether it was shown", "m_pauseFrameGate.AdvanceForRun(&pendingPausedFrameRequest)" in cpp and '"Run/PausedFrameStep"' in cpp and "next timed source sample" in cpp)
check("Stop, BeginFlush, NewSegment and runtime failure invalidate delayed paused-frame requests", all(x in cpp for x in ["m_pauseFrameGate.Invalidate();", "HRESULT CDirectMotion::BeginFlush()", "HRESULT CDirectMotion::NewSegment(", "void CDirectMotion::FailRuntime("]))
check("DeliverOriginalSample validates the pause epoch before allocator wait, after allocator wait and before downstream Deliver", all(x in cpp for x in ["expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch)", "Discarded paused-frame allocator sample after Run/Stop/Flush/seek", "Discarded the copied paused-frame sample before downstream Deliver"]))
pause_delivery_start = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(")
pause_delivery_end = cpp.find("void CDirectMotion::FailRuntime(", pause_delivery_start)
pause_delivery_fn = cpp[pause_delivery_start:pause_delivery_end] if pause_delivery_start >= 0 and pause_delivery_end > pause_delivery_start else ""
interpolated_delivery_start = cpp.find("HRESULT CDirectMotion::DeliverInterpolated(")
interpolated_delivery_end = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(", interpolated_delivery_start)
interpolated_delivery_fn = cpp[interpolated_delivery_start:interpolated_delivery_end] if interpolated_delivery_start >= 0 and interpolated_delivery_end > interpolated_delivery_start else ""
post_deliver_epoch_guard = "if (expectedPauseEpoch != 0 && !IsPauseFrameRequestCurrent(expectedPauseEpoch))"
post_deliver_guard_pos = pause_delivery_fn.rfind(post_deliver_epoch_guard)
post_deliver_call_pos = pause_delivery_fn.find("hr = DeliverDownstreamTracked(output)")
post_deliver_failure_pos = pause_delivery_fn.find("if (FAILED(hr))", post_deliver_guard_pos)
check("Post-Deliver paused-frame epoch guard is inside DeliverOriginalSample and after downstream Deliver, never in DeliverInterpolated", post_deliver_guard_pos >= 0 and post_deliver_call_pos >= 0 and post_deliver_call_pos < post_deliver_guard_pos < post_deliver_failure_pos and post_deliver_epoch_guard not in interpolated_delivery_fn and "const char* site = reason ? reason : \"DeliverOriginalSample\";" in pause_delivery_fn)
check("A superseded or decommitted paused-frame allocator request is discarded without raising a runtime error", "return S_FALSE;" in cpp[cpp.find("HRESULT CDirectMotion::DeliverOriginalSample"):cpp.find("void CDirectMotion::FailRuntime")] and "releasing this one-shot claim" in cpp)
check("ResetEngine clears only the presented marker while retaining the current epoch claim", "m_pauseFrameGate.ClearPresented();" in cpp and "deliberately keeps the claim" in pause_frame_gate_h)
check("Paused-frame gate tests cover lifecycle invalidation, allocator race, retry and concurrent claims", all(x in pause_frame_gate_test for x in ["RunInvalidatesBlockedAllocatorRequest", "TestRunCanRetryFailedLocalFlush", "TestOtherLifecycleInvalidations", "TestConcurrentClaimsAreOneShot", "TestAllocatorFailureReleasesClaim", "TestRunConsumesPresentedMarker"]))
check("Windows CI compiles and retains the paused-frame one-shot/epoch regression suite", all(x in workflow for x in ["Run portable paused-frame one-shot and stale-epoch tests", "test_pause_frame_gate.cpp", "PAUSE_FRAME_GATE_TEST_RUN_0.2.57.txt"]))
check("First non-preroll source sample establishes one authoritative visibility boundary", "!inputPreroll && !m_prerollBoundaryInitialized" in recv and "m_prerollBoundaryInitialized = true;" in recv and "if (hasInputTime)" in recv and "m_prerollBoundaryTime = inputStart;" in recv and "TryComputeSourceFrameOutputPts(" in recv)
check("Engine reset clears timestamp and output-frame preroll boundaries", "m_prerollBoundaryInitialized = false;" in cpp and "m_prerollBoundaryTime = 0;" in cpp and "m_prerollBoundaryTimeValid = false;" in cpp and "m_prerollBoundaryOutputPts = 0;" in cpp and "m_prerollBoundaryOutputPtsValid = false;" in cpp)
process_start = cpp.find("HRESULT CDirectMotion::ProcessAvailableFrames(")
process_end = cpp.find("void CDirectMotion::ProcessingWorkerMain() noexcept", process_start)
processing = cpp[process_start:process_end] if process_start >= 0 and process_end > process_start else ""
check("Processing worker is the only owner of bootstrap and steady-state MBlockFps requests", "m_engine->EmitBootstrapFrame(" in processing and "bootstrapStartOutputIndex, bootstrap, error" in processing and "m_engine->EmitNextAvailable(frame, available, finished, error)" in processing and "EmitSynchronousAvailableFrames" not in cpp)
check("Processing worker evaluates selected bootstrap after source-cache and frontier lookahead gates", "BootstrapInputFramesRequired()" in processing and "m_engine->InputFrameCount() < m_engine->BootstrapInputFramesRequired()" in processing and "EmitBootstrapFrame(" in processing and "bootstrapStartOutputIndex, bootstrap, error" in processing and "CanEmitOutputIndex(bootstrapStartOutputIndex)" in processing)
check("Normal processing queues generated frames without per-frame delivery waits", "QueueInterpolatedForDelivery(" in processing and "bootstrapIsPreroll, generation, false" in processing and "QueueInterpolatedForDelivery(frame, false, generation, terminalDrain)" in processing and "if (!awaitDelivery) return S_OK;" in cpp)
check("Terminal EOS frames alone request acknowledgements and the final FIFO barrier is awaited", "packet.terminalDrain = awaitDelivery;" in cpp and "QueueDeliveryDrainBarrier(generation)" in processing and "barrier.drainBarrier = true;" in cpp and "completion->Wait()" in cpp)
check("Delivery barrier is consumed in FIFO order without issuing a phantom downstream sample", "if (packet.drainBarrier)" in cpp and "AsyncDelivery/DrainBarrier" in cpp and "SUCCEEDED(deliveryHr) && !packet.drainBarrier" in cpp)
check("Processing worker owns its dedicated thread and starts with streaming", "std::thread m_processingWorkerThread" in hdr and "StartProcessingWorker(processingWorkerError)" in cpp and "std::thread(&CDirectMotion::ProcessingWorkerMain, this)" in cpp)
check("Receive remains the producer that can satisfy Prefetch future-source waits", "PushSourceNv12" in recv and "NotifyProcessingWorker" in recv and "WaitDequeueInput" not in recv)
check("Generic AsyncPipelineState input FIFO is not placed between Receive and the AviSynth cache", "WaitDequeueInput" not in cpp and "TryEnqueueInput" not in cpp)
check("Output packets carry immutable timeline/cadence/preroll snapshots", all(x in async_state_h for x in ["streamTimeOrigin100ns", "targetFrameDuration100ns", "outputTimeBaseNumerator", "prerollBoundaryTime100ns", "segmentStart100ns"]) and "m_outputTimelineMutex" in cpp and "packet.streamTimeOrigin100ns = m_streamTimeOrigin" in cpp)
check("Delivery worker owns all synthesized output delivery and uses packet snapshots", cpp.count("DeliverInterpolated(packet, outputPts)") == 1 and "deliveryHr = DeliverInterpolated(packet, outputPts)" in cpp and "AsyncOutputFrame& packet" in cpp)
check("Header declares packet-based delivery and removes the synchronous output pump", "HRESULT DeliverInterpolated(AsyncOutputFrame& packet, int64_t outputPts);" in hdr and "EmitSynchronousAvailableFrames" not in hdr)
check("Delivery worker never acquires Receive critical section", "CAutoLock receiveLock(&m_csReceive)" not in cpp[cpp.find("void CDirectMotion::DeliveryWorkerMain() noexcept"):cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery(")])
reset_engine_start = cpp.find("void CDirectMotion::ResetEngine(bool prepareResumeWarmStart) noexcept")
check("Processing worker is suspended and source waits interrupted before engine destruction", "if (!SuspendProcessingWorkerForEngineReset())" in cpp and reset_engine_start >= 0 and cpp.find("if (!SuspendProcessingWorkerForEngineReset())", reset_engine_start) < cpp.find("m_engine.reset();", reset_engine_start) and 'InterruptPendingSourceRequests("ResetEngine")' in reset_engine)
check("Stop cancels delivery before joining processing and resetting the engine", cpp.find('StopDeliveryWorker("DirectShow StopStreaming")') < cpp.find('StopProcessingWorker("DirectShow StopStreaming")') < cpp.find("ResetEngine();", cpp.find("HRESULT CDirectMotion::StopStreaming()")))
check("Local timestamp discontinuity advances output generation after downstream BeginFlush", cpp.find("m_pOutput->DeliverBeginFlush()", recv_start) < cpp.find("m_asyncPipeline.BeginGeneration()", recv_start) and "Receive/SeekGuard" in recv)
check("Unannounced discontinuity flag also invalidates queued output generation", "Receive/Discontinuity" in recv and "m_asyncPipeline.BeginGeneration()" in recv)
check("EOS finalizes source, waits for worker drain and forwards downstream EOS only afterwards", "RequestProcessingEndOfStream(generation)" in cpp and cpp.find("RequestProcessingEndOfStream(generation)", cpp.find("HRESULT CDirectMotion::EndOfStream()")) < cpp.find("return CTransformFilter::EndOfStream();", cpp.find("HRESULT CDirectMotion::EndOfStream()")))
check("Delivery worker completes optional terminal acknowledgements; generation replacement releases discarded completions", "if (activeCompletion) activeCompletion->Complete(" in cpp and "CompleteDiscarded(AsyncOutputFrame& frame)" in async_state_cpp and "0x80004004u" in async_state_cpp)
check("Delivery exceptions retire only the affected packet and preserve the long-lived downstream worker", "deliveryException = true;" in cpp and "activeCompletion->Complete(static_cast<int32_t>(0x8000FFFFu))" in cpp and "Recoverable delivery iteration exception" in cpp and 'm_asyncPipeline.FailGeneration(generation, "exception while delivering an output frame")' not in cpp)
delivery_start = cpp.find("HRESULT CDirectMotion::DeliverInterpolated(")
delivery_end = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(", delivery_start)
delivery_fn = cpp[delivery_start:delivery_end] if delivery_start >= 0 and delivery_end > delivery_start else ""
check("Generated output preroll prefers immutable packet timestamp boundary when available", "packet.prerollBoundaryTimeValid" in delivery_fn and "start < packet.prerollBoundaryTime100ns" in delivery_fn)
check("Generated output preroll falls back to immutable packet output-frame boundary for untimed first visible samples", "packet.prerollBoundaryOutputPtsValid" in delivery_fn and "outputPts < packet.prerollBoundaryOutputPts" in delivery_fn and "? start < packet.prerollBoundaryTime100ns" in delivery_fn)
check("Output preroll boundaries are traceable", '"Output/Preroll"' in delivery_fn and "boundary100ns=" in delivery_fn and "boundaryOutputPts=" in delivery_fn)
check("Output/Ready diagnostics expose preroll state and both visibility boundaries", "preroll=" in delivery_fn and "visibleBoundaryOutputPts=" in delivery_fn)
original_start = cpp.find("HRESULT CDirectMotion::DeliverOriginalSample(")
original_end = cpp.find("HRESULT CDirectMotion::", original_start + 1)
original_fn = cpp[original_start:original_end] if original_start >= 0 and original_end > original_start else ""
check("Paused-frame dynamic input type is normalized before source copy", "ApplyDynamicInputSampleType" in cpp and "ConvertInputToNv12(sourceBytes" in original_fn and "output->SetMediaType(&normalizedDynamicType)" not in original_fn)
check("Pass-through/control delivery retains the synchronous path", "DeliverDownstreamTracked(sample)" in recv and "DeliverDownstreamTracked(input)" in cpp and "DeliverDownstreamTracked(output)" in original_fn)
check("Downstream delivery remains ordinary blocking Deliver", "return m_pOutput->Deliver(sample);" in cpp and "AM_GBF_NOWAIT" not in cpp)
check("No private pending output queue or NOWAIT allocator retry was introduced", "m_pendingOutputFrame" not in cpp + hdr and "DeliverPendingSynchronousOutput" not in cpp + hdr and "AM_GBF_NOWAIT" not in cpp)
check("EOS drains AviSynth output through the processing worker before forwarding downstream EOS", "RequestProcessingEndOfStream(generation)" in cpp and "ProcessAvailableFrames(generation, terminalDrain, continuationRequired)" in cpp and "m_processingEosCompleted = true" in cpp)
check("BeginFlush propagates downstream flush before invalidating delivery queue generation", cpp.find("CTransformFilter::BeginFlush()", cpp.find("HRESULT CDirectMotion::BeginFlush()")) < cpp.find("m_asyncPipeline.BeginGeneration()", cpp.find("HRESULT CDirectMotion::BeginFlush()")))
check("NewSegment invalidates output generation before waiting for Receive lock", cpp.find("m_asyncPipeline.BeginGeneration()", cpp.find("HRESULT CDirectMotion::NewSegment(")) < cpp.find("CAutoLock receiveLock(&m_csReceive)", cpp.find("HRESULT CDirectMotion::NewSegment(")))

# Reset ordering must stop/wake Prefetch source requests before graph/runtime teardown.
reset_start = cpp.find("void CDirectMotion::ResetEngine(bool prepareResumeWarmStart) noexcept")
reset_end = cpp.find("HRESULT CDirectMotion::", reset_start + 1)
reset = cpp[reset_start:reset_end] if reset_start >= 0 and reset_end > reset_start else ""
check("Reset interrupts source waits before destroying the AviSynth engine", reset.find("m_engine->InterruptSourceWaits()") >= 0 and reset.find("m_engine->InterruptSourceWaits()") < reset.find("m_engine->Reset()"))
engine_reset = engine[engine.find("void AviSynthMvToolsEngine::Reset() noexcept"):]
check("Graph Reset releases Prefetch and dependent clips in order without destroying the persistent AviSynth environment", all(engine_reset.find(x) >= 0 for x in ["m_flowClip.reset()", "m_rawFlowClip.reset()", "m_mvForwardClip.reset()", "m_mvBackwardClip.reset()", "m_superClip.reset()", "m_sourceClip.reset()", "m_runtimeSession.reset()"]) and engine_reset.find("m_flowClip.reset()") < engine_reset.find("m_rawFlowClip.reset()") < engine_reset.find("m_mvForwardClip.reset()") < engine_reset.find("m_superClip.reset()") < engine_reset.find("m_sourceClip.reset()") < engine_reset.find("m_runtimeSession.reset()") and "DeleteScriptEnvironment" not in engine_reset)
session_destroy = engine[engine.find("void AviSynthRuntimeSession::DestroyEnvironment() noexcept"):engine.find("bool AviSynthRuntimeSession::EnsureReady(")]
session_ensure = engine[engine.find("bool AviSynthRuntimeSession::EnsureReady("):engine.find("bool AviSynthMvToolsEngine::BuildGraph(")]
check("DM-22.1 persistent session creates one environment and reuses it after the first successful initialization", all(x in session_ensure for x in ["if (m_ready && m_env)", "return true;", "CreateScriptEnvironment", "AvisynthPluginInit3", "m_ready = true"]) and "m_ready && m_env" in session_ensure and "if (createdNow) *createdNow = false;" in session_ensure)
check("DM-22.1 only the per-filter session destructor deletes IScriptEnvironment, after graph clips are gone", "m_env->DeleteScriptEnvironment()" in session_destroy and "DeleteScriptEnvironment" not in engine_reset and all(x in session_destroy for x in ["m_env = nullptr", "m_ready = false"]))
check("DM-22.1 session outlives graph engine and both graph initialization paths receive the same filter-owned session", hdr.find("m_avisynthRuntimeSession") < hdr.find("std::unique_ptr<ActiveFrucEngine> m_engine") and cpp.count("m_avisynthRuntimeSession, ") >= 2 and "const std::shared_ptr<AviSynthRuntimeSession>& runtimeSession" in engine_h)
check("DM-22.1 runtime registration is session-scoped while graph nodes are still rebuilt per generation", session_ensure.find("AvisynthPluginInit3") >= 0 and "AviSynthMvToolsEngine::BuildGraph" in engine and engine.find("BuildGraph(evaluationGate, error)") > engine.find("m_runtimeSession->EnsureReady(error"))
check("DM-22.1 setup and graph build latency are logged separately on success and graph-build failure", all(x in engine for x in ["AviSynth/GraphTiming", "runtimeSessionMs=", "graphBuildMs=", 'runtimeSessionCreated ? "created" : "reused"']))

# Output-rate selection: preserve old 2x/3x IDs, add isolated IDs for 2.5x/4x/Auto.
check("Rate-mode IDs preserve legacy 2x/3x values and isolate new modes", all(x in hdr for x in ["kRateMode2x = 0", "kRateMode3x = 2", "kRateMode25x = 0x445A", "kRateMode4x = 0x445B", "kRateModeAuto = 0x445C"]))
check("Property page exposes 2x, 2.5x, 3x, 4x and Auto", all(x in settings_cpp for x in ["2x source FPS", "2.5x source FPS", "3x source FPS", "4x source FPS", "Auto — display refresh"]))
check("Target rate calculates exact rational 2.5x and 4x modes", "source.num * 5, source.den * 2" in cpp and "source.num * 4, source.den" in cpp)
check("Auto rate uses exact display rational and does not downconvert faster sources", "return displayFps > sourceFps ? m_autoDisplayRate : source;" in cpp)
check("Auto obtains the actual EVR display HWND where available", "IMFVideoDisplayControl" in cpp and "GetVideoWindow(&videoWindow)" in cpp and "kMrVideoRenderService" in cpp)
check("Auto uses active QueryDisplayConfig rational refresh and maps it by GDI monitor name", "QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS" in cpp and "DISPLAYCONFIG_SOURCE_DEVICE_NAME" in cpp and "path.targetInfo.refreshRate.Numerator" in cpp)
check("Auto has visible same-process/primary-display fallback path and logs unavailable refresh", "foreground process-window fallback" in cpp and "largest visible process-window fallback" in cpp and "primary-monitor fallback" in cpp and "Auto safely falls back to 2x" in cpp)
check("Auto ignores unrelated foreground apps and rejects invalid display refresh modes", "foregroundProcess == GetCurrentProcessId()" in cpp and "IsWindowVisible(foreground)" in cpp and "refresh.num < 20 * refresh.den" in cpp and "refresh.num > 1000 * refresh.den" in cpp and "refresh.den > (std::numeric_limits<int>::max)()" in cpp)
check("MBlockFps returned fps is validated against exact target rational", "MBlockFps returned" in engine and "expectedOutputRate" in engine and "fps_numerator" in engine)
check("Frontier-relative Prefetch output fps remains identical to raw MBlockFps cadence", "Frontier-relative Prefetch changed the negotiated MBlockFps dimensions, format, cadence or frame range" in engine)
check("Cache source-frame requirement uses the effective rational target/source ratio", "const long double outputPerSource =" in engine and "static_cast<long double>(m_targetRate.num) * m_sourceRate.den" in engine and "static_cast<long double>(m_targetRate.den) * m_sourceRate.num" in engine and "std::ceil(needed)" in engine and "effectiveOutputPerSource=" in engine)
check("Cache plan logs actual source frames needed for its output Prefetch window", "cacheSourceFramesForPrefetchWindow=" in engine)

# Complete quick profile matrix: both motion-vector stages are profile-owned.
check("Quality reproduces the original 32px exhaustive -> 8px exhaustive chain", all(x in engine for x in [
    "m_performance == 1) ? 3", "m_performance == 1) ? 3", "m_performance == 1) ? -24",
    "const bool quickFastRefinement", "quickFastRefinement ? 16 : 8", "quickFastRefinement ? 0 : 3", "quickFastRefinement ? 1 : 0",
    "profileAnalyseBlockSize = 32"
]))
check("Balanced uses 32px Hexagon search and retains 8px exhaustive refinement", all(x in engine for x in [
    "(m_performance == 3) ? 0 : 4", "(m_performance == 3) ? 1 : 2",
    "(m_performance == 1) ? -24 : (m_performance == 2) ? 24 : 0",
    "const bool quickFastRefinement", "quickFastRefinement ? 16 : 8", "quickFastRefinement ? 0 : 3"
]))
check("Fast uses OneTime search in both stages and 16px refinement", all(x in engine for x in [
    "(m_performance == 3) ? 0 : 4", "(m_performance == 3) ? 1 : 2",
    "m_performance == 3 || m_quickRecalcMode == 1", "quickFastRefinement ? 16 : 8",
    "quickFastRefinement ? 0 : 3", "quickFastRefinement ? 1 : 0"
]))
check("Quick profile log records complete profile name and both refinement parameters", all(x in engine for x in [
    'm_performance == 1 ? "Quality"', 'm_performance == 2 ? "Balanced" : "Fast"',
    '",mt=false) -> MRecalculate("', 'std::to_string(recalcBlock)', 'std::to_string(recalcSearch)',
    'std::to_string(recalcSearchParam)'
]))
check("Graph log identifies profile-default versus explicit MAnalyse grid override", "MAnalyseGridOverride=" in engine and "profile-default" in engine and '"enabled"' in engine)
check("Quick profile retains MSuper defaults and distinguishes chroma default from vector-stage chroma", 'default(true)' in engine and 'm_options.advanced ? m_options.analyseChroma : false' in engine)
check("Bad-vector wide second search is not falsely implied by badrange alone", 'badSAD=10000(disabled)' in engine and "badrange" in engine)
check("MAnalyse grid defaults to profile's 32px and custom grid is an explicit first-pass override", all(x in engine for x in [
    "profileAnalyseBlockSize = 32", "(m_requestedGrid == 1) ? 4", "(m_requestedGrid == 8) ? 32"
]) and "gridOverrideEnabled ? m_settings.grid : 0" in cpp)
check("Grid override state persists and old default Grid=4 migrates to profile default", all(x in cpp for x in [
    'readDword(L"GridOverrideEnabled", legacyGridOverrideDefault)',
    'legacyGridOverrideDefault =', '(m_settings.grid != 0 && m_settings.grid != 4)',
    '{L"GridOverrideEnabled", m_settings.gridOverrideEnabled ? 1u : 0u}'
]))
check("Property page exposes Profile default and labels MAnalyse-only grid override", all(x in settings_cpp for x in [
    'L"Profile default"', 'L"4 px"', "candidate.gridOverrideEnabled = gridIndex != 0"
]))
check("Settings rebuild when the grid override enablement changes", "requested.gridOverrideEnabled != m_settings.gridOverrideEnabled" in cpp)

# Exact rational-rate and source-cache arithmetic examples for the requested modes.
rate_examples = [
    Fraction(24000, 1001) * Fraction(5, 2) == Fraction(60000, 1001),
    Fraction(24, 1) * Fraction(5, 2) == Fraction(60, 1),
    Fraction(25, 1) * Fraction(12, 5) == Fraction(60, 1),
    Fraction(30, 1) * Fraction(2, 1) == Fraction(60, 1),
    Fraction(15, 1) * Fraction(4, 1) == Fraction(60, 1),
]
check("Rational examples cover 23.976-to-59.94, 24-to-60, 25-to-60, 30-to-60 and 15-to-60", all(rate_examples))
prefetch_frames = 16
cache_margin = 2 + 4 + 24  # MVTools look-ahead + safety + independent history margin
planned_cache_by_ratio = {
    "2x": math.ceil(prefetch_frames / 2.0) + cache_margin,
    "2.5x": math.ceil(prefetch_frames / 2.5) + cache_margin,
    "3x": math.ceil(prefetch_frames / 3.0) + cache_margin,
    "4x": math.ceil(prefetch_frames / 4.0) + cache_margin,
    "25-to-60-Auto": math.ceil(prefetch_frames / 2.4) + cache_margin,
}
check("Source-cache window responds to effective multiplier with separate historical margin: 2x=38, 2.5x=37, 3x=36, 4x=34", [planned_cache_by_ratio[k] for k in ["2x", "2.5x", "3x", "4x"]] == [38, 37, 36, 34])
check("Auto 25-to-60 cache conversion uses 2.4x rather than rounding to an integer multiplier", planned_cache_by_ratio["25-to-60-Auto"] == 37)

# Check worker ownership through executable call sites rather than comment text.
check("AviSynth source publication, frame evaluation and downstream delivery remain on their dedicated owners", "m_engine->PushSourceNv12(" in cpp and "m_engine->EmitBootstrapFrame(" in cpp and "m_engine->EmitNextAvailable(" in cpp and "m_pOutput->Deliver(sample)" in cpp)
# 0.2.52 queue/lifecycle foundation remains the base for the 0.2.53 output worker.
check("AsyncPipelineState is owned by CDirectMotion and active callbacks use its generation/output queue", "AsyncPipelineState m_asyncPipeline" in hdr and "m_asyncPipeline.BeginGeneration()" in cpp and "m_asyncPipeline.WaitEnqueueOutput" in cpp)
check("Async source/output packets own normalized NV12 payloads without COM sample pointers", all(x in async_state_h for x in ["struct AsyncSourceFrame", "struct AsyncOutputFrame", "std::vector<uint8_t> nv12", "streamStart100ns", "typeSpecificFlags"]) and "IMediaSample" not in async_state_h)
check("Input queue publication is explicitly non-blocking and reports Full", "TryEnqueueInput(AsyncSourceFrame&& frame)" in async_state_h and "m_impl->input.TryPush" in async_state_cpp and "AsyncQueueStatus::Full" in async_state_cpp)
check("Output queue producer waits only on its own bounded capacity", "WaitEnqueueOutput(AsyncOutputFrame&& frame)" in async_state_h and "m_impl->output.WaitPush" in async_state_cpp and "m_notFull.wait" in async_state_cpp)
check("Both queues are generation-aware and reject stale work", all(x in async_state_cpp for x in ["generation != m_generation", "AsyncQueueStatus::StaleGeneration", "m_impl->input.Reset(next)", "m_impl->output.Reset(next)", "WaitPop(generation, frame)"]))
check("Normal completion closes queues while preserving accepted FIFO items", "Close(false, false)" in async_state_cpp and "m_items.empty()" in async_state_cpp and "m_items.pop_front()" in async_state_cpp)
check("Cancellation discards queued work and wakes both queue wait conditions", "Close(true, true)" in async_state_cpp and "m_notEmpty.notify_all()" in async_state_cpp and "m_notFull.notify_all()" in async_state_cpp)
check("Worker state and queue snapshot are generation-scoped", all(x in async_state_h for x in ["AsyncWorkerKind", "AsyncWorkerState", "AsyncPipelineSnapshot"]) and "SetWorkerState(Generation generation" in async_state_cpp and "AsyncPipelineSnapshot AsyncPipelineState::Snapshot()" in async_state_cpp)
check("Standalone C++17 test covers bounded queues, FIFO, close, cancellation and generation changes", all(x in async_state_test for x in ["AsyncPipelineState state(2, 1)", "AsyncQueueStatus::Full", "MarkInputComplete", "CancelGeneration", "StaleGeneration", "FailGeneration"]))
check("New async state implementation and test are part of the source package/project", all(x in read("directmotion/directmotion.vcxproj") for x in ["AsyncPipelineState.cpp", "AsyncPipelineState.h"]) and (ROOT / "tools/test_async_pipeline_state.cpp").exists())
check("Pause quiesces in-flight delivery and retains an active EOS generation", "std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex);" in cpp[cpp.find("STDMETHODIMP CDirectMotion::Pause()"):cpp.find("STDMETHODIMP CDirectMotion::Stop()") ] and "terminalEosActive = m_processingEosRequested && !m_processingEosCompleted" in cpp)
check("Terminal output is marked as preroll during Pause and remains deliverable through the pause gate", "packet.terminalDrain" in cpp and "terminalPausePreroll" in cpp and "DeliverDownstreamTracked(output, packet.terminalDrain)" in cpp)
check("Terminal pause cancellation does not suppress an established runtime failure", "!m_runtimeFailed.load(std::memory_order_acquire)" in cpp[cpp.find('"AsyncDelivery/TerminalPreroll"')-500:cpp.find('"AsyncDelivery/TerminalPreroll"')+100])
check("Terminal pause allocator cancellation completes the EOS acknowledgement without runtime failure", '"AsyncDelivery/TerminalPreroll"' in cpp and "deliveryHr = S_OK;" in cpp)
check("Receive-side explicit interpolation toggle fences delivery with downstream flush", '"Tray/RuntimeToggle/BeginFlush"' in cpp and '"Tray/RuntimeToggle/EndFlush"' in cpp and "DeliverBeginFlush()" in recv)
check("Unannounced discontinuity fences delivery even without reliable source timestamps", '"Receive/Discontinuity/BeginFlush"' in cpp and '"Receive/Discontinuity/EndFlush"' in cpp)
check("NewSegment locally flushes downstream when upstream did not already flush", '"NewSegment/LocalFlush"' in cpp and "ownsDownstreamFlush" in segment_cb and "DeliverEndFlush()" in segment_cb)
check("Delivered-source progress is accounted under the delivery lock rather than on worker tail", "RecordOutputProgress(deliveredProgress" in delivery_fn and cpp.find("RecordOutputProgress(deliveredProgress") > cpp.find("HRESULT CDirectMotion::DeliverInterpolated") and "RecordOutputProgress(deliveredProgress" not in cpp[cpp.find("void CDirectMotion::DeliveryWorkerMain() noexcept"):cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery(")])

check("Tray menu refreshes from a dedicated thread timer inside the modal menu loop", all(x in runtime_diag for x in ["SetTimer(nullptr, kTrayMenuRefreshTimerId", "TrayMenuRefreshTimerProc", "RefreshOpenTrayMenu(hwnd)", "KillTimer(nullptr, g_openTrayMenuTimer)"]))
check("Tray menu refresh updates seven live status rows and forces popup repaint", all(x in runtime_diag for x in ["kTrayActivityRowCount = 7", "SetMenuItemInfoW(menu, kTrayActivityFirstPosition", "RedrawWindow(popup", 'FindWindowW(L"#32768"', "CURRENT:", "SOURCE WAIT:", "PROGRESS:"]))
check("Tray menu uses a separate modal-loop timer instead of relying only on window WM_TIMER", "SetTimer(nullptr, kTrayMenuRefreshTimerId" in runtime_diag and "falling back to the tray window timer" in runtime_diag)
check("Bootstrap source lead resets safely with each engine generation", "m_bootstrapInputFramesRequired = DirectMotionBootstrapEvaluation::RequiredSourceFramesForFirstOutput(kMotionLookaheadFrames);" in engine[engine.find("void AviSynthMvToolsEngine::Reset()") :])
check("Bootstrap policy saturates integer lookahead and clamps invalid negative values", "safeLookahead >= INT_MAX ? INT_MAX : safeLookahead + 1" in bootstrap_eval_h and "RequiredSourceFramesForFirstOutput(-5)" in bootstrap_eval_test and "RequiredSourceFramesForFirstOutput(INT_MAX)" in bootstrap_eval_test)
check("Prefetch-relative indexing starts at zero for the first successor after direct bootstrap", "UsesPrefetchForOutput" in engine and "outputIndex >= firstPrefetchOutputIndex" in bootstrap_eval_h and "PrefetchRelativeIndex(61, 61) == 0" in bootstrap_eval_test and "counts.direct == 1 && counts.prefetch == 128" in bootstrap_eval_test)
check("Seek frontier is atomically sealed before waiting for its source lookahead", "m_seekFallbackAcceptingSource = false;" in bootstrap_block and bootstrap_block.find("m_seekFallbackAcceptingSource = false;") < bootstrap_block.find("if (!m_engine->CanEmitOutputIndex(bootstrapStartOutputIndex))") and "Snapshot the current fallback PTS frontier immediately" in bootstrap_block and "The PTS frontier cannot move afterward" in bootstrap_block)
check("Invalid seek frontier failure runs outside the fallback mutex", "if (invalidFrontier)" in cpp and cpp.find("if (invalidFrontier)") < cpp.find('FailRuntime("AsyncProcessing/SeekFrontier"') and "never invoke it while" in cpp)
check("Cold-start source gate is reset to the same direct-bootstrap lookahead policy", "m_bootstrapInputFramesRequired = DirectMotionBootstrapEvaluation::RequiredSourceFramesForFirstOutput(kMotionLookaheadFrames);" in engine[engine.find("void AviSynthMvToolsEngine::Reset()") :])

# 0.2.56 observability: bounded queue counters and independently sampled worker activity.
check("Queue metrics expose depth/capacity/high-water and accepted/dequeued/discarded counts", all(x in async_state_h for x in ["struct AsyncQueueMetrics", "highWater", "enqueued", "dequeued", "discarded"]))
check("Queue metrics count full/stale/cancelled/closed rejections", all(x in async_state_h for x in ["fullRejects", "staleRejects", "cancelledRejects", "closedRejects"]))
check("Queue metrics capture producer and consumer wait count/total/max microseconds", all(x in async_state_h for x in ["producerWaits", "producerWaitTotalUs", "producerWaitMaxUs", "consumerWaits", "consumerWaitTotalUs", "consumerWaitMaxUs"]))
check("Queue producer wait timing is recorded only for capacity-blocked pushes", "const bool waited = m_items.size() >= m_capacity" in async_state_cpp and "RecordProducerWaitLocked" in async_state_cpp)
check("Queue consumer wait timing is recorded only for empty-open pops", "const bool waited = m_items.empty()" in async_state_cpp and "RecordConsumerWaitLocked" in async_state_cpp)
check("Diagnostic counters do not participate in queue capacity or wait predicates", not re.search(r"(?:if|while)\s*\([^)]*m_metrics", async_state_cpp))
check("Generation reset and cancellation count discarded queued packets", "m_metrics.discarded += static_cast<uint64_t>(discarded.size())" in async_state_cpp)
check("Worker busy state and completed work-item counts are part of the immutable snapshot", all(x in async_state_h for x in ["processingBusy", "deliveryBusy", "processingWorkItems", "deliveryWorkItems"]) and "SetWorkerBusy(Generation generation" in async_state_h)
check("Processing worker marks active GetFrame passes busy and clears the state on exit", "SetWorkerBusy(generation, AsyncWorkerKind::Processing, true)" in cpp and "SetWorkerBusy(generation, AsyncWorkerKind::Processing, false)" in cpp)
check("Delivery worker marks active packet consumption busy and clears the state on exit", "SetWorkerBusy(generation, AsyncWorkerKind::Delivery, true)" in cpp and "SetWorkerBusy(generation, AsyncWorkerKind::Delivery, false)" in cpp)
check("Tray exposes generation, worker activity, FIFO depth/high-water and queue waits", all(x in cpp for x in ["Pipeline gen %llu", "Output FIFO %zu/%zu high %zu", "FIFO waits: producer", "pipeline.output"]))
check("Persistent telemetry log is throttled by an independent 250ms sampler to five-second intervals", all(x in cpp for x in ["DiagnosticsWorkerMain", "std::chrono::milliseconds(250)", "LogAsyncPipelineTelemetry();", "now - previous < 5000", "AsyncPipeline/Telemetry"]))
check("Pipeline telemetry logs queue waits, worker counts, rejections and stage timings", all(x in cpp for x in ["producerWaitAvgMs=", "consumerWaitMaxMs=", "processingWorkItems=", "deliveryWorkItems=", "outputFullRejects=", "outputStaleRejects=", "getFrameAvgMs=", "allocatorLastMs=", "downstreamAvgMs="]))
check("Work-phase ownership is per thread and Idle cannot erase another thread's operation", all(x in runtime_diag for x in ["std::map<DWORD, std::vector<ActiveWorkRecord>> g_activeWorkByThread", "GetCurrentThreadId()", "g_activeWorkByThread.find(threadId)", "stack.pop_back()"]))
check("Nested tagged operations preserve their parent scope on the same thread", "g_activeWorkByThread[threadId].push_back(std::move(record))" in runtime_diag and "if (stack.empty()) g_activeWorkByThread.erase(found)" in runtime_diag)
check("Tagged per-thread work is cleared on exceptional Receive and worker exits", all(x in runtime_diag for x in ["void DirectMotionTrayClearCurrentThreadWork() noexcept", "g_activeWorkByThread.erase(found)"]) and all(x in cpp for x in ["~ReceiveWorkPhaseCleanup() noexcept", "DirectMotionTrayClearCurrentThreadWork();"]))
check("Watchdog enumerates the current innermost operation on every active thread", all(x in runtime_diag for x in ["for (auto& item : g_activeWorkByThread)", "ActiveWorkRecord& record = stack.back()", "activeTaggedThreads="]))
check("Independent diagnostics worker is started and stopped with the streaming lifecycle", all(x in cpp for x in ["StartDiagnosticsWorker();", "StopDiagnosticsWorker();", "DiagnosticsWorkerMain()", "m_diagnosticsWorkerThread.join()", "m_diagnosticsWorkerStop.store(true"]) and "std::thread m_diagnosticsWorkerThread" in hdr)
check("Independent telemetry reports active per-thread work outside the Receive refresh path", "activeTaggedThreads=" in cpp and "DirectMotionTrayGetActiveWorkCount()" in cpp and "DirectMotionTrayGetActiveWorkCount()" in runtime_diag)
check("Independent diagnostic worker refreshes only the dynamic tray pipeline rows without reading engine pointers", "RefreshAsyncPipelineTrayInfo(stallStatus);" in cpp and "DirectMotionTraySetRuntimeInfoRows(8, lines)" in cpp and "void DirectMotionTraySetRuntimeInfoRows" in runtime_diag and "std::min)(lines.size(), kMaxRuntimeRows - startIndex)" in runtime_diag)
check("Dynamic tray pipeline rows refresh every 500ms even when file logging is disabled", "now - previous < 500" in cpp and cpp.find("RefreshAsyncPipelineTrayInfo();", cpp.find("void CDirectMotion::DiagnosticsWorkerMain")) < cpp.find("if (!DirectMotionIsLoggingEnabled())", cpp.find("void CDirectMotion::DiagnosticsWorkerMain")))
check("Diagnostic counters distinguish received, normalized, published, generated and interpolated-delivered frames", all(x in hdr and x in cpp for x in ["m_diagInputReceivedFrames", "m_diagInputNormalizedFrames", "m_diagInputPublishedFrames", "m_diagOutputGeneratedFrames", "m_diagOutputDeliveredFrames"]))
check("Receive counter advances only for media samples and before normalization", "m_diagInputReceivedFrames.fetch_add(1" in cpp and cpp.find("m_diagInputReceivedFrames.fetch_add(1") < cpp.find("ApplyDynamicInputSampleType(sample") and cpp.find("if (sampleProps && sampleProps->dwStreamId != AM_STREAM_MEDIA)") < cpp.find("m_diagInputReceivedFrames.fetch_add(1"))
check("Normalized and published counters advance only after their respective stages succeed", cpp.find("m_diagInputNormalizedFrames.fetch_add(1") > cpp.find("const bool inputConverted = ConvertInputToNv12") and cpp.find("m_diagInputPublishedFrames.fetch_add(1") > cpp.find("const HRESULT pushHr = m_engine->PushSourceNv12"))
check("Generated counter advances after successful AviSynth bootstrap or available output", "m_diagOutputGeneratedFrames.fetch_add(1" in cpp and "if (!bootstrapReady)" in cpp and "if (!available)" in cpp)
check("Interpolated delivery counter advances after successful downstream delivery", cpp.find("m_diagOutputDeliveredFrames.fetch_add(1") > cpp.find("HRESULT CDirectMotion::DeliverInterpolated") and cpp.find("m_diagOutputDeliveredFrames.fetch_add(1") > cpp.find("if (FAILED(hr)) return hr;", cpp.find("HRESULT CDirectMotion::DeliverInterpolated")))
check("Tray runtime detail retains all thirteen current diagnostics rows", "g_runtimeInfo.size() > 13" in runtime_diag and "lines.reserve(13)" in cpp)
check("C++17 tests cover queue pressure metrics, worker busy counts and consumer idle waits", all(x in async_state_test for x in ["fullRejects", "producerWaitMaxUs", "consumerWaitMaxUs", "SetWorkerBusy", "highWater"]))

# FIX-2: detect and capture the first stalled boundary without influencing media flow.
check("Portable stall classifier defines all pipeline boundaries and a conservative 1.5s threshold", all(x in stall_diag_h for x in [
    "ReceiveNormalization", "SourceCachePublication", "OutputQueueBackpressure", "DownstreamDelivery",
    "PrefetchSourceWait", "AviSynthProcessing", "ProcessingNotScheduled", "UpstreamInputIdle",
    "kPipelineStallThresholdMs = 1500"]))
check("Stall classifier distinguishes pending work or the 128-frame pass-cap marker from normal future-source lookahead", "state.processingPending || state.processingPassCapped" in stall_diag_h and "Prefetch may legitimately wait for input" in stall_diag_h)
check("F-001 pass-cap condition is surfaced diagnostically and cleared at next pass and engine reset", all(x in cpp for x in ["m_diagProcessingPassCapped.store(true", "m_diagProcessingPassCapped.store(false", "processingPassCapped=", "observation.processingPassCapped"]) and "m_diagProcessingPassCapped.store(false" in cpp[cpp.find("bool CDirectMotion::SuspendProcessingWorkerForEngineReset"):cpp.find("void CDirectMotion::ResumeProcessingWorkerAfterEngineReset")] and "state.processingPending || state.processingPassCapped" in stall_diag_h and "128-frame pass cap without a follow-up pass" in stall_diag_test)
check("Stall classifier requires sustained delivery stagnation before labelling a full FIFO", "state.deliverAgeMs >= kPipelineStallThresholdMs" in stall_diag_h and "state.outputQueued >= state.outputCapacity" in stall_diag_h)
check("Output FIFO capacity wait is tagged independently from AviSynth GetFrame", "DirectMotionWorkPhase::OutputQueue" in cpp and "WaitEnqueueOutput / wait for output FIFO capacity" in cpp and "output FIFO capacity wait" in runtime_diag)
check("Terminal EOS barrier enqueue is tagged for stall snapshots", "WaitEnqueueOutput / enqueue terminal EOS drain barrier" in cpp)
check("Diagnostic snapshot enumerates every active thread operation and source wait range", all(x in runtime_diag for x in ["DirectMotionTrayGetDiagnosticContext() noexcept", "g_activeWorkByThread", "sourceWaiters=", "sourceRequested=", "sourceCache="]))
check("Independent stall monitor is gated to active Run and ignores Pause/Flush/Stop/segment transitions", all(x in cpp for x in ["graphActivelyRunning", "m_runActive.load", "m_pauseTransition.load", "m_pauseCallInProgress.load", "m_flushTransition.Active", "m_segmentTransition.Active", "DirectMotionTrayInterpolationEnabled()" ]))
check("Stall monitor samples all five stage counters and records per-stage progress ages", all(x in cpp for x in ["stageLastProgress[5]", "m_diagInputReceivedFrames.load", "m_diagInputNormalizedFrames.load", "m_diagInputPublishedFrames.load", "m_diagOutputGeneratedFrames.load", "m_diagOutputDeliveredFrames.load", "stageAgeMsRxNv12CacheGeneratedDelivered=" ]))
tray_counter_block = cpp[cpp.find("void CDirectMotion::RefreshAsyncPipelineTrayInfo"):cpp.find("void CDirectMotion::LogAsyncPipelineTelemetry")]
check("Tray stage labels map once and in order to RX, NV12, published cache, generated and delivered counters", all(x in tray_counter_block for x in ["Stages: RX %llu | NV12 %llu | cache %llu | gen %llu | interp-delivered %llu", "m_diagInputReceivedFrames.load", "m_diagInputNormalizedFrames.load", "m_diagInputPublishedFrames.load", "m_diagOutputGeneratedFrames.load", "m_diagOutputDeliveredFrames.load"]) and all(tray_counter_block.count(x) == 1 for x in ["m_diagInputReceivedFrames.load", "m_diagInputNormalizedFrames.load", "m_diagInputPublishedFrames.load", "m_diagOutputGeneratedFrames.load", "m_diagOutputDeliveredFrames.load"]))
check("Processing pending state is sampled under its owning mutex without holding it during media work", "processingPending = m_processingWorkPending" in cpp and "std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex)" in cpp[cpp.find("void CDirectMotion::DiagnosticsWorkerMain"):cpp.find("void CDirectMotion::RefreshAsyncPipelineTrayInfo")])
check("Stall snapshots include generation, worker states, FIFO metrics, operation context and stage timings", all(x in cpp for x in ["AsyncPipeline/StallSnapshot", "processingWorkerState=", "deliveryWorkerState=", "activeWork={", "outputQueue=", "allocatorLastMs=", "downstreamLastMs=" ]))
check("Stall snapshot emits immediately on category transition and repeats no more than every five seconds", "newCategory || repeatDue" in cpp and "now - lastStallSnapshotTick >= 5000" in cpp)
check("Tray exposes the current stall classification even when file logging is disabled", "STALL: " in cpp and "DirectMotionTraySetRuntimeInfoRows(8, lines)" in cpp and "lines.emplace_back(stallStatus)" in cpp)
check("Stall monitor grants downstream stages a fresh grace period on first upstream activity", all(x in cpp for x in ["if (counts[0] > 0 && counts[1] == 0) stageLastProgress[1] = now", "if (counts[3] > 0 && counts[4] == 0) stageLastProgress[4] = now"]))
check("Portable C++17 classifier test covers FIFO pressure, downstream waits, source-cache pressure/expiry, Prefetch, GetFrame and scheduling", all(x in stall_diag_test for x in ["full FIFO wins", "downstream delivery stall", "Prefetch waiting for source", "AviSynth/MVTools processing stall", "pending work without active processing", "brief FIFO saturation is not called a stall", "expired source frame", "source cache producer backpressure"]))
check("Stall classifier header is part of the Visual Studio project", "PipelineStallDiagnostics.h" in read("directmotion/directmotion.vcxproj"))
check("PauseFrameGate header is part of the Visual Studio project", "PauseFrameGate.h" in read("directmotion/directmotion.vcxproj"))
check("ResumeWarmStart policy header is part of the Visual Studio project", "ResumeWarmStart.h" in read("directmotion/directmotion.vcxproj"))
check("ProcessingContinuation policy header is part of the Visual Studio project", "ProcessingContinuation.h" in read("directmotion/directmotion.vcxproj"))
check("ProcessingQuiescence policy header is part of the Visual Studio project", "ProcessingQuiescence.h" in read("directmotion/directmotion.vcxproj"))
check("ProcessingGenerationPolicy is part of the Visual Studio project and shared with a portable regression", "ProcessingGenerationPolicy.h" in read("directmotion/directmotion.vcxproj") and "MatchesEngineGeneration" in processing_generation_test)
worker_start = cpp.find("void CDirectMotion::ProcessingWorkerMain() noexcept")
worker_end = cpp.find("HRESULT CDirectMotion::RequestProcessingEndOfStream", worker_start)
worker_fn = cpp[worker_start:worker_end] if worker_start >= 0 and worker_end > worker_start else ""
process_start = cpp.find("HRESULT CDirectMotion::ProcessAvailableFrames(")
process_end = cpp.find("void CDirectMotion::ProcessingWorkerMain() noexcept", process_start)
process_fn = cpp[process_start:process_end] if process_start >= 0 and process_end > process_start else ""
queue_start = cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery(")
queue_end = cpp.find("HRESULT CDirectMotion::QueueDeliveryDrainBarrier", queue_start)
queue_fn = cpp[queue_start:queue_end] if queue_start >= 0 and queue_end > queue_start else ""
early_engine_guard = process_fn.find("MatchesEngineGeneration(generation, engineGeneration)")
first_engine_access = process_fn.find("m_engine->")
check("ProcessingWorker rejects a FIFO/engine generation mismatch before any engine dereference", early_engine_guard >= 0 and first_engine_access > early_engine_guard and "m_engineGeneration.load(std::memory_order_acquire)" in process_fn)
check("ProcessingWorker interruption predicate rechecks engine generation during a pass", "generation, m_engineGeneration.load(std::memory_order_acquire))" in process_fn and "processingWasInterrupted()" in process_fn)
emit_result_pos = process_fn.find("const bool ok = m_engine->EmitNextAvailable")
post_emit_guard_pos = process_fn.find("if (processingWasInterrupted()) return S_OK;", emit_result_pos)
check("ProcessingWorker preserves a completed GetFrame across strict Pause but rejects later invalidation", emit_result_pos >= 0 and "ShouldQueueCompletedFrameAfterPause" in process_fn and "QueueInterpolatedForDelivery(\n                    frame, false, generation, false)" in process_fn and "IsGraphTransitioning()" in process_fn[emit_result_pos:process_fn.find("if (!ok)", emit_result_pos)])
check("Output enqueue independently rejects packets whose engine identity does not match the FIFO generation", "MatchesEngineGeneration(" in queue_fn and "m_engineGeneration.load(std::memory_order_acquire)" in queue_fn and queue_fn.find("MatchesEngineGeneration(") < queue_fn.find("AsyncOutputFrame packet"))
check("Generation-guard regression covers the Run race, reset gap, stale pass and valid rebuilt engine", all(x in processing_generation_test for x in ["regression: Run cannot relabel work", "while ResetEngine has no active engine", "only after the rebuilt engine", "delayed old-generation worker pass", "large equal generation identifiers", "concurrent regression: a pending worker woken after FIFO rollover cannot touch the old engine", "concurrent regression: the rebuilt graph resumes work under the new FIFO generation"]))
check("Windows CI compiles and retains the FIX-5.6 processing-generation regression", all(x in workflow for x in ["Run portable processing-worker generation guard tests", "test_processing_generation_policy.cpp", "PROCESSING_GENERATION_POLICY_TEST_RUN_0.2.57_FIX5_6.txt"]))
check("Windows CI compiles and retains the FIX-5.7 cold-start preroll regression", all(x in workflow for x in ["Run portable initial-preroll admission tests", "test_initial_preroll_policy.cpp", "INITIAL_PREROLL_POLICY_TEST_RUN_0.2.57_FIX5_7.txt"]))
check("FIX-5.8 pause policy header and portable regression are wired into production and source inventory", all(x in hdr for x in ['#include "PauseRetentionPolicy.h"']) and "PauseRetentionPolicy.h" in read("directmotion/directmotion.vcxproj") and "pause-cancelled active packet retries after Run" in read("tools/test_pause_retention_policy.cpp"))
pause_policy_h = read("directmotion/PauseRetentionPolicy.h")
pause_policy_test = read("tools/test_pause_retention_policy.cpp")
check("Pause retention policy rejects terminal drain and lifecycle invalidation", all(x in pause_policy_h for x in ["strictPauseHold && !terminalDrain && !lifecycleInvalidated", "!terminalDrain", "generationCurrent", "!stopping", "expectedPauseCancellation"]))
check("Pause retention portable truth-table covers active packet retry, ordered frame retention and stale lifecycle rejection", all(x in pause_policy_test for x in ["pause-cancelled active packet retries after Run", "stale generation is not retried", "completed GetFrame is retained during strict pause", "completed frame during seek/flush is rejected"]))
check("DM-20.1 pure pause policy suspends ordinary processing, exempts terminal EOS drain and protects the in-flight GetFrame publication race", "ShouldSuspendProcessing" in pause_policy_h and "strictPauseHold && !terminalDrain" in pause_policy_h and "WasGetFrameInterruptedByStrictPause" in pause_policy_h and all(x in pause_policy_test for x in ["strict Pause suspends further interpolated frame production", "normal Running continues interpolated frame production", "terminal EOS drain retains its existing processing policy", "active hold detects GetFrame completion before strict pause epoch publication", "pause epoch detects a GetFrame that completed after Run already released hold"]))
check("DM-20.1 ProcessingWorker checks strict Pause before starting more MVTools output while preserving an in-flight GetFrame even when Run races the return", "ShouldSuspendProcessing(\n                m_pauseHoldActive.load(std::memory_order_acquire), terminalDrain)" in process_fn and "(!terminalDrain && pauseEpochChanged)" in process_fn and "WasGetFrameInterruptedByStrictPause" in process_fn and "ShouldQueueCompletedFrameAfterPause" in process_fn and "QueueInterpolatedForDelivery(\n                    frame, false, generation, false)" in process_fn)
pause_delivery_start = cpp.find("bool CDirectMotion::WaitForDeliveryAdmission(")
pause_delivery_end = cpp.find("void CDirectMotion::DeliveryWorkerMain()", pause_delivery_start)
pause_delivery_block = cpp[pause_delivery_start:pause_delivery_end]
check("Delivery worker parks dequeued output while strict pause is active", "ShouldHoldDelivery" in pause_delivery_block and "m_pauseHoldActive" in pause_delivery_block)
delivery_worker_start = cpp.find("void CDirectMotion::DeliveryWorkerMain()")
delivery_worker_end = cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery", delivery_worker_start)
delivery_worker_block = cpp[delivery_worker_start:delivery_worker_end]
check("Delivery worker retries only the same pause-interrupted packet after Run and before FIFO successors", all(x in delivery_worker_block for x in ["WaitForDeliveryAdmission(generation, packet.terminalDrain)", "ShouldRetryInterruptedDelivery", "packet.sequence", "packet.pts", "this exact slot remains ahead of queued successors"]))
check("Strict Pause retains the current generation and does not interrupt/reset the live source history", all(x in pause_block for x in ["newGeneration = oldGeneration", "Preserved the running output timeline and AviSynth source cache"]) and "BeginGeneration()" not in pause_block and "InterruptPendingSourceRequests(\"Pause/RunToPaused\")" not in pause_block and "m_processingNeedsReset.store(true" not in pause_block)
check("Strict Pause publishes the monotonic epoch before its interruption flag", pause_block.find("m_strictPauseEpoch.fetch_add(1, std::memory_order_acq_rel)") >= 0 and pause_block.find("m_strictPauseEpoch.fetch_add(1, std::memory_order_acq_rel)") < pause_block.find("m_pauseTransition.store(true, std::memory_order_release)"))
check("Failed strict Run republishes a new pause epoch before restoring the paused interruption flag", "if (resumedAfterStrictPauseHold) {\n            m_strictPauseEpoch.fetch_add(1, std::memory_order_acq_rel);\n            m_pauseHoldActive.store(true, std::memory_order_release);\n        }\n        m_pauseTransition.store(true, std::memory_order_release);" in run_block)
check("Run releases strict delivery hold only inside the successful base Run branch", "const HRESULT hr = CTransformFilter::Run(start)" in run_block and "if (resumedAfterStrictPauseHold) {\n            m_pauseHoldActive.store(false" in run_block[run_block.find("const HRESULT hr = CTransformFilter::Run(start)"):] and "NotifyDeliveryWorker()" in run_block[run_block.find("const HRESULT hr = CTransformFilter::Run(start)"):])
check("Pause/Run delivery wake notifications cover lifecycle cancellation", all(x in cpp for x in ["NotifyDeliveryWorker();", "void CDirectMotion::NotifyDeliveryWorker()", "m_deliveryPauseChanged.notify_all()"]))
check("Windows CI compiles and retains the pause-retention regression including the new processing-hold cases", all(x in workflow for x in ["Run portable Pause/Run timeline-retention tests", "test_pause_retention_policy.cpp", "PAUSE_RETENTION_POLICY_TEST_RUN_0.2.58.21.txt"]))
check("F-001 pass cap requests a continuation only after a complete 128-output non-terminal pass", "continuationRequired = true" in process_fn and "emittedThisPass >= kMaxFramesPerPass" in process_fn and "!terminalDrain" in process_fn and "continuationRequired = false" in process_fn)
check("F-001 continuation is re-armed under the processing-worker mutex with generation and lifecycle guards", all(x in worker_fn for x in ["std::lock_guard<std::mutex> stateLock(m_processingWorkerMutex)", "ShouldRearmAfterPass(", "m_processingWorkerStop", "m_processingWorkerSuspend", "m_asyncPipeline.IsCurrentGeneration(generation)", "IsGraphTransitioning()", "IsPauseTransitionPending()", "m_runtimeFailed.load", "m_processingWorkPending = true"]))
check("F-001 continuation preserves any Receive notification already pending during the capped pass", "Do not overwrite a Receive notification" in worker_fn and "m_processingWorkPending = true" in worker_fn)
check("F-001 follow-up pass is bounded and returns to input wait when no output is available", "kMaxFramesPerPass = 128" in process_fn and "if (!available)" in process_fn and "if (continuationScheduled)" in worker_fn and "condition-variable wait if the next pass finds no eligible output" in worker_fn)
check("F-001 policy suppresses continuation for terminal drain, failures, stale generations and transitions", all(x in processing_continuation_h for x in ["passLimitReached", "!terminalDrain", "!workerStop", "!workerSuspend", "generationCurrent", "!graphTransitioning", "!pauseTransitionPending", "!runtimeFailed", "processingSucceeded"]))
check("F-001 regression covers 128/129/256/512/1024 output bursts and bounded FIFO pressure", all(x in processing_continuation_test for x in ["{128, 2", "{129, 2", "{256, 3", "{512, 5", "{1024, 9", "producerWaits > 0", "highWater <= AsyncPipelineState::kDefaultOutputCapacity"]))
check("F-001 regression covers concurrent Receive notification, cancellation and no-busy-loop behavior", all(x in processing_continuation_test for x in ["1000 notification-versus-rearm races", "non-capped follow-up pass", "Stop racing with a capped pass", "terminal EOS drain keeps its existing uncapped drain path"]))
check("Windows build workflow compiles and executes the F-001 continuation regression", all(x in workflow for x in ["Run portable processing-worker continuation tests", "test_processing_continuation.cpp", "ProcessingContinuation.h", "PROCESSING_CONTINUATION_TEST_RUN_0.2.57_FIX3_7.txt"]))
check("Windows CI compiles and executes the portable C++17 stall classifier", all(x in workflow for x in ["Run portable pipeline stall classifier tests", "vcvars64.bat", "test_pipeline_stall_diagnostics.cpp", "PIPELINE_STALL_TEST_RUN_0.2.57.txt"]))


# Progressive post-seek fallback: bounded, non-blocking and PTS-ordered.
seek_policy = read("directmotion/SeekFallbackPolicy.h")
seek_policy_test = read("tools/test_seek_fallback_policy.cpp")
check("Seek fallback maps source intervals onto the target PTS lattice with boundary rounding", all(x in seek_policy for x in ["FrucRescaleRound(sourceFrameIndex", "sourceFrameIndex + 1", "targetFrameTimeBase"]))
check("Seek fallback policy separates source admission from visible interpolation readiness", all(x in seek_policy for x in ["ShouldQueueSourceFallback", "ShouldSealSourceFallback", "!explicitPreroll"]))
check("Seek fallback tests cover fractional up-rate, down-rate, gaps, preroll and watermark handoff", all(x in seek_policy_test for x in ["23.976-to-59.86", "30-to-60", "down-rate", "bootstrap preroll", "strictly after the last fallback PTS", "seals source fallback admission"]))
check("DM-11 keeps generated output hidden until a visible source boundary is known", all(x in seek_policy for x in ["ShouldTreatInterpolationAsPreroll", "!visibleSourceBoundaryKnown"]) and all(x in cpp for x in ["visibleSourceBoundaryKnown", "ShouldTreatInterpolationAsPreroll("]))
check("DM-11 first MVTools output during decoder preroll cannot seal source fallback", "timeline.visibleSourceBoundaryKnown =" in cpp and "ShouldTreatInterpolationAsPreroll(\n                interpolationIsPreroll, timeline.visibleSourceBoundaryKnown" in cpp)
check("DM-11 stream-time origin is rebased from the first timed source index on the output PTS lattice", all(x in seek_policy for x in ["TryComputeRebasedStreamTimeOrigin100ns", "TryComputeSourceFrameOutputPts", "frameOffset100ns"]) and all(x in cpp for x in ["inputStart, m_nextInputPts, m_sourceRate, m_outputTimeBase", "m_streamTimeOrigin = hasRebasedOrigin ? rebasedStreamTimeOrigin : inputStart", "Receive/TimelineOrigin"]))
check("Sample GetTime policy is a portable production header included by the Visual Studio project", "#include \"SampleTimePolicy.h\"" in cpp and "SampleTimePolicy.h" in read("directmotion/directmotion.vcxproj"))
check("Receive accepts VFW_S_NO_STOP_TIME as a valid start but not as a valid stop", "inputTimeHr = sample->GetTime(&inputStart, &inputEnd)" in cpp and "inputTimeHr, static_cast<HRESULT>(S_OK)," in cpp and "static_cast<HRESULT>(VFW_S_NO_STOP_TIME)" in cpp and "const bool hasInputTime = inputTime.hasStart;" in cpp and "const bool hasInputStopTime = inputTime.hasStop;" in cpp)
check("Runtime interpolation toggle also preserves a start-only GetTime result", "toggleTimeHr = sample->GetTime(&toggleStart, &toggleEnd)" in cpp and "toggleTime.hasStart" in cpp)
check("Seek trace records the exact GetTime HRESULT and independent start/stop availability", all(x in cpp for x in ["getTimeHr=", "hasStreamTime=", "hasStreamStopTime=", "inputStopTimeAvailable="]))
check("Continuity guard does not consume GetTime's synthetic start-plus-one stop sentinel", "PreviousSampleContinuityAnchor(" in cpp and "m_lastInputSampleEnd = hasInputStopTime ? inputEnd : inputStart;" in cpp and "m_lastInputSampleStopTimeValid = hasInputStopTime;" in cpp and "m_lastInputSampleStopTimeValid = false;" in cpp)
check("Previous-sample continuity anchor is reset with the engine generation", "m_lastInputSampleStopTimeValid = false;" in reset_state_block)
check("Portable GetTime regression covers S_OK, start-only success, no timestamps and continuity sentinel", all(x in sample_time_policy_test for x in ["VFW_S_NO_STOP_TIME preserves the valid sample start timestamp", "does not advertise a valid sample stop timestamp", "VFW_E_SAMPLE_TIME_NOT_SET", "synthetic start-plus-one stop sentinel"]))
check("Windows CI compiles and executes the GetTime policy regression", all(x in workflow for x in ["Run portable DirectShow sample GetTime policy tests", "test_sample_time_policy.cpp", "SAMPLE_TIME_POLICY_TEST_RUN_0.2.58.13.txt"]))
bootstrap_helper_start = cpp.find("bool CDirectMotion::EnsureEngineForBootstrapGeneration(")
bootstrap_helper_end = cpp.find("HRESULT CDirectMotion::DeliverDownstreamTracked(", bootstrap_helper_start)
bootstrap_helper = cpp[bootstrap_helper_start:bootstrap_helper_end] if bootstrap_helper_start >= 0 and bootstrap_helper_end > bootstrap_helper_start else ""
bootstrap_initialize_index = bootstrap_helper.find("candidate->Initialize(")
bootstrap_locks = [m.start() for m in re.finditer(r"CAutoLock receiveLock\(&m_csReceive\)", bootstrap_helper)]
check("DM-14.2 snapshots post-seek graph configuration under a short Receive lock, then initializes the candidate after releasing it", bool(bootstrap_helper) and len(bootstrap_locks) >= 2 and bootstrap_locks[0] < bootstrap_initialize_index < bootstrap_locks[1] and "outside Receive lock" in bootstrap_helper)
check("DM-14.2 publishes the candidate only after revalidating pipeline generation, staging generation, Receive lifecycle and deferred reset", all(x in bootstrap_helper for x in ["CanCommitCandidate(", "m_asyncPipeline.IsCurrentGeneration(expectedGeneration)", "m_sourceBootstrapQueue.IsActiveForGeneration(expectedGeneration)", "IsReceiveCommitCurrent(expectedGeneration)", "!m_seekResetState.deferredResetPending", "!m_engine", "candidate->IsInitialized()", "m_engine = std::move(candidate)"]))
check("DM-14.2 stale candidates are discarded without replacing a newer engine generation", all(x in bootstrap_helper for x in ["if (!committed)", "error.clear();", "return false;"]) and "m_engineGeneration.store(expectedGeneration" in bootstrap_helper)
dm14_source_worker_start = cpp.find("void CDirectMotion::SourceBootstrapWorkerMain()")
dm14_source_worker_end = cpp.find("// Called only while m_processingWorkerLifecycleMutex", dm14_source_worker_start)
dm14_source_worker_block = cpp[dm14_source_worker_start:dm14_source_worker_end] if dm14_source_worker_start >= 0 and dm14_source_worker_end > dm14_source_worker_start else ""
dm14_reset_start = cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset(")
dm14_reset_end = cpp.find("HRESULT CDirectMotion::StartStreaming()", dm14_reset_start)
dm14_reset_block = cpp[dm14_reset_start:dm14_reset_end] if dm14_reset_start >= 0 and dm14_reset_end > dm14_reset_start else ""
check("DM-14.2 SourceBootstrapWorker no longer calls synchronous EnsureEngine while holding m_csReceive", "EnsureEngineForBootstrapGeneration(staged.generation, error)" in dm14_source_worker_block and "EnsureEngine(error, staged.generation)" not in dm14_source_worker_block)
check("DM-14.2 candidate-build diagnostics expose build latency, commit-lock wait and staged backlog", all(x in bootstrap_helper for x in ["SourceBootstrap/EngineBuild", "candidateBuildMs=", "commitLockWaitMs=", "stagedFramesWaiting=", "stagingBytesOutstanding="]))
dm14_reset_lock_positions = [m.start() for m in re.finditer(r"CAutoLock receiveLock\(&m_csReceive\)", dm14_reset_block)]
dm14_retired_destroy_position = dm14_reset_block.find("retiredEngine->Reset()")
check("DM-14.2 retains ProcessingWorker quiescence, performs a Receive-lock preflight, detaches under a second short lock and destroys after unlocking before final revalidation", "SuspendProcessingWorkerForEngineReset()" in dm14_reset_block and "retiredEngine = std::move(m_engine)" in dm14_reset_block and len(dm14_reset_lock_positions) >= 3 and dm14_reset_lock_positions[0] < dm14_reset_lock_positions[1] < dm14_reset_block.find("retiredEngine = std::move(m_engine)") < dm14_retired_destroy_position < dm14_reset_lock_positions[2])
check("DM-14.2 finalizes deferred teardown only after checking the current generation and source queue again", all(x in dm14_reset_block for x in ["CanFinalizeRetiredEngine(", "m_asyncPipeline.IsCurrentGeneration(resetGeneration)", "expectedGeneration == 0 || m_sourceBootstrapQueue.IsActiveForGeneration(resetGeneration)", "IsReceiveCommitCurrent(resetGeneration)", "DirectMotionSeekReset::CanComplete(m_seekResetState, resetGeneration)", "m_sourceBootstrapWorkerStop", "!m_engine", "ResumeProcessingWorkerAfterEngineReset()"]))
check("DM-14.2 candidate-commit and teardown-finalization policies have portable regression coverage", "EngineBootstrapCommitPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in engine_bootstrap_commit_policy_test for x in ["current prepared graph may commit", "retired pipeline generation", "retired source-bootstrap generation", "Receive lifecycle transition", "deferred old-engine reset", "engine installed by another path", "partially initialized candidate", "detached-engine teardown may finalize", "active Receive lifecycle transition prevents deferred-reset finalization", "stopped source-bootstrap worker cannot resume processing"]))
check("Windows CI compiles and executes the DM-14.2 engine candidate commit policy regression", all(x in workflow for x in ["Run post-seek engine candidate commit policy tests", "test_engine_bootstrap_commit_policy.cpp", "ENGINE_BOOTSTRAP_COMMIT_POLICY_TEST_RUN_0.2.58.14.txt"]))
check("DM-11 visible preroll boundary uses the same source-index-to-output-PTS mapper as origin rebase", "TryComputeSourceFrameOutputPts(\n                    m_nextInputPts, m_sourceRate, m_outputTimeBase, boundaryPts)" in cpp)
check("DM-11 policy tests cover unresolved boundary, PTS rebase, index zero and invalid/overflow inputs", all(x in seek_policy_test for x in ["until the first non-preroll source boundary is known", "first timestamped frame after untimed preroll", "source frame zero preserves", "negative source index", "invalid source rate", "invalid output time base", "signed underflow"]))
check("Fallback packets use a separate generation-bound priority lane and the same single output worker", "TryEnqueueOutput(std::move(packet))" in cpp and "GenerationPriorityOutputQueue" in async_state_cpp and "m_fallbackItems" in async_state_cpp and "kDefaultOutputCapacity = 16" in async_state_h and "kDefaultFallbackOutputCapacity = 1024" in async_state_h and "std::thread(&CDirectMotion::DeliveryWorkerMain, this)" in cpp and "m_asyncPipeline.WaitDequeueOutput(generation, packet)" in cpp and "ShouldTakeFallbackLocked()" in async_state_cpp)
receive_body = cpp[cpp.find("HRESULT CDirectMotion::Receive(IMediaSample* sample)"):]
fallback_admission_index = receive_body.find("QueueSeekFallbackForDelivery(normalizedInput")
ensure_engine_index = receive_body.find("const bool engineReady = EnsureEngine(error, deliveryGeneration)")
source_publish_index = receive_body.find("m_engine->PushSourceNv12(")
processing_notify_index = receive_body.find("NotifyProcessingWorker();", fallback_admission_index)
receive_fallback_start = receive_body.find("const int64_t publishedSourceFrameIndex = m_nextInputPts;")
receive_fallback_end = receive_body.find("DirectMotionTraySetWorkPhase(DirectMotionWorkPhase::Backend", receive_fallback_start)
receive_fallback_block = receive_body[receive_fallback_start:receive_fallback_end] if receive_fallback_start >= 0 and receive_fallback_end > receive_fallback_start else ""
fallback_gate_index = receive_fallback_block.find("if (fallbackWasActive)")
fallback_clock_index = receive_fallback_block.find("const auto fallbackEnqueueStarted")
fallback_helper_index = receive_fallback_block.find("fallbackQueueHr = sharedSourcePayload")
fallback_gate_end = receive_fallback_block.find("\n    }\n    if (!isCurrentOrInFlightInitialStage())", fallback_gate_index)
deferred_complete_index = receive_fallback_block.find("if (m_seekResetState.deferredResetPending && !CompleteDeferredSeekEngineReset()) return E_ABORT;")
check("DM-03 Receive gates source fallback helper on the acquire-published active mirror", "fallbackWasActive =\n        m_seekFallbackActivePublished.load(std::memory_order_acquire)" in receive_fallback_block and fallback_gate_index >= 0 and fallback_helper_index > fallback_gate_index and fallback_gate_end > fallback_helper_index)
check("DM-03 inactive Receive path skips fallback helper, fallback mutex path and packet admission work", fallback_helper_index > fallback_gate_index and "fallbackQueueHr = sharedSourcePayload" not in receive_fallback_block[:fallback_gate_index])
check("DM-03 fallback enqueue clock is inside active gate and enabled only for FullTrace", fallback_clock_index > fallback_gate_index and "const bool traceFallbackAdmission = DirectShowFullTraceEnabled();" in receive_fallback_block and re.search(r"traceFallbackAdmission\s*\?", receive_fallback_block) is not None and receive_fallback_block.find("std::chrono::steady_clock::now()", fallback_gate_index) > fallback_clock_index)
check("DM-03 deferred engine reset remains outside fallback gate and before replacement engine initialization", fallback_gate_end >= 0 and deferred_complete_index > fallback_gate_end and ensure_engine_index > 0 and "CompleteDeferredSeekEngineReset()" in receive_fallback_block[deferred_complete_index:])
check("DM-03 active path retains the helper's mutex-authoritative state recheck", "ShouldQueueSourceFallback(\n                    m_seekFallbackActive, m_seekFallbackAcceptingSource)" in cpp[cpp.find("HRESULT CDirectMotion::QueueSeekFallbackForDelivery("):cpp.find("void CDirectMotion::ArmSeekFallback")])
check("BeginFlush requests deferred engine teardown with its captured generation token without waiting for the old GetFrame pass", "BeginDeferredSeekEngineReset(nextDeliveryGeneration, true)" in cpp[cpp.find("HRESULT CDirectMotion::BeginFlush()") : cpp.find("HRESULT CDirectMotion::EndFlush()") ] and "SuspendProcessingWorkerForEngineReset()" not in cpp[cpp.find("bool CDirectMotion::BeginDeferredSeekEngineReset(") : cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset(")])
check("The first fallback FIFO admission precedes deferred worker quiescence and graph destruction", cpp.find("QueueSeekFallbackForDelivery(normalizedInput") < cpp.find("CompleteDeferredSeekEngineReset()", cpp.find("QueueSeekFallbackForDelivery(normalizedInput")) < cpp.find("EnsureEngine(error, deliveryGeneration)", cpp.find("QueueSeekFallbackForDelivery(normalizedInput")))
deferred_reset_block = cpp[cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset("):cpp.find("HRESULT CDirectMotion::StartStreaming()", cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset("))]
check("Deferred teardown preserves the engine until fallback admission, destroys it outside Receive lock and resumes only after generation revalidation", "engineRetained=1" in cpp and "detaching the retired AviSynth/MVTools graph" in deferred_reset_block and deferred_reset_block.find("retiredEngine = std::move(m_engine)") < deferred_reset_block.find("retiredEngine->Reset()") < deferred_reset_block.find("CanFinalizeRetiredEngine(") < deferred_reset_block.find("ResumeProcessingWorkerAfterEngineReset()") and "outside m_csReceive" in deferred_reset_block)
check("First seek fallback is enqueued before EnsureEngine, InputClip publication and processing notification",
      fallback_admission_index >= 0 and ensure_engine_index > fallback_admission_index and
      source_publish_index > ensure_engine_index and processing_notify_index > source_publish_index)
check("Seek fallback records one compact arm-to-first-downstream-Deliver latency per seek",
      all(x in cpp for x in ["m_seekFallbackArmedTickNs", "seekFallbackFirstPacket", "SeekFallback/FirstFrameDelivered", "seekToDownstreamMs"]))
check("Post-seek trace records first decoder input and first visible input exactly once", all(x in cpp for x in ["SeekTrace/FirstReceive", "SeekTrace/FirstVisibleReceive", "m_seekTraceFirstReceiveLogged", "m_seekTraceFirstVisibleLogged", "precedingPrerollSamples", "armToVisibleReceiveMs"]))
check("Seek trace samples upstream media time only on the first visible post-seek frame", "seekTraceFirstVisibleSample" in cpp and "DirectShowFullTraceEnabled() || seekTraceFirstVisibleSample" in cpp)
check("Seek trace one-shot markers are consumed only after a valid monotonic timestamp is available", "firstReceivePending && traceNowNs != 0 &&" in cpp and "firstVisiblePending && traceNowNs != 0 &&" in cpp)
check("Seek fallback records MVTools warm-up latency and output timestamp context", all(x in cpp for x in ["armToFallbackQueueMs", "armToWarmupMs", "streamOrigin100ns", "outputTimeBase", "segmentStart100ns"]))
seek_trace_script = read("tools/extract_seek_trace.ps1")
check("Compact PowerShell extractor selects only seek, fallback and slow-delivery events", all(x in seek_trace_script for x in ["NewSegment", "SeekTrace", "FirstVisibleReceive", "FirstFrameDelivered", "DeferredSeekReset", "Slow GetDeliveryBuffer", "MaxLines = 40"]))
check("Seek fallback skips decoder preroll and rechecks state after payload construction", "if (sourcePreroll) return S_FALSE;" in cpp and "ShouldQueueSourceFallback(\n                    m_seekFallbackActive, m_seekFallbackAcceptingSource)" in cpp)
check("Warm-up fallback seals on first visible interpolated output and hands off after queued PTS watermark", all(x in cpp for x in ["SeekFallback/WarmupReady", "SeekFallback/Handoff", "firstInterpolatedPts", "m_seekFallbackLastQueuedPts + 1"]))
queue_fallback_start = cpp.find("HRESULT CDirectMotion::QueueInterpolatedForDelivery(")
queue_fallback_end = cpp.find("HRESULT CDirectMotion::QueueSeekFallbackForDelivery(", queue_fallback_start)
queue_fallback_block = cpp[queue_fallback_start:queue_fallback_end] if queue_fallback_start >= 0 and queue_fallback_end > queue_fallback_start else ""
active_writes = re.findall(r"m_seekFallbackActive\s*=\s*(true|false);", cpp)
active_publications = re.findall(r"m_seekFallbackActivePublished\.store\((true|false),\s*std::memory_order_release\);", cpp)
check("Fallback active mirror is atomic and synchronized with all three mutex-owned state transitions", active_writes == ["false", "true", "false"] and active_publications == ["false", "true", "false"] and "std::atomic<bool> m_seekFallbackActivePublished{ false };" in hdr)
check("Ordinary output skips fallback classification and fallback mutex when the published state is inactive", "m_seekFallbackActivePublished.load(std::memory_order_acquire)" in queue_fallback_block and queue_fallback_block.count("if (fallbackActivePublished)") == 2 and "std::lock_guard<std::mutex> fallbackLock(m_seekFallbackMutex);" in queue_fallback_block)
check("Output packet metadata and fallback preroll classification share one coherent timeline snapshot", queue_fallback_block.count("timelineLock(m_outputTimelineMutex)") == 1 and all(x in queue_fallback_block for x in ["timeline.streamTimeOrigin", "timeline.outputTimeBase", "packet.streamTimeOrigin100ns = timeline.streamTimeOrigin", "packet.segmentStart100ns = timeline.segmentStart"]))
check("Seek fallback reuses one shared NV12 image for repeat-count target slots", all(x in cpp for x in ["packet.repeatCount", "packet.seekFallback = true", "packet.sharedNv12 = sharedNv12", "DeliverInterpolated(packet, outputPts)"]))
check("Seek fallback arm/clear follows NewSegment, BeginFlush, StartStreaming and StopStreaming", all(x in cpp for x in ["ArmSeekFallback(\"NewSegment/seek\", nextDeliveryGeneration)", "ClearSeekFallback();", "ResetSeekFallbackProgress(expectedFallbackGeneration);"]))
check("No fixed 500ms sleep or second delivery pipeline is introduced", "sleep_for(std::chrono::milliseconds(500))" not in cpp and "StartSeekFallbackDeliveryWorker" not in cpp and "StartFallbackWorker" not in cpp)
check("Seek fallback and Receive commit policy headers are included in DirectMotion and Visual Studio project inventory", '#include "SeekFallbackPolicy.h"' in hdr and "SeekFallbackPolicy.h" in read("directmotion/directmotion.vcxproj") and '#include "ReceiveCommitPolicy.h"' in hdr and "ReceiveCommitPolicy.h" in read("directmotion/directmotion.vcxproj"))
check("Windows CI compiles and retains the Receive generation commit-gate regression", all(x in workflow for x in ["Run portable Receive generation commit-gate tests", "test_receive_generation_policy.cpp", "ReceiveCommitPolicy.h", "RECEIVE_GENERATION_COMMIT_TEST_RUN_0.2.58.8.txt"]))
check("Windows CI compiles and runs the strict MAnalyse indexed-access/single-scheduler regression", all(x in workflow for x in ["Run portable MAnalyse single-scheduler policy tests", "test_manalyse_invocation_policy.cpp", "MAnalyseInvocationPolicy.h", "MANALYSE_SCHEDULER_POLICY_TEST_RUN_0.2.58.25.txt"]))
check("Windows CI compiles and retains the progressive seek fallback and DM-11 readiness/PTS policy tests", all(x in workflow for x in ["Run portable progressive seek fallback policy tests", "test_seek_fallback_policy.cpp", "SEEK_FALLBACK_POLICY_TEST_RUN_0.2.58.10.txt"]))
check("Windows CI compiles and runs the bounded DM-12/DM-13 source-bootstrap queue regression", all(x in workflow for x in ["Run bounded decoupled source-bootstrap queue tests", "test_source_bootstrap_queue.cpp", "SourceBootstrapQueue.h", "SOURCE_BOOTSTRAP_QUEUE_TEST_RUN_0.2.58.12.txt"]))
check("Windows CI compiles and retains the bounded fallback output-FIFO regression", all(x in workflow for x in ["Run bounded asynchronous output FIFO tests", "test_async_pipeline_state.cpp", "ASYNC_PIPELINE_STATE_TEST_RUN_0.2.58.txt"]))
check("Async pipeline regression verifies separate lane capacity, PTS tie priority and shared payload lifetime", all(x in async_state_test for x in ["TryEnqueueOutput(std::move(fallbackPacket))", "fallback lane admits frames up to its own independent capacity", "fallback lane applies its own bounded non-blocking capacity", "PTS merge gives source fallback priority on a tie", "queuedFallback.sharedNv12 == expectedFallbackPayload", "single delivery consumer merges lanes by PTS"]))
check("0.2.58.28 CI records validation under the current source-generation-rebinding filename in RUNNER_TEMP", "VALIDATION_RUN_0.2.58.28.txt" in workflow and "$validationLog = Join-Path $env:RUNNER_TEMP" in workflow and "Tee-Object -FilePath $validationLog" in workflow)
check("Windows CI sends all 29 portable test executables and stdout logs to RUNNER_TEMP", workflow.count("$testExe = Join-Path $env:RUNNER_TEMP") == 29 and workflow.count("$testLog = Join-Path $env:RUNNER_TEMP") == 29 and workflow.count('cd /d "') == 29 and workflow.count("cmd.exe /d /s /c $command") == 29)
check("Windows CI compiles and executes the BootstrapQueueState regression", all(x in workflow for x in ["Run bootstrap queue state regression tests", "test_bootstrap_queue_state.cpp", "directmotion\\AsyncPipelineState.cpp", "BOOTSTRAP_QUEUE_STATE_TEST_RUN.txt"]))
check("Windows CI compiles and executes the delivery-latency diagnostics regression", all(x in workflow for x in ["Run delivery latency diagnostics regression tests", "test_delivery_latency_diagnostics.cpp", "DELIVERY_LATENCY_DIAGNOSTICS_TEST_RUN.txt"]))

check("DM-23.1 seek-frontier policy is included in Visual Studio and Windows CI tests", "SeekFrontierPolicy.h" in read("directmotion/directmotion.vcxproj") and all(x in workflow for x in ["Run seek-frontier bootstrap policy tests", "test_bootstrap_evaluation_policy.cpp", "SeekFrontierPolicy.h", "SEEK_FRONTIER_BOOTSTRAP_TEST_RUN_0.2.58.25.txt"]))
check("DM-13 shared source staging/fallback budget is 768 MiB and charges the shared vector allocation until its last owner releases it", all(x in source_bootstrap_queue_h for x in ["kDefaultByteBudget = 768u * 1024u * 1024u", "const size_t bytes = source.capacity()", "m_budget->bytesOutstanding += bytes", "std::shared_ptr<const std::vector<uint8_t>>", "budget->bytesOutstanding - bytes", "frame.chargedBytes = payload->capacity()", "admissionEpoch", "capturedEpoch != m_budget->admissionEpoch", "SetFallbackAdmissionActive", "m_budget->admissionActive = active || m_active"]))
check("DM-13 exposes source-fallback lane pressure and shared NV12 budget in tray and periodic telemetry", all(x in cpp for x in ["Source lane %zu/%zu", "outputQueue.fallbackHighWater", "outputQueue.fallbackDiscarded", "sourceFallbackQueue=", "sourceFallbackFullRejects=", "sourceSharedBytesOutstanding=", "sourceSharedByteBudget="]))
check("DM-13 seals shared fallback admission atomically at PTS handoff while preserving active staging budget", "m_sourceBootstrapQueue.SetFallbackAdmissionActive(false);" in cpp[cpp.find("InterpolationDecision::HandoffToInterpolation"):cpp.find("if (fallbackWarmupReady)")] and "m_sourceBootstrapQueue.SetFallbackAdmissionActive(false);" in cpp[cpp.find("void CDirectMotion::ClearSeekFallback("):cpp.find("void CDirectMotion::ResetSeekFallbackProgress(")])
check("DM-12 queue tests cover admission/backpressure, in-flight accounting, generation cancellation, FIFO order and oversized payload ownership", all(x in source_bootstrap_queue_test for x in ["oversized frame is rejected instead of waiting forever", "in-flight frame remains charged", "admission resumes after frame publication completes", "frame completion is idempotent for an RAII cleanup path", "prior generation rejected", "stale in-flight old generation remains charged", "completion of stale in-flight frame releases its own global budget charge", "FIFO preserves source frame ordering", "generation change cancels old admission waiter", "rejected rvalue payload remains owned by Receive caller"]))
check("DM-12 queue tests cover admission/backpressure, in-flight accounting, generation cancellation, FIFO order and oversized payload ownership", all(x in source_bootstrap_queue_test for x in ["oversized frame is rejected instead of waiting forever", "in-flight frame remains charged", "admission resumes after frame publication completes", "frame completion is idempotent for an RAII cleanup path", "prior generation rejected", "stale in-flight old generation remains charged", "completion of stale in-flight frame releases its own global budget charge", "FIFO preserves source frame ordering", "generation change cancels old admission waiter", "rejected rvalue payload remains owned by Receive caller"]))
source_worker_start = cpp.find("void CDirectMotion::SourceBootstrapWorkerMain()")
source_worker_end = cpp.find("// Called only while m_processingWorkerLifecycleMutex", source_worker_start)
source_worker_block = cpp[source_worker_start:source_worker_end] if source_worker_start >= 0 and source_worker_end > source_worker_start else ""
dm12_reset_start = cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset(")
dm12_reset_end = cpp.find("HRESULT CDirectMotion::StartStreaming()", dm12_reset_start)
dm12_reset_block = cpp[dm12_reset_start:dm12_reset_end] if dm12_reset_start >= 0 and dm12_reset_end > dm12_reset_start else ""
receive_stage_start = receive_body.find("m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration)")
receive_stage_end = receive_body.find("if (m_seekResetState.deferredResetPending", receive_stage_start)
receive_stage_block = receive_body[receive_stage_start:receive_stage_end] if receive_stage_start >= 0 and receive_stage_end > receive_stage_start else ""
check("DM-12 Receive transfers normalized NV12 ownership to staging and returns before deferred teardown/EnsureEngine", receive_stage_block.find("TryPush(deliveryGeneration") >= 0 and receive_stage_block.find("return S_OK") >= 0 and receive_stage_block.find("CompleteDeferredSeekEngineReset") < 0 and receive_body.find("m_sourceBootstrapQueue.IsActiveForGeneration(deliveryGeneration)") < receive_body.find("CompleteDeferredSeekEngineReset()"))
check("DM-12 bootstrap worker preserves staged source order, releases every popped frame through RAII and rechecks generation before graph/source commits", all(x in source_worker_block for x in ["WaitPop(staged", "StagedFrameLease", "~StagedFrameLease() noexcept", "IsCurrent(staged)", "IsReceiveCommitCurrent(staged.generation)", "CompleteDeferredSeekEngineReset(staged.generation)", "EnsureEngineForBootstrapGeneration(staged.generation, error)", "PushSourceNv12(", "staged.sourceFrameIndex", "TryCompleteGeneration(staged.generation)"]))
preflight_lock = dm12_reset_block.find("CAutoLock receiveLock(&m_csReceive)")
phase1_lock = dm12_reset_block.find("std::unique_lock<std::mutex> processingLifecycleFence")
detach_lock = dm12_reset_block.find("CAutoLock receiveLock(&m_csReceive)", preflight_lock + 1)
check("DM-12 reset phases preflight under Receive lock, release it before the processing lifecycle fence, detach under a second short Receive lock and destroy outside both", preflight_lock >= 0 and phase1_lock > preflight_lock and detach_lock > phase1_lock and "Phase 1 owns only the processing-worker lifecycle mutex" in dm12_reset_block and "Phase 2 briefly owns m_csReceive only to detach the retired engine" in dm12_reset_block and dm12_reset_block.find("retiredEngine = std::move(m_engine)") < dm12_reset_block.find("retiredEngine->Reset()") < dm12_reset_block.find("CanFinalizeRetiredEngine("))
check("DM-12/13 staging and fallback budget backpressure is awaited before m_csReceive and rechecked after sample properties refresh", all(x in base_transfrm_cpp for x in ["WaitForReceiveAdmission(pSample)", "CAutoLock receiveLock(&m_pTransformFilter->m_csReceive)", "ShouldWaitForReceiveAdmission(pSample)", "receiveLock.Unlock()" ]) and "m_sourceBootstrapQueue.WaitForAdmission" in cpp and "m_sourceBootstrapQueue.ShouldBackpressure" in cpp)
check("DM-12 queue is retired on ResetEngine/BeginFlush/NewSegment and the worker is stopped on streaming stop and destruction", all(x in cpp for x in ["m_sourceBootstrapQueue.Cancel();", "StopSourceBootstrapWorker(\"DirectShow StopStreaming\")", "StopSourceBootstrapWorker(\"filter destruction\")", "m_sourceBootstrapQueue.BeginGeneration(expectedGeneration, !awaitNewSegment)", "BeginDeferredSeekEngineReset(nextDeliveryGeneration, false)"]))
check("DM-12 queue header is included in the Visual Studio project inventory", "SourceBootstrapQueue.h" in read("directmotion/directmotion.vcxproj"))
deferred_begin_start = cpp.find("bool CDirectMotion::BeginDeferredSeekEngineReset(")
deferred_begin_end = cpp.find("bool CDirectMotion::CompleteDeferredSeekEngineReset(", deferred_begin_start)
deferred_complete_end = cpp.find("HRESULT CDirectMotion::StartStreaming()", deferred_begin_end)
deferred_begin_block = cpp[deferred_begin_start:deferred_begin_end] if deferred_begin_start >= 0 and deferred_begin_end > deferred_begin_start else ""
deferred_complete_block = cpp[deferred_begin_end:deferred_complete_end] if deferred_begin_end >= 0 and deferred_complete_end > deferred_begin_end else ""
begin_flush_block = cpp[cpp.find("HRESULT CDirectMotion::BeginFlush()"):cpp.find("HRESULT CDirectMotion::EndFlush()")]
check("DM-16 every deferred-reset caller passes its own generation token", all(x in cpp for x in [
    "BeginDeferredSeekEngineReset(newGeneration",
    "BeginDeferredSeekEngineReset(nextDeliveryGeneration",
    "BeginDeferredSeekEngineReset(resetGeneration"]))
check("DM-16 BeginDeferredSeekEngineReset rejects stale tokens before and after worker suspension", "currentGeneration != expectedGeneration" in deferred_begin_block and "ResetEngineGenerationState(true, expectedGeneration)" in deferred_begin_block)
check("DM-16 deferred reset validates its generation before changing warm-start or worker state", deferred_begin_block.find("currentGeneration != expectedGeneration") < deferred_begin_block.find("m_resumeWarmStartPending.store(false") < deferred_begin_block.find("m_processingWorkerSuspend = true"))
check("DM-16 deferred reset keeps generation fence across suspension, fallback reset and staging publication", all(x in deferred_begin_block for x in ["std::lock_guard<std::recursive_mutex> deliveryFence(m_deliveryMutex)", "m_engineGeneration.store(0", "ResetEngineGenerationState(true, expectedGeneration)", "m_sourceBootstrapQueue.BeginGeneration(expectedGeneration, !awaitNewSegment)"]))
check("DM-16 synchronous engine reset commits timeline state only while its captured generation remains current", "if (m_asyncPipeline.CurrentGeneration() == resetGeneration)" in cpp[cpp.find("void CDirectMotion::ResetEngine(bool prepareResumeWarmStart)"):cpp.find("bool CDirectMotion::BeginDeferredSeekEngineReset(")])
check("DM-16 fallback generation policy test covers stale reset-token rejection", "delayed reset carrying an old token cannot reset a replacement fallback" in seek_fallback_generation_test)
check("FIX-2.2 begins deferred seek reset by suspending ProcessingWorker and invalidating its engine generation", all(x in deferred_begin_block for x in ["AsyncPipelineState::Generation& expectedGeneration", "m_processingWorkerSuspend = true", "InterruptPendingSourceRequests(\"Seek/DeferredEngineReset\")", "m_engineGeneration.store(0", "DirectMotionSeekReset::BeginDeferredReset(", "ResetEngineGenerationState(true, expectedGeneration)", "currentGeneration != expectedGeneration"]))
check("FIX-2.2 request phase never waits for worker quiescence or destroys the retained engine", "SuspendProcessingWorkerForEngineReset" not in deferred_begin_block and "m_engine->Reset()" not in deferred_begin_block and "m_engine.reset()" not in deferred_begin_block and "engineRetained=1" in deferred_begin_block)
check("FIX-2.2 completion proves quiescence before detaching/destroying the old engine and resumes only after generation revalidation", deferred_complete_block.find("SuspendProcessingWorkerForEngineReset()") >= 0 and deferred_complete_block.find("SuspendProcessingWorkerForEngineReset()") < deferred_complete_block.find("retiredEngine = std::move(m_engine)") < deferred_complete_block.find("retiredEngine->Reset()") < deferred_complete_block.find("ResumeProcessingWorkerAfterEngineReset()"))
check("FIX-2.2 BeginFlush defers seek teardown with the exact generation token and preserves synchronous non-seek lifecycle reset", "BeginDeferredSeekEngineReset(nextDeliveryGeneration, true)" in begin_flush_block and "m_currentSegmentMetadataValid" in begin_flush_block and "ResetEngine();" in begin_flush_block)
check("FIX-2.2 Receive admits the fallback before the deferred quiescence barrier and new engine construction", receive_body.find("QueueSeekFallbackForDelivery(normalizedInput") >= 0 and receive_body.find("QueueSeekFallbackForDelivery(normalizedInput") < receive_body.find("CompleteDeferredSeekEngineReset()") < receive_body.find("EnsureEngine(error, deliveryGeneration)") and "m_engine->PushSourceNv12(" in receive_body)
check("FIX-2.2 processing-worker lifetime and old-engine retention use the existing quiescence barrier", "The old engine remains alive until CompleteDeferredSeekEngineReset" in deferred_begin_block and "engine remains retained" in deferred_complete_block and "ProcessingWorker resumed for the replacement engine" in deferred_complete_block)
check("Windows CI compiles and runs the processing-worker quiescence regression for FIX-2.2", all(x in workflow for x in ["Run deferred seek engine-quiescence lifecycle tests", "test_processing_quiescence.cpp", "PROCESSING_QUIESCENCE_TEST_RUN_0.2.58_FIX2_2.txt"]))

# Basic source/project structure checks.
cpp_files = list((ROOT / "directmotion").glob("*.cpp")) + list((ROOT / "directmotion").glob("*.h")) + [
    ROOT / "common" / "baseclasses" / "transfrm.cpp",
    ROOT / "common" / "baseclasses" / "transfrm.h",
    ROOT / "common" / "baseclasses" / "wxutil.h",
]
check("Windows CI parses the compact seek-trace PowerShell extractor", "Parse compact seek trace extraction script" in workflow and "Parser]::ParseFile" in workflow and "extract_seek_trace.ps1" in workflow)
check("C++ delimiters balanced", all(balanced_cpp(p.read_text(encoding="utf-8", errors="replace")) for p in cpp_files), f"{len(cpp_files)} C++ files checked")
project_files = list((ROOT / "directmotion").glob("*.vcxproj")) + list((ROOT / "directmotion").glob("*.vcxproj.filters"))
xml_ok = True
for path in project_files:
    try: ET.parse(path)
    except ET.ParseError: xml_ok = False
check("Visual Studio project XML parses", xml_ok, f"{len(project_files)} project files")
requirements_path = ROOT / "tools" / "requirements-validation.txt"
requirements_text = requirements_path.read_text(encoding="utf-8") if requirements_path.exists() else ""
install_step = workflow.find("- name: Install Python validation dependencies")
validation_step = workflow.find("- name: Run source and scheduler validation")
workflow_installs_yaml_dependency = (
    "pip install --disable-pip-version-check -r .\\tools\\requirements-validation.txt" in workflow
    and install_step >= 0
    and validation_step > install_step
)
check(
    "CI installs pinned Python validation dependencies before running validation",
    "PyYAML==6.0.3" in requirements_text and workflow_installs_yaml_dependency,
    "tools/requirements-validation.txt",
)
check(
    "CI pins the Python setup action to an immutable full commit SHA",
    "actions/setup-python@6d4fd22b20465ccc0f7354476588e8df7c9ca0e0 # v7.1.0" in workflow,
    ".github/workflows/build-directmotion.yml",
)
setup_python_step = workflow.find("- name: Configure Python for workflow validation")
python_install_position = workflow.find("- name: Install Python validation dependencies")
check(
    "CI selects exact CPython 3.14.10 before installing validation dependencies",
    setup_python_step >= 0
    and python_install_position > setup_python_step
    and "python-version: '3.14.10'" in workflow
    and "check-latest: false" in workflow,
    ".github/workflows/build-directmotion.yml",
)
check(
    "CI records the Python and PyYAML runtime versions used by validation",
    "python --version" in workflow
    and "Python version check failed" in workflow
    and "print('Python runtime:', sys.version); print('PyYAML runtime:', yaml.__version__)" in workflow
    and "Validation runtime identification failed" in workflow,
    ".github/workflows/build-directmotion.yml",
)
if yaml is not None:
    try: yaml.safe_load(workflow); yaml_ok = True
    except Exception: yaml_ok = False
else:
    yaml_ok = False
check("GitHub Actions workflow YAML parses", yaml_ok, "PyYAML" if yaml is not None else "PyYAML unavailable")

failed = [item for item in checks if not item[1]]
for name, ok, detail in checks:
    print(f"{'PASS' if ok else 'FAIL'} | {name}" + (f" | {detail}" if detail else ""))
print(f"\nRESULT: {len(checks)-len(failed)}/{len(checks)} checks passed")
if failed:
    sys.exit(1)
