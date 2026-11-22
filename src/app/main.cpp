// SPDX-License-Identifier: GPL-3.0-only
// fluvi.exe: window thread + render loop + the app logic (sign-in, consoles, connecting, settings, pad navigation).
//
// Threads: main = render (this file), Window = Win32 messages, AppController tasks = network, StreamController =
// session, GamepadReader = pad, libdatachannel's own. Nothing but this thread touches the GPU.
//
// Where the rest plugs in (main session, wave 2 integration):
//   HOOK(decode) / HOOK(chain)  AppHooks.stream (StreamController.h): decoder in media.video, chain in connected/closing.
//   HOOK(video)                 below, in the frame: between the clear and ui->Record().
//   HOOK(present)               swap->Present({...}): the stream wants the present clock, not vsync 1.
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>

#include "App.h"
#include "PadNav.h"
#include "SettingsStore.h"
#include "Window.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/QualityChain.h"
#include "fluvi/gpu/SwapChain.h"
#include "fluvi/ui/Ui.h"
#include "fluvi/xhome/WebRtcSession.h"

using namespace fluvi;

namespace {

std::filesystem::path FindAssets() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (auto dir = std::filesystem::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "assets/ui/theme.rcss")) return dir / "assets/ui";
        if (dir == dir.parent_path()) break;
    }
    return std::filesystem::current_path() / "assets/ui";
}

void Fatal(const std::string& what) {
    FLUVI_ERROR("app", "{}", what);
    const int n = MultiByteToWideChar(CP_UTF8, 0, what.data(), static_cast<int>(what.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, what.data(), static_cast<int>(what.size()), w.data(), n);
    MessageBoxW(nullptr, w.c_str(), L"Fluvi", MB_OK | MB_ICONERROR);
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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FLUVI_INFO("app", "Fluvi 0.1.0");

    // Settings: defaults when the file is missing or corrupt (the corrupt file is overwritten on the next change).
    const std::filesystem::path settingsFile = app::SettingsPath();
    app::LoadedSettings loaded = app::LoadSettings(settingsFile);
    AppSettings settings = loaded.settings;

    app::Window window;
    window.SetFullscreenKey(settings.hotkeys.toggleFullscreen);
    std::string error;
    if (!window.Start(L"Fluvi", 1280, 720, &error)) { Fatal("Não foi possível abrir a janela: " + error); return 1; }

    Status st;
    auto device = gpu::Device::Create({}, st);
    if (!device) { Fatal("Não foi possível iniciar a placa de vídeo (D3D12): " + st.message); return 1; }

    // First run: the quality preset follows the GPU (the user can change it in Configurações; that sets presetDetected too).
    bool saveInitial = !loaded.existed || !loaded.valid;
    if (!settings.video.presetDetected) {
        settings.video.preset = gpu::DetectPreset(gpu::CapsFromDevice(*device));
        settings.video.presetDetected = true;
        saveInitial = true;
        FLUVI_INFO("app", "first run: preset {}", int(settings.video.preset));
    }

    gpu::SwapChainDesc sd;
    sd.hwnd = window.hwnd();
    auto swap = gpu::SwapChain::Create(*device, sd, st);
    if (!swap) { Fatal("Não foi possível criar a tela de apresentação: " + st.message); return 1; }
    auto ctx = device->CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, sd.bufferCount);

    ui::UiInitDesc ud;
    ud.hwnd = window.hwnd();
    ud.device = device->D3D();
    ud.queue = ctx->Queue();
    ud.rtvFormat = sd.format;
    ud.width = swap->Width();
    ud.height = swap->Height();
    ud.framesInFlight = sd.bufferCount;
    ud.assetsDir = FindAssets();
    auto ui = ui::Ui::Create(ud, st);
    if (!ui) { Fatal("Não foi possível carregar a interface: " + st.message); return 1; }
    window.SetUi(ui.get());

    app::AppController app(*ui, settings, loaded.raw, settingsFile, saveInitial);
    // app.SetHooks({...});   // HOOK(decode) / HOOK(chain): the main session fills AppHooks here, before Start().
    app.Start();

    app::PadNav nav;
    const int64_t start = time::NowQpc();
    int rc = 0;
    while (!app.QuitRequested() && !window.TakeCloseRequest()) {
        if (window.TakeFullscreenToggle()) app.ToggleFullscreen();
        if (swap->Fullscreen() != app.DesiredFullscreen()) swap->SetFullscreen(app.DesiredFullscreen());

        const app::WindowSize ws = window.Size();
        if (ws.minimized) { Sleep(20); continue; }
        if (ws.width && ws.height && (ws.width != swap->Width() || ws.height != swap->Height())) {
            device->WaitIdle();
            if (Status rs = swap->Resize(ws.width, ws.height); !rs) FLUVI_WARN("app", "resize: {}", rs.message);
            ui->Resize(swap->Width(), swap->Height(), 1.0f);
        }

        // Controller -> keys for the UI (not while the stream owns the pad), View + Menu -> overlay.
        const double nowMs = time::ElapsedMs(start, time::NowQpc());
        const app::PadNavResult nav_ = nav.Update(app.PadState(), nowMs, app.PadNavEnabled());
        for (const app::NavKey& k : nav_.keys) {
            const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(k.vk, MAPVK_VK_TO_VSC)) << 16;
            const LPARAM lp = 1 | scan | (k.repeat ? (1 << 30) : 0) | (k.down ? 0 : (LPARAM(3) << 30));
            ui->HandleMessage(window.hwnd(), k.down ? WM_KEYDOWN : WM_KEYUP, k.vk, lp);
        }
        if (nav_.toggleOverlay) app.ToggleOverlayFromPad();

        if (!swap->WaitForNextFrame(100)) continue;

        ui->Update();
        const uint32_t i = swap->CurrentIndex();
        auto* list = ctx->Begin();
        Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float bed[4] = {0.027f, 0.078f, 0.098f, 1.0f};   // theme --bed
        list->ClearRenderTargetView(swap->Rtv(i), bed, 0, nullptr);
        // HOOK(video): while app.Streaming(), record decode -> QualityChain::Record -> copy into the back buffer here
        // (the chain leaves its RGBA8 output in UNORDERED_ACCESS; rebind the shader heap afterwards). The UI then draws
        // on top (overlay on Screen::Stream).
        ui->Record(list, swap->BackBuffer(i), swap->Rtv(i), i);
        Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ctx->Submit();
        if (Status ps = swap->Present({1, false}); !ps) {   // HOOK(present): the stream presents on its own clock
            FLUVI_ERROR("app", "present: {}", ps.message);
            if (FAILED(device->CheckRemoved())) { Fatal("A placa de vídeo foi reiniciada pelo Windows. Abra o Fluvi de novo."); rc = 2; }
            break;
        }
    }

    // Order: logic and network first (callbacks into the UI stop), then the GPU objects, then the window.
    app.Shutdown();
    window.SetUi(nullptr);
    device->WaitIdle();
    ui.reset();
    ctx.reset();
    swap.reset();
    device.reset();
    window.Stop();
    xhome::shutdownWebRtc();
    return rc;
}
