#pragma once

#include <streams.h>
#include "directmotion.h"

class CDirectMotionSettingsPage final : public CBasePropertyPage {
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN punk, HRESULT* phr);
    CDirectMotionSettingsPage(LPUNKNOWN punk, HRESULT* phr);
    ~CDirectMotionSettingsPage() override;

protected:
    HRESULT OnConnect(IUnknown* unknown) override;
    HRESULT OnDisconnect() override;
    HRESULT OnActivate() override;
    INT_PTR OnReceiveMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) override;
    HRESULT OnApplyChanges() override;

private:
    void PopulateControls();
    void UpdateControlEnablement();
    void MarkDirty();
    bool ReadInteger(int controlId, int& value, bool signedValue = false) const;
    bool ReadDouble(int controlId, double& value) const;

    IDirectMotionSettings* m_settings = nullptr;
    DirectMotionSettings m_values{};
};
