// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), src/gfx/PresentSink.cpp @ c6633121. Changes: only the frame-statistics scanout estimate and percentile style; new swapchain, interval stats and present-mode reporting.
#include "fluvi/gpu/SwapChain.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "Display.h"
#include "PresentModeEtw.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"

using Microsoft::WRL::ComPtr;

namespace fluvi::gpu {
namespace {

const char* ModeName(PresentMode m) {
    switch (m) {
    case PresentMode::Composed: return "Composed";
    case PresentMode::IndependentFlip: return "Independent Flip";
    case PresentMode::HardwareOverlay: return "Hardware Overlay";
    default: return "Unknown";
    }
}

class SwapChainImpl final : public SwapChain {
public:
    explicit SwapChainImpl(Device& device) : device_(device) {}

    ~SwapChainImpl() override {
        if (sc_) device_.WaitIdle();
        if (fullscreen_) SetFullscreen(false);
        ReleaseBuffers();
        device_.RtvHeap().Free(rtv_);
        if (waitable_) CloseHandle(waitable_);
    }

    bool Init(const SwapChainDesc& desc, Status& status) {
        desc_ = desc;
        hwnd_ = desc.hwnd;
        if (!hwnd_ || !IsWindow(hwnd_)) {
            status = Status::Fail("janela inválida");
            return false;
        }
        BOOL allow = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(device_.Factory()->QueryInterface(IID_PPV_ARGS(&f5))) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearing_ = allow != FALSE;
        flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        width_ = desc.width ? desc.width : std::max<uint32_t>(1, uint32_t(rc.right - rc.left));
        height_ = desc.height ? desc.height : std::max<uint32_t>(1, uint32_t(rc.bottom - rc.top));
        const uint32_t count = std::clamp<uint32_t>(desc.bufferCount, 2, DXGI_MAX_SWAP_CHAIN_BUFFERS);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width_;
        sd.Height = height_;
        sd.Format = desc.format;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = count;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = flags_;
        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = device_.Factory()->CreateSwapChainForHwnd(device_.DirectQueue(), hwnd_, &sd, nullptr, nullptr, &sc1);
        if (FAILED(hr) || FAILED(sc1.As(&sc_))) {
            status = Status::Fail(std::format("CreateSwapChainForHwnd falhou (0x{:08X})", unsigned(hr)));
            return false;
        }
        // Never exclusive fullscreen: Alt+Enter is ours (borderless).
        device_.Factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        sc_->SetMaximumFrameLatency(std::max<uint32_t>(1, desc.maxFrameLatency));
        waitable_ = sc_->GetFrameLatencyWaitableObject();

        rtv_ = device_.RtvHeap().Allocate(count);
        if (!rtv_) {
            status = Status::Fail("sem RTVs para o swapchain");
            return false;
        }
        buffers_.resize(count);
        if (!AcquireBuffers()) {
            status = Status::Fail("GetBuffer falhou");
            return false;
        }
        etw_ = PresentModeEtw::Acquire();
        const DisplayInfo d = Display();
        FLUVI_INFO("gpu", "swapchain {}x{} x{} tearing={} | monitor {}x{} @ {}/{} ({:.3f} Hz) VRR={}", width_, height_,
                   count, tearing_, d.width, d.height, d.refreshNumerator, d.refreshDenominator, d.refreshHz,
                   d.vrrEnabled ? (*d.vrrEnabled ? "on" : "off") : "unknown");
        status = Status::Ok();
        return true;
    }

    bool WaitForNextFrame(uint32_t timeoutMs) override {
        return !waitable_ || WaitForSingleObjectEx(waitable_, timeoutMs, TRUE) == WAIT_OBJECT_0;
    }
    uint32_t CurrentIndex() const override { return sc_->GetCurrentBackBufferIndex(); }
    ID3D12Resource* BackBuffer(uint32_t i) const override { return i < buffers_.size() ? buffers_[i].Get() : nullptr; }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const override { return rtv_.Cpu(i); }
    uint32_t Width() const override { return width_; }
    uint32_t Height() const override { return height_; }

