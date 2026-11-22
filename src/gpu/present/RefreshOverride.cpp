// SPDX-License-Identifier: GPL-3.0-only
// Temporary monitor refresh switch (no-VRR path: 165 Hz monitor, 60 fps stream -> 120 Hz).
#include <format>

#include "Display.h"
#include "fluvi/core/Log.h"
#include "fluvi/gpu/SwapChain.h"

namespace fluvi::gpu {
namespace {

class RefreshOverrideImpl final : public RefreshOverride {
public:
    explicit RefreshOverrideImpl(std::wstring device) : device_(std::move(device)) {}
    ~RefreshOverrideImpl() override { Restore(); }
    void Restore() override {
        if (!active_) return;
        active_ = false;
        // CDS_FULLSCREEN changes are temporary: a null mode returns to the registry mode.
        const LONG rc = ChangeDisplaySettingsExW(device_.c_str(), nullptr, nullptr, 0, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) FLUVI_WARN("gpu", "refresh restore failed ({})", rc);
        else FLUVI_INFO("gpu", "refresh restored");
    }
    bool active_ = false;

private:
    std::wstring device_;
};

} // namespace

std::unique_ptr<RefreshOverride> RefreshOverride::Apply(HWND window, double targetHz, Status& status) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi)) {
        status = Status::Fail("monitor da janela não encontrado");
        return nullptr;
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &current)) {
        status = Status::Fail("modo atual do monitor ilegível");
        return nullptr;
    }
    std::vector<DEVMODEW> devmodes;
    std::vector<detail::DisplayModeCandidate> modes;
    for (DWORD i = 0;; ++i) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(mi.szDevice, i, &dm, 0)) break;
        devmodes.push_back(dm);
        modes.push_back({dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency});
    }
    const int pick = detail::SelectRefreshMode(modes, current.dmPelsWidth, current.dmPelsHeight, current.dmBitsPerPel, targetHz);
    if (pick < 0) {
        status = Status::Fail("nenhum modo com a resolução atual");
        return nullptr;
    }
    DEVMODEW dm = devmodes[size_t(pick)];
    auto token = std::make_unique<RefreshOverrideImpl>(mi.szDevice);
    if (dm.dmDisplayFrequency == current.dmDisplayFrequency) {
        status = Status::Ok();   // already there; nothing to restore
        return token;
    }
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    const LONG rc = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        status = Status::Fail(std::format("troca de Hz recusada ({})", rc));
        return nullptr;
    }
    token->active_ = true;
    FLUVI_INFO("gpu", "refresh {} -> {} Hz (target {:.3f})", current.dmDisplayFrequency, dm.dmDisplayFrequency, targetHz);
    status = Status::Ok();
    return token;
}

} // namespace fluvi::gpu
