// SPDX-License-Identifier: GPL-3.0-only
// fluvi_ui_demo: the five screens with fake data over an animated "video" background.
// Keys 1-5 switch screens (Entrar, Consoles, Conectando, Stream, Configurações); F1 overlay, F2 Jogar/Liso, F5 reload.
// Arguments: [seconds] [--screen N]. With seconds, it walks through every screen (overlay open on the stream), then exits
// and reports D3D12 debug-layer errors; exit code 2 if there were any or the device was removed.
#include <windows.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/SwapChain.h"
#include "fluvi/ui/Ui.h"

using namespace fluvi;

namespace {

ui::Ui* g_ui = nullptr;
bool g_quit = false, g_resized = false;
int g_screenKey = -1;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp >= '1' && wp <= '5') {
        g_screenKey = static_cast<int>(wp - '1');
        return 0;
    }
    if (g_ui && g_ui->HandleMessage(hwnd, msg, wp, lp)) return 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_ui && !g_ui->WantsInput()) g_quit = true;
        return 0;
    case WM_SIZE: g_resized = true; return 0;
    case WM_CLOSE: g_quit = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::filesystem::path FindAssets() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (auto dir = std::filesystem::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "assets/ui/theme.rcss")) return dir / "assets/ui";
        if (dir == dir.parent_path()) break;
    }
    return std::filesystem::current_path() / "assets/ui";
}

ui::SignInModel FakeSignIn(bool busy, bool loading) {
    ui::SignInModel m;
    if (busy && !loading) {
        m.userCode = "B7KQ-M4XZ";
        m.verificationUri = "https://www.microsoft.com/link";
        m.status = "Esperando você confirmar no outro aparelho";
    }
    m.busy = busy;
    return m;
}

ui::ConsolesModel FakeConsoles(bool loading) {
    ui::ConsolesModel m;
    m.refreshing = loading;
    if (loading) return m;   // first refresh: no account, no list yet (skeletons)
    m.accountName = "Remanso Azul";
    m.consoles = {
        {"F4000A1B2C3D", "Sala", "Xbox Series X", ui::PowerState::On, false},
        {"F4000E5F6A7B", "Quarto", "Xbox Series S", ui::PowerState::Standby, false},
        {"F4000C8D9E0F", "Escritório", "Xbox One X", ui::PowerState::Off, false},
    };
    return m;
}

ui::ConnectingModel FakeConnecting(double t, bool loading) {
    ui::ConnectingModel m;
    if (loading) return m;   // console name not known yet (skeleton)
    m.consoleName = "Sala";
    const int stage = static_cast<int>(std::fmod(t, 10.0) / 2.0);
    m.stage = stage >= 4 ? ui::ConnectStage::Failed : static_cast<ui::ConnectStage>(stage);
    if (m.stage == ui::ConnectStage::Failed) m.detail = "O console recusou a sessão (código 0x87E10BC6). Tente de novo em alguns segundos.";
    return m;
}

