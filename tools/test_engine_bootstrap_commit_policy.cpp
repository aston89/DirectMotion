#include "../directmotion/EngineBootstrapCommitPolicy.h"

#include <iostream>

namespace {
int failures = 0;
int checks = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS | " << description << '\n';
    } else {
        ++failures;
        std::cout << "FAIL | " << description << '\n';
    }
}
}

int main() {
    using DirectMotionEngineBootstrapCommit::CanCommitCandidate;

    Check(CanCommitCandidate(true, true, true, true, true, true),
        "current prepared graph may commit after deferred reset");
    Check(!CanCommitCandidate(false, true, true, true, true, true),
        "retired pipeline generation cannot commit a prepared graph");
    Check(!CanCommitCandidate(true, false, true, true, true, true),
        "retired source-bootstrap generation cannot commit a prepared graph");
    Check(!CanCommitCandidate(true, true, false, true, true, true),
        "Receive lifecycle transition rejects a prepared graph");
    Check(!CanCommitCandidate(true, true, true, false, true, true),
        "graph cannot commit before deferred old-engine reset completes");
    Check(!CanCommitCandidate(true, true, true, true, false, true),
        "candidate cannot overwrite an engine installed by another path");
    Check(!CanCommitCandidate(true, true, true, true, true, false),
        "partially initialized candidate is never published");
    Check(!CanCommitCandidate(true, false, false, false, false, false),
        "multiple simultaneous retirement conditions fail closed");

    using DirectMotionEngineBootstrapCommit::CanFinalizeRetiredEngine;
    Check(CanFinalizeRetiredEngine(true, true, true, true, true, true),
        "current detached-engine teardown may finalize and resume the worker");
    Check(!CanFinalizeRetiredEngine(false, true, true, true, true, true),
        "superseded pipeline generation cannot finalize an older teardown");
    Check(!CanFinalizeRetiredEngine(true, false, true, true, true, true),
        "retired source-bootstrap generation cannot finalize an older teardown");
    Check(!CanFinalizeRetiredEngine(true, true, false, true, true, true),
        "active Receive lifecycle transition prevents deferred-reset finalization");
    Check(!CanFinalizeRetiredEngine(true, true, true, false, true, true),
        "superseded deferred reset cannot be cleared by the older worker pass");
    Check(!CanFinalizeRetiredEngine(true, true, true, true, false, true),
        "stopped source-bootstrap worker cannot resume processing");
    Check(!CanFinalizeRetiredEngine(true, true, true, true, true, false),
        "deferred reset cannot finalize if another engine occupies the slot");
    Check(!CanFinalizeRetiredEngine(false, false, false, false, false, false),
        "multiple post-teardown retirement conditions fail closed");

    std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << checks << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