    Status Present(const PresentRequest& req) override {
        const UINT flags = (req.syncInterval == 0 && req.allowTearing && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT hr = sc_->Present(std::min<uint32_t>(req.syncInterval, 4), flags);
        const int64_t now = time::NowQpc();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            device_.CheckRemoved();
            return Status::Fail(std::format("GPU removida (0x{:08X})", unsigned(device_.D3D()->GetDeviceRemovedReason())));
        }
        if (FAILED(hr)) return Status::Fail(std::format("Present falhou (0x{:08X})", unsigned(hr)));
        std::lock_guard lock(statsMutex_);
        OnPresentLocked(now);
        SampleFrameStatisticsLocked();
        return Status::Ok();
    }

    Status Resize(uint32_t width, uint32_t height) override {
        width = std::max<uint32_t>(1, width);
        height = std::max<uint32_t>(1, height);
        if (width == width_ && height == height_) return Status::Ok();
        device_.WaitIdle();
        ReleaseBuffers();
        const HRESULT hr = sc_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags_);
        if (FAILED(hr)) {
            device_.CheckRemoved();
            return Status::Fail(std::format("ResizeBuffers falhou (0x{:08X})", unsigned(hr)));
        }
        width_ = width;
        height_ = height;
        if (!AcquireBuffers()) return Status::Fail("GetBuffer falhou");
        std::lock_guard lock(statsMutex_);
        statsBaseValid_ = false;
        return Status::Ok();
    }