OverlayStats FakeStats(double t, StreamMode mode) {
    OverlayStats s;
    s.mode = mode;
    s.streamWidth = 2560;
    s.streamHeight = 1440;
    s.outputWidth = 1920;
    s.outputHeight = 1080;
    s.bitrateMbps = 38.4 + 3.0 * std::sin(t * 0.7);
    s.gameFps = 30;
    s.streamFps = 60;
    s.generatedFps = mode == StreamMode::Smooth ? 30 : 0;
    s.presentedFps = mode == StreamMode::Smooth ? 60 : 59.9;
    s.latencyMs = 14.0 + 3.0 * std::sin(t * 1.3);
    s.rttMs = 6.0 + std::sin(t);
    s.decodeMs = 2.1;
    s.gpuMs = 1.4;
    s.packetsLost = static_cast<uint64_t>(t / 7.0);
    s.presentMode = PresentMode::IndependentFlip;
    s.tearing = false;
    s.vrr = true;
    s.refreshHz = 143.856;
    return s;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

// Stand-in for the video: drifting colour bands, so the frosted overlay has something to blur.
void DrawFakeVideo(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h, double t) {
    const float sky[4] = {0.10f, 0.20f + 0.05f * float(std::sin(t * 0.5)), 0.28f, 1};
    list->ClearRenderTargetView(rtv, sky, 0, nullptr);
    const LONG W = LONG(w), H = LONG(h);
    for (int i = 0; i < 9; ++i) {
        const double phase = t * (0.15 + 0.04 * i) + i * 0.9;
        const LONG x = LONG((std::fmod(phase, 1.4) - 0.2) * W);
        const LONG bw = W / (6 + i);
        const D3D12_RECT r{std::max<LONG>(0, x), LONG(H * (0.08 + 0.1 * i)), std::min(W, x + bw), LONG(H * (0.16 + 0.1 * i))};
        const float c[4] = {0.9f - 0.07f * i, 0.45f + 0.05f * i, 0.25f + 0.06f * i, 1};
        if (r.right > r.left) list->ClearRenderTargetView(rtv, c, 1, &r);
    }
}

int ReportDebugLayer(ID3D12Device* device) {
    ID3D12InfoQueue* q = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) return 0;
    int errors = 0;
    const UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::string buf(len, '\0');
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(q->GetMessage(i, m, &len))) continue;
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;
            if (errors <= 20) std::fprintf(stderr, "D3D12 %s: %s\n", m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "CORRUPTION", m->pDescription);
        } else if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "D3D12 WARNING: %s\n", m->pDescription);
        }
    }
    q->Release();
    return errors;
}

} // namespace

