#include "../directmotion/DeliveryLatencyDiagnostics.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using directmotion_diagnostics::ShouldWarnSlowOutputStage;
using directmotion_diagnostics::kSlowOutputStageWarningMs;

int main() {
    struct TestCase { const char* name; double elapsedMs; bool expected; };
    const std::vector<TestCase> tests = {
        {"zero duration is not slow", 0.0, false},
        {"negative duration is not slow", -1.0, false},
        {"just below threshold is not slow", kSlowOutputStageWarningMs - 0.001, false},
        {"exact threshold is slow", kSlowOutputStageWarningMs, true},
        {"ordinary slow call is reported", 40.0, true},
        {"severely slow call is reported", 1500.0, true},
        {"NaN is not reported as slow", std::numeric_limits<double>::quiet_NaN(), false},
        {"positive infinity is reported as slow", std::numeric_limits<double>::infinity(), true},
    };

    unsigned failed = 0;
    for (const auto& test : tests) {
        const bool actual = ShouldWarnSlowOutputStage(test.elapsedMs);
        const bool ok = actual == test.expected;
        std::cout << (ok ? "PASS" : "FAIL") << " | " << test.name
                  << " | actual=" << (actual ? 1 : 0)
                  << " | expected=" << (test.expected ? 1 : 0) << '\n';
        if (!ok) ++failed;
    }
    std::cout << "\nRESULT: " << (tests.size() - failed) << '/' << tests.size()
              << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