    void SetFullscreen(bool fullscreen) override {
        if (fullscreen == fullscreen_) return;
        if (fullscreen) {
            savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            savedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(hwnd_, &savedPlacement_);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            const RECT r = mi.rcMonitor;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_ & ~LONG_PTR(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
            SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_);
            SetWindowPlacement(hwnd_, &savedPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        }
        fullscreen_ = fullscreen;
        std::lock_guard lock(displayMutex_);
        displayStamp_ = 0;   // the monitor may have changed
    }
    bool Fullscreen() const override { return fullscreen_; }

    DisplayInfo Display() const override {
        std::lock_guard lock(displayMutex_);
        const ULONGLONG now = GetTickCount64();
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (!displayStamp_ || mon != displayMonitor_ || now - displayStamp_ > 1000) {
            display_ = detail::QueryDisplay(hwnd_, tearing_);
            refreshHzHint_ = display_.refreshHz;
            displayMonitor_ = mon;
            displayStamp_ = now;
        }
        return display_;
    }

    PresentStats Stats() const override {
        PresentStats s;
        {
            std::lock_guard lock(statsMutex_);
            s.presents = presents_;
            s.lateFrames = late_;
            s.lastPresentQpc = lastPresentQpc_;
            s.lastShownQpc = lastShownQpc_;
            std::vector<double> iv;
            iv.reserve(times_.size());
            for (size_t i = 1; i < times_.size(); ++i) iv.push_back(time::ElapsedMs(times_[i - 1], times_[i]));
            if (!iv.empty()) {
                double sum = 0;
                for (double v : iv) sum += v;
                s.intervalAvgMs = sum / double(iv.size());
                std::sort(iv.begin(), iv.end());
                s.intervalP99Ms = iv[std::min(iv.size() - 1, (iv.size() * 99 + 99) / 100 - 1)];
                s.intervalMaxMs = iv.back();
            }
        }
        s.mode = etw_ ? etw_->Mode() : PresentMode::Unknown;
        if (s.mode != loggedMode_) {
            loggedMode_ = s.mode;
            FLUVI_INFO("gpu", "present mode: {}", ModeName(s.mode));
        }
        return s;
    }

private:
    bool AcquireBuffers() {
        for (uint32_t i = 0; i < buffers_.size(); ++i) {
            if (FAILED(sc_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) return false;
            buffers_[i]->SetName(std::format(L"fluvi.backbuffer{}", i).c_str());
            device_.D3D()->CreateRenderTargetView(buffers_[i].Get(), nullptr, rtv_.Cpu(i));
        }
        return true;
    }
    void ReleaseBuffers() {
        for (auto& b : buffers_) b.Reset();
    }

    double RefreshPeriodQpcLocked() const {
        if (refreshQpc_ > 0) return refreshQpc_;
        const double hz = refreshHzHint_.load();
        return hz >= 1 ? double(time::QpcFrequency()) / hz : 0;
    }

    // Interval window (~2 s) and late frames: a present whose interval exceeds the window median by more
    // than half a refresh came later than the cadence asked for.
    void OnPresentLocked(int64_t now) {
        ++presents_;
        if (lastPresentQpc_) {
            const int64_t interval = now - lastPresentQpc_;
            if (times_.size() >= 8) {
                scratch_.clear();
                for (size_t i = 1; i < times_.size(); ++i) scratch_.push_back(times_[i] - times_[i - 1]);
                auto mid = scratch_.begin() + ptrdiff_t(scratch_.size() / 2);
                std::nth_element(scratch_.begin(), mid, scratch_.end());
                const double period = RefreshPeriodQpcLocked();
                if (period > 0 && double(interval) > double(*mid) + period * 0.5) ++late_;
            }
        }
        lastPresentQpc_ = now;
        times_.push_back(now);
        const int64_t cutoff = now - 2 * time::QpcFrequency();
        while (times_.size() > 2 && times_.front() < cutoff) times_.pop_front();
    }

    // When the newest shown present hit the screen (Veyra's scanout estimate: the vblank of the latest
    // statistics minus the refreshes since that present was latched).
    void SampleFrameStatisticsLocked() {
        DXGI_FRAME_STATISTICS st{};
        if (FAILED(sc_->GetFrameStatistics(&st)) || !st.SyncQPCTime.QuadPart) {
            statsBaseValid_ = false;   // DISJOINT: mode change or first frames
            return;
        }
        if (statsBaseValid_ && st.SyncRefreshCount > lastSyncRefresh_ && st.SyncQPCTime.QuadPart > lastSyncQpc_)
            refreshQpc_ = double(st.SyncQPCTime.QuadPart - lastSyncQpc_) / double(st.SyncRefreshCount - lastSyncRefresh_);
        statsBaseValid_ = true;
        lastSyncQpc_ = st.SyncQPCTime.QuadPart;
        lastSyncRefresh_ = st.SyncRefreshCount;
        const double period = RefreshPeriodQpcLocked();
        if (period > 0 && st.SyncRefreshCount >= st.PresentRefreshCount && st.SyncRefreshCount - st.PresentRefreshCount <= 16)
            lastShownQpc_ = st.SyncQPCTime.QuadPart - int64_t(double(st.SyncRefreshCount - st.PresentRefreshCount) * period);
    }

    Device& device_;
    SwapChainDesc desc_;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain3> sc_;
    std::vector<ComPtr<ID3D12Resource>> buffers_;
    DescriptorRange rtv_;
    HANDLE waitable_ = nullptr;
    UINT flags_ = 0;
    bool tearing_ = false;
    uint32_t width_ = 0, height_ = 0;

    bool fullscreen_ = false;
    LONG_PTR savedStyle_ = 0, savedExStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{};

    mutable std::mutex displayMutex_;
    mutable DisplayInfo display_;
    mutable HMONITOR displayMonitor_ = nullptr;
    mutable ULONGLONG displayStamp_ = 0;

    std::shared_ptr<PresentModeEtw> etw_;
    mutable std::atomic<PresentMode> loggedMode_{PresentMode::Unknown};
    mutable std::atomic<double> refreshHzHint_{0};

    mutable std::mutex statsMutex_;
    uint64_t presents_ = 0, late_ = 0;
    int64_t lastPresentQpc_ = 0, lastShownQpc_ = 0;
    std::deque<int64_t> times_;
    std::vector<int64_t> scratch_;
    bool statsBaseValid_ = false;
    int64_t lastSyncQpc_ = 0;
    UINT lastSyncRefresh_ = 0;
    double refreshQpc_ = 0;
};

} // namespace

std::unique_ptr<SwapChain> SwapChain::Create(Device& device, const SwapChainDesc& desc, Status& status) {
    auto sc = std::make_unique<SwapChainImpl>(device);
    if (!sc->Init(desc, status)) return nullptr;
    return sc;
}

} // namespace fluvi::gpu
