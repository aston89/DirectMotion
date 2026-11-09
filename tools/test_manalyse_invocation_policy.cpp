#include "../directmotion/MAnalyseInvocationPolicy.h"

#include <cstring>
#include <iostream>

namespace {
int checks = 0;
int failures = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    } else {
        std::cout << "PASS: " << description << '\n';
    }
}

bool NameEquals(const char* actual, const char* expected) {
    if (actual == nullptr || expected == nullptr) {
        return actual == expected;
    }
    return std::strcmp(actual, expected) == 0;
}

void CheckNames(const DirectMotionAviSynth::MAnalyseInvocationContract& contract,
                const char* const* expected, std::size_t expectedCount,
                const char* description) {
    Check(contract.argumentCount == expectedCount, description);
    if (contract.argumentCount != expectedCount) {
        return;
    }
    for (std::size_t i = 0; i < expectedCount; ++i) {
        Check(NameEquals(contract.argumentNames[i], expected[i]),
              "required MAnalyse argument position matches the supported signature");
    }
    Check(NameEquals(contract.argumentNames[expectedCount - 2], "mt"),
          "mt remains explicitly named before the temporal predictor override");
    Check(NameEquals(contract.argumentNames[expectedCount - 1], "temporal"),
          "temporal access policy is explicitly named after mt");
}
} // namespace

int main() {
    using namespace DirectMotionAviSynth;

    Check(kMAnalyseInternalThreadingEnabled == false,
          "MVTools internal threading is disabled by the shared production policy");
    Check(kMAnalyseTemporalPredictorsEnabled == false,
          "linear temporal predictors are disabled by the shared seek-frontier policy");

    const char* standardExpected[] = {
        nullptr, "chroma", "isb", "blksize", "blksizev",
        "searchparam", "plevel", "search", "badrange", "mt", "temporal"
    };
    const char* advancedExpected[] = {
        nullptr, "chroma", "isb", "blksize", "blksizev", "searchparam",
        "plevel", "search", "badrange", "overlap", "truemotion", "mt", "temporal"
    };

    const auto& standard = GetMAnalyseInvocationContract(false);
    const auto& advanced = GetMAnalyseInvocationContract(true);

    CheckNames(standard, standardExpected, 11,
               "standard MAnalyse requires eleven arguments including mt=false and temporal=false");
    CheckNames(advanced, advancedExpected, 13,
               "advanced MAnalyse requires thirteen arguments including mt=false and temporal=false");
    Check(&GetMAnalyseInvocationContract(false) == &kMAnalyseStandardMtFalseContract,
          "standard profile can select only the explicit mt=false contract");
    Check(&GetMAnalyseInvocationContract(true) == &kMAnalyseAdvancedMtFalseContract,
          "advanced profile can select only the explicit mt=false contract");
    Check(kMAnalyseStandardMtFalseNames[9] != nullptr &&
          std::strcmp(kMAnalyseStandardMtFalseNames[9], "mt") == 0 &&
          !kMAnalyseInternalThreadingEnabled,
          "standard profile's mt argument is explicitly false");
    Check(kMAnalyseAdvancedMtFalseNames[11] != nullptr &&
          std::strcmp(kMAnalyseAdvancedMtFalseNames[11], "mt") == 0 &&
          !kMAnalyseInternalThreadingEnabled,
          "advanced profile's mt argument is explicitly false");
    Check(std::strcmp(kMAnalyseStandardMtFalseNames[10], "temporal") == 0 &&
          std::strcmp(kMAnalyseAdvancedMtFalseNames[12], "temporal") == 0,
          "both MAnalyse paths expose an explicit temporal predictor argument");

    std::cout << "\nRESULT: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << '/' << checks << " checks passed)\n";
    return failures == 0 ? 0 : 1;
}