int main(int argc, char** argv) {
    double runSeconds = 0;
    int startScreen = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) startScreen = std::clamp(std::atoi(argv[++i]) - 1, 0, 4);
        else runSeconds = std::atof(argv[i]);
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FluviUiDemo";
    RegisterClassExW(&wc);
    RECT r{0, 0, 1600, 900};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"fluvi_ui_demo  (1-5 telas, F1 overlay, F2 modo, F5 recarrega)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    Status st;
    gpu::DeviceDesc dd;
    dd.debugLayer = true;   // needs the Graphics Tools optional feature; fall back without it
    auto device = gpu::Device::Create(dd, st);
    if (!device) {
        std::fprintf(stderr, "debug layer unavailable (%s), running without it\n", st.message.c_str());
        device = gpu::Device::Create({}, st);
    }
    if (!device) { std::fprintf(stderr, "device: %s\n", st.message.c_str()); return 1; }
    gpu::SwapChainDesc sd;
    sd.hwnd = hwnd;
    auto swap = gpu::SwapChain::Create(*device, sd, st);
    if (!swap) { std::fprintf(stderr, "swapchain: %s\n", st.message.c_str()); return 1; }
    auto ctx = device->CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, sd.bufferCount);

    ui::UiInitDesc ud;
    ud.hwnd = hwnd;
    ud.device = device->D3D();
    ud.queue = ctx->Queue();
    ud.rtvFormat = sd.format;
    ud.width = swap->Width();
    ud.height = swap->Height();
    ud.framesInFlight = sd.bufferCount;
    ud.assetsDir = FindAssets();
    auto ui = ui::Ui::Create(ud, st);
    if (!ui) { std::fprintf(stderr, "ui: %s\n", st.message.c_str()); return 1; }
    g_ui = ui.get();

    StreamMode mode = StreamMode::Play;
    bool signBusy = false;
    ui::UiCallbacks cb;
    cb.signIn = [&] { signBusy = true; FLUVI_INFO("demo", "signIn"); };
    cb.cancelSignIn = [&] { signBusy = false; FLUVI_INFO("demo", "cancelSignIn"); };
    cb.signOut = [&] { g_screenKey = 0; };
    cb.refreshConsoles = [] { FLUVI_INFO("demo", "refreshConsoles"); };
    cb.wake = [](const std::string& id) { FLUVI_INFO("demo", "wake {}", id); };
    cb.connect = [](const std::string& id) { FLUVI_INFO("demo", "connect {}", id); g_screenKey = 2; };
    cb.cancelConnect = [] { g_screenKey = 1; };
    cb.disconnect = [] { g_screenKey = 1; };
    cb.settingsChanged = [](const AppSettings& s) { FLUVI_INFO("demo", "settings: res {} sharp {:.1f}", int(s.video.resolution), s.video.sharpness); };
    cb.modeChanged = [&](StreamMode m) { mode = m; FLUVI_INFO("demo", "mode {}", int(m)); };
    cb.quit = [] { g_quit = true; };
    ui->SetCallbacks(cb);
    g_screenKey = startScreen;
    // Each screen first shows its loading state (skeletons) for a moment, then the fake data arrives.
    ui::Screen current = ui::Screen::SignIn;
    double screenSince = 0;
    bool settingsSent = false, statsSent = false;

    const int64_t start = time::NowQpc();
    int64_t lastModel = 0;
    uint64_t frames = 0;
    int autoScreen = -1;
    int rc = 0;
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int64_t now = time::NowQpc();
        const double t = time::ElapsedMs(start, now) / 1000.0;

        if (runSeconds > 0) {   // timed run: walk through every screen
            const int s = std::min(4, static_cast<int>(t / (runSeconds / 5.0)));
            if (s != autoScreen) {
                autoScreen = s;
                g_screenKey = s;
            }
        }
        if (g_screenKey >= 0) {
            const auto screen = static_cast<ui::Screen>(g_screenKey);
            g_screenKey = -1;
            ui->ShowScreen(screen);
            current = screen;
            screenSince = time::ElapsedMs(start, time::NowQpc()) / 1000.0;
            ui->SetOverlayVisible(screen == ui::Screen::Stream && runSeconds > 0);
        }
        if (g_resized) {
            g_resized = false;
            RECT c{};
            GetClientRect(hwnd, &c);
            if (c.right > 0 && c.bottom > 0) {
                device->WaitIdle();
                swap->Resize(uint32_t(c.right), uint32_t(c.bottom));
                ui->Resize(swap->Width(), swap->Height(), 1.0f);
            }
        }
        if (time::ElapsedMs(lastModel, now) >= 250) {
            lastModel = now;
            const double loadSeconds = runSeconds > 0 ? runSeconds / 5.0 * 0.4 : 1.5;
            const bool loading = t - screenSince < loadSeconds;
            ui->SetSignIn(FakeSignIn(signBusy || runSeconds > 0, loading && current == ui::Screen::SignIn));
            ui->SetConsoles(FakeConsoles(loading && current == ui::Screen::Consoles));
            ui->SetConnecting(FakeConnecting(t - screenSince, loading && current == ui::Screen::Connecting));
            if (statsSent || (current == ui::Screen::Stream && !loading)) {
                ui->SetStats(FakeStats(t, mode));
                statsSent = true;
            }
            if (!settingsSent && current == ui::Screen::Settings && !loading) {
                ui->SetSettings(AppSettings{});
                settingsSent = true;
            }
        }
        if (IsIconic(hwnd)) { Sleep(10); continue; }
        if (!swap->WaitForNextFrame(1000)) continue;

        ui->Update();
        const uint32_t i = swap->CurrentIndex();
        auto* list = ctx->Begin();
        Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        DrawFakeVideo(list, swap->Rtv(i), swap->Width(), swap->Height(), t);
        ui->Record(list, swap->BackBuffer(i), swap->Rtv(i), i);
        Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ctx->Submit();
        if (Status ps = swap->Present({1, false}); !ps) { std::fprintf(stderr, "present: %s\n", ps.message.c_str()); rc = 2; break; }
        ++frames;
        if (runSeconds > 0 && t >= runSeconds) g_quit = true;
    }
    device->WaitIdle();
    const HRESULT removed = device->CheckRemoved();
    const int errors = ReportDebugLayer(device->D3D());
    g_ui = nullptr;
    ui.reset();
    std::printf("fluvi_ui_demo: %llu frames, device %s, %d D3D12 errors\n", static_cast<unsigned long long>(frames),
                SUCCEEDED(removed) ? "ok" : "REMOVED", errors);
    if (FAILED(removed) || errors > 0) rc = 2;
    return rc;
}
