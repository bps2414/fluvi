// SPDX-License-Identifier: GPL-3.0-only
// fluvi_gpu_demo: colour-cycling clear + a moving bar (tearing/judder visible), present stats in the title.
// F11 toggles borderless fullscreen, Esc quits. Optional arguments: seconds to run, then exit; "fs" starts fullscreen.
#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/SwapChain.h"

using namespace fluvi;

namespace {

bool g_quit = false, g_toggle = false, g_resized = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_quit = true;
        if (wp == VK_F11) g_toggle = true;
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { g_toggle = true; return 0; }   // Alt+Enter = our borderless toggle
        break;
    case WM_SIZE: g_resized = true; return 0;
    case WM_CLOSE: g_quit = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

const char* ModeName(PresentMode m) {
    switch (m) {
    case PresentMode::Composed: return "Composed";
    case PresentMode::IndependentFlip: return "Independent Flip";
    case PresentMode::HardwareOverlay: return "HW Overlay";
    default: return "Unknown";
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    const double runSeconds = cmd && *cmd ? _wtof(cmd) : 0;
    g_toggle = cmd && wcsstr(cmd, L"fs") != nullptr;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FluviGpuDemo";
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"fluvi_gpu_demo", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    Status st;
    auto device = gpu::Device::Create({}, st);
    if (!device) { MessageBoxA(hwnd, st.message.c_str(), "fluvi_gpu_demo", MB_ICONERROR); return 1; }
    gpu::SwapChainDesc sd;
    sd.hwnd = hwnd;
    auto swap = gpu::SwapChain::Create(*device, sd, st);
    if (!swap) { MessageBoxA(hwnd, st.message.c_str(), "fluvi_gpu_demo", MB_ICONERROR); return 1; }
    auto ctx = device->CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, 3);

    const int64_t start = time::NowQpc();
    int64_t lastTitle = start;
    uint64_t frames = 0;
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_toggle) { g_toggle = false; swap->SetFullscreen(!swap->Fullscreen()); }
        if (g_resized) {
            g_resized = false;
            RECT c{};
            GetClientRect(hwnd, &c);
            if (c.right > 0 && c.bottom > 0) swap->Resize(uint32_t(c.right), uint32_t(c.bottom));
        }
        if (IsIconic(hwnd)) { Sleep(10); continue; }
        if (!swap->WaitForNextFrame(1000)) continue;

        const double t = time::ElapsedMs(start, time::NowQpc()) / 1000.0;
        const uint32_t i = swap->CurrentIndex();
        auto* list = ctx->Begin();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = swap->BackBuffer(i);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        const float bg[4] = {0.25f + 0.2f * float(std::sin(t)), 0.25f + 0.2f * float(std::sin(t * 1.3 + 2)),
                             0.25f + 0.2f * float(std::sin(t * 0.7 + 4)), 1};
        list->ClearRenderTargetView(swap->Rtv(i), bg, 0, nullptr);
        // Vertical bar crossing the screen once per second: tearing shows as a break, judder as uneven steps.
        const LONG w = LONG(swap->Width()), h = LONG(swap->Height()), barW = std::max<LONG>(8, w / 40);
        const LONG x = LONG(std::fmod(t, 1.0) * double(w + barW)) - barW;
        const D3D12_RECT bar{std::max<LONG>(0, x), 0, std::min(w, x + barW), h};
        const float white[4] = {1, 1, 1, 1};
        if (bar.right > bar.left) list->ClearRenderTargetView(swap->Rtv(i), white, 1, &bar);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        ctx->Submit();
        if (Status ps = swap->Present({0, true}); !ps) { FLUVI_ERROR("demo", "{}", ps.message); break; }
        ++frames;

        const int64_t now = time::NowQpc();
        if (time::ElapsedMs(lastTitle, now) >= 1000) {
            lastTitle = now;
            const auto s = swap->Stats();
            const auto d = swap->Display();
            const std::string title = std::format(
                "fluvi_gpu_demo | {} | {:.3f} Hz ({}/{}) | VRR {} | tearing {} | interval avg {:.2f} p99 {:.2f} max {:.2f} ms | late {} | F11",
                ModeName(s.mode), d.refreshHz, d.refreshNumerator, d.refreshDenominator,
                d.vrrEnabled ? (*d.vrrEnabled ? "on" : "off") : "?", d.tearingSupported ? "yes" : "no", s.intervalAvgMs,
                s.intervalP99Ms, s.intervalMaxMs, s.lateFrames);
            SetWindowTextA(hwnd, title.c_str());
            FLUVI_INFO("demo", "{}", title);
        }
        if (runSeconds > 0 && time::ElapsedMs(start, now) >= runSeconds * 1000) g_quit = true;
    }
    device->WaitIdle();
    if (swap->Fullscreen()) swap->SetFullscreen(false);
    FLUVI_INFO("demo", "{} frames", frames);
    return 0;
}
