#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include "directmotionSettings.h"
#include "uids.h"
#include "resource.h"
#include "RuntimeDiagnostics.h"

namespace {
void PopulateCombo(HWND dialog, int id, const std::vector<std::wstring>& labels, int selection) {
    const HWND control = GetDlgItem(dialog, id);
    SendMessageW(control, CB_RESETCONTENT, 0, 0);
    for (const auto& label : labels) SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(control, CB_SETCURSEL, selection, 0);
}

bool IsEditControl(WORD id) {
    switch (id) {
    case IDC_SUPER_LEVELS: case IDC_ANALYSE_SEARCH_PARAM: case IDC_ANALYSE_BADRANGE:
    case IDC_ANALYSE_OVERLAP: case IDC_RECALC_SEARCH_PARAM: case IDC_RECALC_OVERLAP:
    case IDC_BLOCKFPS_ML: case IDC_TH_SCD1: case IDC_TH_SCD2:
    case IDC_CPU_WORKERS:
        return true;
    default: return false;
    }
}

bool IsComboControl(WORD id) {
    switch (id) {
    case IDC_RATE_MODE: case IDC_PERFORMANCE: case IDC_GRID:
    case IDC_SUPER_PEL: case IDC_SUPER_SHARP: case IDC_SUPER_RFILTER:
    case IDC_ANALYSE_BLOCK: case IDC_ANALYSE_SEARCH:
    case IDC_RECALC_BLOCK: case IDC_RECALC_SEARCH: case IDC_BLOCKFPS_MODE:
    case IDC_CPU_PROFILE: case IDC_RECEIVE_BATCH: case IDC_PREFETCH_FRAMES:
    case IDC_QUICK_RECALC:
        return true;
    default: return false;
    }
}

bool IsButtonControl(WORD id) {
    switch (id) {
    case IDC_ENABLED: case IDC_SAVE_FIRST_INPUT_BMP: case IDC_ENABLE_LOGGING:
    case IDC_ADVANCED_MVTOOLS: case IDC_ANALYSE_CHROMA: case IDC_ANALYSE_TRUEMOTION:
    case IDC_BLOCKFPS_BLEND:
        return true;
    default: return false;
    }
}

void SetChecked(HWND dialog, int id, bool checked) {
    SendDlgItemMessageW(dialog, id, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool IsChecked(HWND dialog, int id) {
    return SendDlgItemMessageW(dialog, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetInteger(HWND dialog, int id, int value) {
    SetDlgItemInt(dialog, id, static_cast<UINT>(value), TRUE);
}
}

CUnknown* WINAPI CDirectMotionSettingsPage::CreateInstance(LPUNKNOWN punk, HRESULT* phr) {
    auto* page = new CDirectMotionSettingsPage(punk, phr);
    if (!page && phr) *phr = E_OUTOFMEMORY;
    return page;
}

CDirectMotionSettingsPage::CDirectMotionSettingsPage(LPUNKNOWN punk, HRESULT* phr)
    : CBasePropertyPage(NAME("directmotion Settings"), punk,
                        IDD_DIRECTMOTION_SETTINGS, IDS_DIRECTMOTION_TITLE) {
    if (phr) *phr = S_OK;
}

CDirectMotionSettingsPage::~CDirectMotionSettingsPage() {
    if (m_settings) {
        m_settings->Release();
        m_settings = nullptr;
    }
}

HRESULT CDirectMotionSettingsPage::OnConnect(IUnknown* unknown) {
    if (!unknown) return E_POINTER;
    if (m_settings) {
        m_settings->Release();
        m_settings = nullptr;
    }
    return unknown->QueryInterface(IID_IDirectMotionSettings, reinterpret_cast<void**>(&m_settings));
}

HRESULT CDirectMotionSettingsPage::OnDisconnect() {
    if (m_settings) {
        m_settings->Release();
        m_settings = nullptr;
    }
    return S_OK;
}

HRESULT CDirectMotionSettingsPage::OnActivate() {
    if (!m_settings) return E_UNEXPECTED;
    const HRESULT hr = m_settings->GetSettings(&m_values);
    if (FAILED(hr)) return hr;
    PopulateControls();
    m_bDirty = FALSE;
    return S_OK;
}

void CDirectMotionSettingsPage::PopulateControls() {
    int rateModeIndex = 0;
    switch (m_values.rateMode) {
    case kRateMode25x: rateModeIndex = 1; break;
    case kRateMode3x: rateModeIndex = 2; break;
    case kRateMode4x: rateModeIndex = 3; break;
    case kRateModeAuto: rateModeIndex = 4; break;
    default: rateModeIndex = 0; break;
    }
    PopulateCombo(m_Dlg, IDC_RATE_MODE, {
        L"2x source FPS", L"2.5x source FPS", L"3x source FPS",
        L"4x source FPS", L"Auto — display refresh"
    }, rateModeIndex);
    PopulateCombo(m_Dlg, IDC_PERFORMANCE, {
        L"Quality / Exhaustive + 8px", L"Balanced / Hexagon + 8px",
        L"Fast / OneTime + 16px"
    }, m_values.performance - 1);
    int gridIndex = 0; // Profile default: each quick preset owns its MAnalyse grid.
    if (m_values.gridOverrideEnabled) {
        if (m_values.grid == 1) gridIndex = 1;
        else if (m_values.grid == 2) gridIndex = 2;
        else if (m_values.grid == 4) gridIndex = 3;
        else if (m_values.grid == 8) gridIndex = 4;
    }
    PopulateCombo(m_Dlg, IDC_GRID, {
        L"Profile default", L"4 px", L"8 px", L"16 px", L"32 px"
    }, gridIndex);
    PopulateCombo(m_Dlg, IDC_QUICK_RECALC, {
        L"Profile default — Quality/Balanced 8px Exhaustive; Fast 16px OneTime",
        L"Fast refinement — 16px OneTime (diagnostic override)"
    }, std::clamp(m_values.quickRecalcMode, 0, 1));

    PopulateCombo(m_Dlg, IDC_CPU_PROFILE, {
        L"Auto — logical CPUs (max 32)", L"Physical cores — SMT-aware cap",
        L"Half logical — HT estimate", L"Custom worker cap"
    }, std::clamp(m_values.cpuProfile, kCpuProfileAuto, kCpuProfileCustom));
    SetInteger(m_Dlg, IDC_CPU_WORKERS, m_values.customCpuWorkers);
    const int receiveBatchIndex = m_values.receiveBatchFrames == 4 ? 2 :
        (m_values.receiveBatchFrames == 2 ? 1 : 0);
    PopulateCombo(m_Dlg, IDC_RECEIVE_BATCH, {
        L"1 frame", L"2 frames", L"4 frames"
    }, receiveBatchIndex);
    const int prefetchValues[] = { 0, 8, 12, 16, 24, 32, 48, 64 };
    int prefetchIndex = 0;
    for (int i = 0; i < static_cast<int>(_countof(prefetchValues)); ++i) {
        if (prefetchValues[i] == m_values.prefetchFrames) { prefetchIndex = i; break; }
    }
    PopulateCombo(m_Dlg, IDC_PREFETCH_FRAMES, {
        L"Auto (2x workers)", L"8", L"12", L"16", L"24", L"32", L"48", L"64"
    }, prefetchIndex);
    const auto& mv = m_values.mvtools;
    PopulateCombo(m_Dlg, IDC_SUPER_PEL, { L"1 (pixel)", L"2 (half-pixel)", L"4 (quarter-pixel)" },
                  mv.superPel == 1 ? 0 : mv.superPel == 2 ? 1 : 2);
    PopulateCombo(m_Dlg, IDC_SUPER_SHARP, { L"0", L"1", L"2" }, mv.superSharp);
    PopulateCombo(m_Dlg, IDC_SUPER_RFILTER, { L"0", L"1", L"2", L"3", L"4" }, mv.superRFilter);
    SetInteger(m_Dlg, IDC_SUPER_LEVELS, mv.superLevels);

    const std::vector<std::wstring> blockSizes{ L"4", L"8", L"16", L"32" };
    auto blockIndex = [](int block) { return block == 4 ? 0 : block == 8 ? 1 : block == 16 ? 2 : 3; };
    const std::vector<std::wstring> searchTypes{
        L"0 — One-time", L"1 — N-step", L"2 — Diamond", L"3 — Exhaustive",
        L"4 — Hexagon", L"5 — UMH", L"6 — Horizontal", L"7 — Vertical"
    };
    PopulateCombo(m_Dlg, IDC_ANALYSE_BLOCK, blockSizes, blockIndex(mv.analyseBlockSize));
    PopulateCombo(m_Dlg, IDC_ANALYSE_SEARCH, searchTypes, mv.analyseSearch);
    SetInteger(m_Dlg, IDC_ANALYSE_SEARCH_PARAM, mv.analyseSearchParam);
    SetInteger(m_Dlg, IDC_ANALYSE_BADRANGE, mv.analyseBadRange);
    SetInteger(m_Dlg, IDC_ANALYSE_OVERLAP, mv.analyseOverlap);
    SetChecked(m_Dlg, IDC_ANALYSE_CHROMA, mv.analyseChroma);
    SetChecked(m_Dlg, IDC_ANALYSE_TRUEMOTION, mv.analyseTrueMotion);

    PopulateCombo(m_Dlg, IDC_RECALC_BLOCK, blockSizes, blockIndex(mv.recalcBlockSize));
    PopulateCombo(m_Dlg, IDC_RECALC_SEARCH, searchTypes, mv.recalcSearch);
    SetInteger(m_Dlg, IDC_RECALC_SEARCH_PARAM, mv.recalcSearchParam);
    SetInteger(m_Dlg, IDC_RECALC_OVERLAP, mv.recalcOverlap);

    PopulateCombo(m_Dlg, IDC_BLOCKFPS_MODE, {
        L"0 — Average (fastest)", L"1 — Static median", L"2 — Dynamic median",
        L"3 — Time-weighted", L"4 — Weighted + average", L"5 — Occlusion mask (debug only)",
        L"6 — SAD time-weighted", L"7 — SAD weighted + average", L"8 — SAD occlusion mask (debug only)"
    }, mv.blockFpsMode);
    wchar_t maskText[48]{};
    swprintf_s(maskText, L"%.2f", mv.blockFpsMaskScale);
    SetDlgItemTextW(m_Dlg, IDC_BLOCKFPS_ML, maskText);
    SetChecked(m_Dlg, IDC_BLOCKFPS_BLEND, mv.blockFpsBlend);
    SetInteger(m_Dlg, IDC_TH_SCD1, mv.thSCD1);
    SetInteger(m_Dlg, IDC_TH_SCD2, mv.thSCD2);

    SetChecked(m_Dlg, IDC_ENABLED, m_values.enabled);
    SetChecked(m_Dlg, IDC_ADVANCED_MVTOOLS, mv.advanced);
    SetChecked(m_Dlg, IDC_SAVE_FIRST_INPUT_BMP, m_values.saveFirstInputBmp);
    SetChecked(m_Dlg, IDC_ENABLE_LOGGING, m_values.logEnabled);

    UpdateControlEnablement();
}

void CDirectMotionSettingsPage::UpdateControlEnablement() {
    const bool advanced = IsChecked(m_Dlg, IDC_ADVANCED_MVTOOLS);
    const int cpuProfile = static_cast<int>(SendDlgItemMessageW(m_Dlg, IDC_CPU_PROFILE, CB_GETCURSEL, 0, 0));
    const int advancedControls[] = {
        IDC_SUPER_PEL, IDC_SUPER_SHARP, IDC_SUPER_RFILTER, IDC_SUPER_LEVELS,
        IDC_ANALYSE_BLOCK, IDC_ANALYSE_SEARCH, IDC_ANALYSE_SEARCH_PARAM,
        IDC_ANALYSE_BADRANGE, IDC_ANALYSE_OVERLAP, IDC_ANALYSE_CHROMA,
        IDC_ANALYSE_TRUEMOTION, IDC_RECALC_BLOCK, IDC_RECALC_SEARCH,
        IDC_RECALC_SEARCH_PARAM, IDC_RECALC_OVERLAP, IDC_BLOCKFPS_MODE,
        IDC_BLOCKFPS_ML, IDC_BLOCKFPS_BLEND, IDC_TH_SCD1, IDC_TH_SCD2
    };
    for (const int id : advancedControls) EnableWindow(GetDlgItem(m_Dlg, id), advanced ? TRUE : FALSE);
    EnableWindow(GetDlgItem(m_Dlg, IDC_PERFORMANCE), advanced ? FALSE : TRUE);
    EnableWindow(GetDlgItem(m_Dlg, IDC_GRID), advanced ? FALSE : TRUE);
    EnableWindow(GetDlgItem(m_Dlg, IDC_QUICK_RECALC), advanced ? FALSE : TRUE);
    EnableWindow(GetDlgItem(m_Dlg, IDC_CPU_WORKERS), cpuProfile == kCpuProfileCustom ? TRUE : FALSE);
    SetDlgItemTextW(m_Dlg, IDC_CPU_HINT,
        L"Independent of profiles. Auto = 2x workers; more frames use RAM and increase startup/seek buffering.");
    const std::wstring capabilityText = advanced
        ? L"Advanced MVTools overrides active; stop playback before applying."
        : L"Profiles define both vector stages. Grid override affects MAnalyse; Refinement can test Fast's 16px OneTime MRecalculate.";
    SetDlgItemTextW(m_Dlg, IDC_CAPABILITIES, capabilityText.c_str());
}

void CDirectMotionSettingsPage::MarkDirty() {
    m_bDirty = TRUE;
    if (m_pPageSite) m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
}

bool CDirectMotionSettingsPage::ReadInteger(int controlId, int& value, bool signedValue) const {
    BOOL ok = FALSE;
    const UINT result = GetDlgItemInt(m_Dlg, controlId, &ok, signedValue ? TRUE : FALSE);
    if (!ok) return false;
    // GetDlgItemInt returns a UINT; on Windows, two's-complement conversion preserves
    // negative values for the one explicitly signed control (MAnalyse badrange).
    value = static_cast<int>(result);
    return true;
}

bool CDirectMotionSettingsPage::ReadDouble(int controlId, double& value) const {
    wchar_t buffer[80]{};
    if (GetDlgItemTextW(m_Dlg, controlId, buffer, static_cast<int>(_countof(buffer))) <= 0) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(buffer, &end);
    if (end == buffer) return false;
    while (*end == L' ' || *end == L'\t') ++end;
    if (*end != L'\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

INT_PTR CDirectMotionSettingsPage::OnReceiveMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        const WORD notification = HIWORD(wParam);
        if ((IsButtonControl(id) && notification == BN_CLICKED) ||
            (IsComboControl(id) && notification == CBN_SELCHANGE) ||
            (IsEditControl(id) && notification == EN_CHANGE)) {
            if ((IsButtonControl(id) && id == IDC_ADVANCED_MVTOOLS) ||
                (IsComboControl(id) && (id == IDC_RATE_MODE || id == IDC_CPU_PROFILE))) {
                UpdateControlEnablement();
            }
            MarkDirty();
            return 1;
        }
    }
    return CBasePropertyPage::OnReceiveMessage(hwnd, message, wParam, lParam);
}

HRESULT CDirectMotionSettingsPage::OnApplyChanges() {
    if (!m_settings) return E_UNEXPECTED;
    DirectMotionSettings candidate = m_values;
    auto selection = [this](int id) -> int {
        return static_cast<int>(SendDlgItemMessageW(m_Dlg, id, CB_GETCURSEL, 0, 0));
    };
    const int rateMode = selection(IDC_RATE_MODE);
    const int performanceIndex = selection(IDC_PERFORMANCE);
    const int gridIndex = selection(IDC_GRID);
    const int quickRecalcIndex = selection(IDC_QUICK_RECALC);
    if (rateMode < 0 || performanceIndex < 0 || gridIndex < 0 ||
        quickRecalcIndex < 0 || quickRecalcIndex > 1) return E_INVALIDARG;

    static constexpr int rateModes[] = {
        kRateMode2x, kRateMode25x, kRateMode3x, kRateMode4x, kRateModeAuto
    };
    if (rateMode < 0 || rateMode >= static_cast<int>(_countof(rateModes))) return E_INVALIDARG;
    candidate.rateMode = rateModes[rateMode];
    candidate.performance = performanceIndex + 1;
    candidate.quickRecalcMode = quickRecalcIndex;
    if (gridIndex < 0 || gridIndex > 4) return E_INVALIDARG;
    candidate.gridOverrideEnabled = gridIndex != 0;
    switch (gridIndex) {
    case 0: candidate.grid = 4; break; // Retain the legacy stored default, but disable override.
    case 1: candidate.grid = 1; break;
    case 2: candidate.grid = 2; break;
    case 3: candidate.grid = 4; break;
    case 4: candidate.grid = 8; break;
    default: return E_INVALIDARG;
    }
    const int cpuProfileIndex = selection(IDC_CPU_PROFILE);
    const int receiveBatchIndex = selection(IDC_RECEIVE_BATCH);
    const int prefetchIndex = selection(IDC_PREFETCH_FRAMES);
    if (cpuProfileIndex < 0 || cpuProfileIndex > 3 || receiveBatchIndex < 0 || receiveBatchIndex > 2 ||
        prefetchIndex < 0 || prefetchIndex > 7)
        return E_INVALIDARG;
    candidate.cpuProfile = cpuProfileIndex;
    candidate.receiveBatchFrames = receiveBatchIndex == 0 ? 1 : (receiveBatchIndex == 1 ? 2 : 4);
    static constexpr int prefetchValues[] = { 0, 8, 12, 16, 24, 32, 48, 64 };
    candidate.prefetchFrames = prefetchValues[prefetchIndex];
    int customWorkers = candidate.customCpuWorkers;
    if (ReadInteger(IDC_CPU_WORKERS, customWorkers)) candidate.customCpuWorkers = customWorkers;
    if (candidate.cpuProfile == kCpuProfileCustom &&
        (candidate.customCpuWorkers < 1 || candidate.customCpuWorkers > 32)) {
        MessageBoxW(m_Dlg, L"Custom CPU worker cap must be between 1 and 32.", L"directmotion", MB_OK | MB_ICONWARNING);
        return E_INVALIDARG;
    }
    auto& mv = candidate.mvtools;
    mv.advanced = IsChecked(m_Dlg, IDC_ADVANCED_MVTOOLS);
    if (mv.advanced) {
        const int superPelIndex = selection(IDC_SUPER_PEL);
        const int superSharp = selection(IDC_SUPER_SHARP);
        const int superRFilter = selection(IDC_SUPER_RFILTER);
        const int analyseBlockIndex = selection(IDC_ANALYSE_BLOCK);
        const int analyseSearch = selection(IDC_ANALYSE_SEARCH);
        const int recalcBlockIndex = selection(IDC_RECALC_BLOCK);
        const int recalcSearch = selection(IDC_RECALC_SEARCH);
        const int blockFpsMode = selection(IDC_BLOCKFPS_MODE);
        if (superPelIndex < 0 || superSharp < 0 || superRFilter < 0 || analyseBlockIndex < 0 ||
            analyseSearch < 0 || recalcBlockIndex < 0 || recalcSearch < 0 || blockFpsMode < 0) return E_INVALIDARG;

        mv.superPel = superPelIndex == 0 ? 1 : superPelIndex == 1 ? 2 : 4;
        mv.superSharp = superSharp;
        mv.superRFilter = superRFilter;
        if (!ReadInteger(IDC_SUPER_LEVELS, mv.superLevels) ||
            !ReadInteger(IDC_ANALYSE_SEARCH_PARAM, mv.analyseSearchParam) ||
            !ReadInteger(IDC_ANALYSE_BADRANGE, mv.analyseBadRange, true) ||
            !ReadInteger(IDC_ANALYSE_OVERLAP, mv.analyseOverlap) ||
            !ReadInteger(IDC_RECALC_SEARCH_PARAM, mv.recalcSearchParam) ||
            !ReadInteger(IDC_RECALC_OVERLAP, mv.recalcOverlap) ||
            !ReadInteger(IDC_TH_SCD1, mv.thSCD1) || !ReadInteger(IDC_TH_SCD2, mv.thSCD2) ||
            !ReadDouble(IDC_BLOCKFPS_ML, mv.blockFpsMaskScale)) {
            MessageBoxW(m_Dlg, L"One or more numeric MVTools values are invalid.", L"directmotion", MB_OK | MB_ICONWARNING);
            return E_INVALIDARG;
        }
        static const int blocks[] = { 4, 8, 16, 32 };
        mv.analyseBlockSize = blocks[analyseBlockIndex];
        mv.analyseSearch = analyseSearch;
        mv.recalcBlockSize = blocks[recalcBlockIndex];
        mv.recalcSearch = recalcSearch;
        mv.blockFpsMode = blockFpsMode;
        mv.analyseChroma = IsChecked(m_Dlg, IDC_ANALYSE_CHROMA);
        mv.analyseTrueMotion = IsChecked(m_Dlg, IDC_ANALYSE_TRUEMOTION);
        mv.blockFpsBlend = IsChecked(m_Dlg, IDC_BLOCKFPS_BLEND);

        if (mv.superLevels < 0 || mv.superLevels > 10 ||
            mv.analyseBlockSize < 4 || mv.analyseSearchParam < 0 || mv.analyseSearchParam > 64 ||
            mv.analyseBadRange < -128 || mv.analyseBadRange > 256 ||
            mv.analyseOverlap < 0 || mv.analyseOverlap > mv.analyseBlockSize / 2 || (mv.analyseOverlap & 1) ||
            mv.recalcSearchParam < 0 || mv.recalcSearchParam > 64 ||
            mv.recalcOverlap < 0 || mv.recalcOverlap > mv.recalcBlockSize / 2 || (mv.recalcOverlap & 1) ||
            mv.blockFpsMaskScale < 0.0 || mv.blockFpsMaskScale > 1000.0 ||
            mv.thSCD1 < 0 || mv.thSCD1 > 1000 || mv.thSCD2 < 0 || mv.thSCD2 > 1000) {
            MessageBoxW(m_Dlg, L"Some values are outside MVTools' supported range. Overlap must be even and no greater than half the block size.", L"directmotion", MB_OK | MB_ICONWARNING);
            return E_INVALIDARG;
        }
        if (mv.blockFpsMode == 5 || mv.blockFpsMode == 8) {
            const int answer = MessageBoxW(m_Dlg,
                L"MBlockFps mode 5 or 8 emits an occlusion/SAD mask for diagnostics, not a normal interpolated video image. Apply this debug mode anyway?",
                L"directmotion — debug output mode", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return E_ABORT;
        }
    }
    candidate.enabled = IsChecked(m_Dlg, IDC_ENABLED);
    candidate.saveFirstInputBmp = IsChecked(m_Dlg, IDC_SAVE_FIRST_INPUT_BMP);
    candidate.logEnabled = IsChecked(m_Dlg, IDC_ENABLE_LOGGING);

    const HRESULT hr = m_settings->SetSettings(&candidate);
    if (FAILED(hr)) {
        const wchar_t* message = hr == VFW_E_NOT_STOPPED
            ? L"Stop playback before applying settings; DirectShow must rebuild the interpolation graph while stopped."
            : L"Could not apply these settings. Check block/overlap compatibility and MVTools parameter ranges.";
        MessageBoxW(m_Dlg, message, L"directmotion", MB_OK | MB_ICONINFORMATION);
        return hr;
    }
    m_values = candidate;
    PopulateControls();
    m_bDirty = FALSE;
    return S_OK;
}
