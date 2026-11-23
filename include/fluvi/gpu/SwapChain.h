// SPDX-License-Identifier: GPL-3.0-only
// Fluvi gpu-core: flip-model swapchain, frame-latency waitable, tearing, borderless fullscreen, and the
// numbers that prove the frame really reached the screen (present mode, intervals).
// Lesson from Veyra #241: AMD fell into "Composed Flip" (75 ms hitches). We aim for Independent Flip and
// always report the mode we actually got.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>
#include "fluvi/common/Stats.h"
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Device.h"

namespace fluvi::gpu {

struct SwapChainDesc {
    HWND hwnd = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t bufferCount = 3;
    DXGI_FORMAT format = kOutputFormat;
    uint32_t maxFrameLatency = 1;
};

struct PresentRequest {
    uint32_t syncInterval = 0;     // 0 = present now (tearing when allowed, or VRR)
    bool allowTearing = true;      // ignored when unsupported
};

struct DisplayInfo {
    std::wstring deviceName;              // \\.\DISPLAY1
    uint32_t width = 0, height = 0;
    uint32_t refreshNumerator = 0, refreshDenominator = 1;   // exact (e.g. 165000/1000, 59940/1000)
    double refreshHz = 0;
    std::optional<bool> vrrEnabled;        // nullopt = can't tell
    bool tearingSupported = false;         // DXGI_FEATURE_PRESENT_ALLOW_TEARING
};

struct PresentStats {
    uint64_t presents = 0;
    PresentMode mode = PresentMode::Unknown;
    double intervalAvgMs = 0, intervalP99Ms = 0, intervalMaxMs = 0;   // between successive presents, last ~2 s
    uint64_t lateFrames = 0;               // shown later than requested by more than half a refresh
    int64_t lastPresentQpc = 0;            // CPU time Present() returned
    int64_t lastShownQpc = 0;              // when it hit the screen, if known (frame statistics), else 0
};

class SwapChain {
public:
    static std::unique_ptr<SwapChain> Create(Device& device, const SwapChainDesc& desc, Status& status);
    virtual ~SwapChain() = default;

    // Blocks on the frame-latency waitable object. False on timeout.
    virtual bool WaitForNextFrame(uint32_t timeoutMs) = 0;
    virtual uint32_t CurrentIndex() const = 0;
    virtual ID3D12Resource* BackBuffer(uint32_t index) const = 0;   // PRESENT state between frames
    virtual D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    virtual Status Present(const PresentRequest& request) = 0;
    // Waits for the direct queue itself before releasing the back buffers.
    virtual Status Resize(uint32_t width, uint32_t height) = 0;
    // Borderless window covering the monitor the window is on; never exclusive fullscreen.
    virtual void SetFullscreen(bool fullscreen) = 0;
    virtual bool Fullscreen() const = 0;

    virtual DisplayInfo Display() const = 0;   // the monitor the window is on (refreshed on display change/move)
    virtual PresentStats Stats() const = 0;
};

// Monitor refresh switching for the no-VRR case (165 Hz monitor, 60 fps stream -> 120 Hz).
// The returned token restores the previous mode on Restore() and on destruction.
class RefreshOverride {
public:
    static std::unique_ptr<RefreshOverride> Apply(HWND window, double targetHz, Status& status);
    virtual ~RefreshOverride() = default;
    virtual void Restore() = 0;
};

} // namespace fluvi::gpu
