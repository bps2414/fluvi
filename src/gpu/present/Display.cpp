// SPDX-License-Identifier: GPL-3.0-only
#include "Display.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include "fluvi/core/Log.h"

namespace fluvi::gpu::detail {
namespace {

// Path whose source is the GDI device; fills the exact refresh. Prefers the target's video signal timing
// (vSyncFreq: what the cable carries, e.g. 60000/1001) over the path's nominal refresh.
bool ExactRefresh(const std::wstring& gdiName, uint32_t& num, uint32_t& den) {
    UINT32 np = 0, nm = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc;
    do {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
    } while (rc == ERROR_INSUFFICIENT_BUFFER);
    if (rc != ERROR_SUCCESS) return false;
    for (UINT32 i = 0; i < np; ++i) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = path.sourceInfo.adapterId;
        src.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || gdiName != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_RATIONAL r = path.targetInfo.refreshRate;
        const UINT32 mi = path.targetInfo.modeInfoIdx;
        if (mi != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mi < nm && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const auto& v = modes[mi].targetMode.targetVideoSignalInfo.vSyncFreq;
            if (v.Numerator && v.Denominator) r = v;
        }
        if (!r.Numerator || !r.Denominator) return false;
        const uint32_t g = std::gcd(r.Numerator, r.Denominator);
        num = r.Numerator / g;
        den = r.Denominator / g;
        return true;
    }
    return false;
}

// --- AMD Display Library (atiadlxx.dll), loaded at runtime. Layouts from the public ADL SDK. ---
struct AdlAdapterInfo {
    int iSize, iAdapterIndex;
    char strUDID[256];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[256], strDisplayName[256];
    int iPresent, iExist;
    char strDriverPath[256], strDriverPathExt[256], strPNPString[256];
    int iOSDisplayIndex;
};
static_assert(sizeof(AdlAdapterInfo) == 1572);
struct AdlDisplayId { int iDisplayLogicalIndex, iDisplayPhysicalIndex, iDisplayLogicalAdapterIndex, iDisplayPhysicalAdapterIndex; };
struct AdlDisplayInfo {
    AdlDisplayId displayID;
    int iDisplayControllerIndex;
    char strDisplayName[256], strDisplayManufacturerName[256];
    int iDisplayType, iDisplayOutputType, iDisplayConnector, iDisplayInfoMask, iDisplayInfoValue;
};
constexpr int kAdlOk = 0;
constexpr int kAdlDisplayConnected = 0x1, kAdlDisplayMapped = 0x2;
using AdlContext = void*;
using AdlMalloc = void*(__stdcall*)(int);
using FnCreate = int (*)(AdlMalloc, int, AdlContext*);
using FnNumAdapters = int (*)(AdlContext, int*);
using FnAdapterInfo = int (*)(AdlContext, AdlAdapterInfo*, int);
using FnDisplayInfo = int (*)(AdlContext, int, int*, AdlDisplayInfo**, int);
using FnFreeSync = int (*)(AdlContext, int, int, int*, int*, int*, int*);

void* __stdcall AdlAlloc(int size) { return std::malloc(size_t(size)); }

// ADL context and the display -> (adapter, display) mapping live for the process; only the FreeSync
// state is queried each time (cheap). Callers hold QueryVrr's mutex.
struct Adl {
    FnFreeSync freeSync = nullptr;
    FnNumAdapters numAdapters = nullptr;
    FnAdapterInfo adapterInfo = nullptr;
    FnDisplayInfo displayInfo = nullptr;
    AdlContext ctx = nullptr;
    std::unordered_map<std::wstring, std::pair<int, int>> map;   // gdi name -> adapter, display (-1 = none)

    bool Load() {
        HMODULE dll = LoadLibraryExW(L"atiadlxx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return false;
        auto create = reinterpret_cast<FnCreate>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
        numAdapters = reinterpret_cast<FnNumAdapters>(GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get"));
        adapterInfo = reinterpret_cast<FnAdapterInfo>(GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get"));
        displayInfo = reinterpret_cast<FnDisplayInfo>(GetProcAddress(dll, "ADL2_Display_DisplayInfo_Get"));
        freeSync = reinterpret_cast<FnFreeSync>(GetProcAddress(dll, "ADL2_Display_FreeSyncState_Get"));
        return create && numAdapters && adapterInfo && displayInfo && freeSync && create(&AdlAlloc, 1, &ctx) == kAdlOk;
    }

    std::pair<int, int> Find(const std::wstring& gdiName) {
        int n = 0;
        if (numAdapters(ctx, &n) != kAdlOk || n <= 0 || n >= 256) return {-1, -1};
        std::vector<AdlAdapterInfo> infos(static_cast<size_t>(n));
        for (auto& a : infos) a.iSize = sizeof(AdlAdapterInfo);
        if (adapterInfo(ctx, infos.data(), int(infos.size() * sizeof(AdlAdapterInfo))) != kAdlOk) return {-1, -1};
        char want[256]{};
        WideCharToMultiByte(CP_ACP, 0, gdiName.c_str(), -1, want, sizeof(want) - 1, nullptr, nullptr);
        for (const auto& a : infos) {
            if (!a.iPresent || std::strncmp(a.strDisplayName, want, sizeof(want)) != 0) continue;
            int count = 0;
            AdlDisplayInfo* displays = nullptr;
            if (displayInfo(ctx, a.iAdapterIndex, &count, &displays, 0) != kAdlOk) continue;
            int found = -1;
            for (int d = 0; d < count && found < 0; ++d) {
                const auto& di = displays[d];
                const int need = kAdlDisplayConnected | kAdlDisplayMapped;
                if ((di.iDisplayInfoValue & need) == need && di.displayID.iDisplayLogicalAdapterIndex == a.iAdapterIndex)
                    found = di.displayID.iDisplayLogicalIndex;
            }
            std::free(displays);
            if (found >= 0) return {a.iAdapterIndex, found};
        }
        return {-1, -1};
    }
};

std::optional<bool> AdlFreeSync(const std::wstring& gdiName) {
    static Adl adl;
    static const bool loaded = adl.Load();
    if (!loaded) return std::nullopt;
    auto it = adl.map.find(gdiName);
    if (it == adl.map.end()) it = adl.map.emplace(gdiName, adl.Find(gdiName)).first;
    const auto [adapter, display] = it->second;
    if (adapter < 0) return std::nullopt;
    int cur = 0, def = 0, minUHz = 0, maxUHz = 0;
    if (adl.freeSync(adl.ctx, adapter, display, &cur, &def, &minUHz, &maxUHz) != kAdlOk) {
        adl.map.erase(it);   // re-resolve next time (hot plug); this display has no FreeSync now
        return false;
    }
    FLUVI_DEBUG("gpu", "ADL FreeSync: current {} range {:.1f}-{:.1f} Hz", cur, minUHz / 1e6, maxUHz / 1e6);
    return (cur & 1) != 0;
}

} // namespace

std::optional<bool> QueryVrr(const std::wstring& gdiDeviceName) {
    // Windows has no public "is VRR active on this monitor" query (the Graphics settings VRR toggle in
    // DirectXUserGlobalSettings only governs windowed DX11 games). Ask the vendor library; NVAPI later.
    static std::mutex mutex;
    static std::unordered_map<std::wstring, std::pair<ULONGLONG, std::optional<bool>>> cache;
    std::lock_guard lock(mutex);
    const ULONGLONG now = GetTickCount64();
    auto it = cache.find(gdiDeviceName);
    if (it != cache.end() && now - it->second.first < 2000) return it->second.second;
    auto value = AdlFreeSync(gdiDeviceName);
    cache[gdiDeviceName] = {now, value};
    return value;
}

DisplayInfo QueryDisplay(HWND hwnd, bool tearingSupported) {
    DisplayInfo info;
    info.tearingSupported = tearingSupported;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return info;
    info.deviceName = mi.szDevice;
    info.width = uint32_t(mi.rcMonitor.right - mi.rcMonitor.left);
    info.height = uint32_t(mi.rcMonitor.bottom - mi.rcMonitor.top);
    uint32_t num = 0, den = 1;
    if (ExactRefresh(info.deviceName, num, den)) {
        info.refreshNumerator = num;
        info.refreshDenominator = den;
    } else {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            info.refreshNumerator = dm.dmDisplayFrequency;
            info.refreshDenominator = 1;
        }
    }
    info.refreshHz = info.refreshDenominator ? double(info.refreshNumerator) / info.refreshDenominator : 0;
    info.vrrEnabled = QueryVrr(info.deviceName);
    return info;
}

double EffectiveHz(uint32_t f) {
    const uint32_t up = f + 1;
    if (up >= 24 && up % 6 == 0) return up * 1000.0 / 1001.0;
    return double(f);
}

int SelectRefreshMode(const std::vector<DisplayModeCandidate>& modes, uint32_t width, uint32_t height,
                      uint32_t bitsPerPixel, double targetHz) {
    bool anyBpp = false;
    for (const auto& m : modes)
        if (m.width == width && m.height == height && m.bitsPerPixel == bitsPerPixel) anyBpp = true;
    int best = -1;
    double bestDiff = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        const auto& m = modes[i];
        if (m.width != width || m.height != height || (anyBpp && m.bitsPerPixel != bitsPerPixel) || !m.frequency) continue;
        const double diff = std::abs(EffectiveHz(m.frequency) - targetHz);
        if (best < 0 || diff < bestDiff - 1e-6 ||
            (std::abs(diff - bestDiff) <= 1e-6 && m.frequency > modes[size_t(best)].frequency)) {
            best = int(i);
            bestDiff = diff;
        }
    }
    return best;
}

} // namespace fluvi::gpu::detail
