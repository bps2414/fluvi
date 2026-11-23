// SPDX-License-Identifier: GPL-3.0-only
// fluvi.exe: window thread + render loop + the app logic (sign-in, consoles, connecting, settings, pad navigation).
//
// Threads: main = render (this file), Window = Win32 messages, AppController tasks = network, StreamController =
// session, GamepadReader = pad, libdatachannel's own. Nothing but this thread touches the GPU.
//
// Stream video (wave 2 integration, main session):
//   session AUs -> media::VideoDecoder (own thread, NV12 + isNew) -> render thread: NewFrameGate -> QualityChain
//   (NV12 -> RGB -> deblock -> downscale / 1:1 / FSR 4 or EASU -> RCAS, into `out` at the window size) -> copy into
//   the back buffer -> UI overlay on top -> Present now (tearing / VRR) when a picture was processed, vsync 1 when
//   only the UI changed. Nothing is presented while streaming if neither changed (the window follows the content).
//   Audio: session Opus packets -> audio::OpusPlayer (WASAPI). Smooth mode (FG) and the present clock are wave 3.
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>

#include "App.h"
#include "fluvi/audio/OpusPlayer.h"
#include "fluvi/common/Stats.h"
#include "fluvi/media/SessionVideo.h"
#include "PadNav.h"
#include "SettingsStore.h"
#include "Window.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/FrameGen.h"
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
    // Latency: 1 ms timer resolution (waits and the Liso pacing wake on time) and the render thread ahead of the rest.
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
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

    // ---- stream: decoder, audio, quality chain ----
    // The decoder lives as long as the app (Reset per stream); the session it asks keyframes from is the live one.
    std::mutex sessionMutex;                  // guards liveSession against the session being closed
    xhome::WebRtcSession* liveSession = nullptr;
    std::atomic<bool> feeding{false};
    std::atomic<bool> streamReset{false};
    std::atomic<StreamMode> mode{settings.video.mode};
    std::mutex pendingMutex;
    std::optional<AppSettings> pendingSettings;

    media::VideoDecoderDesc vd;
    vd.device = device.get();
    vd.requestKeyframe = [&] {
        std::lock_guard lock(sessionMutex);
        if (liveSession) liveSession->requestKeyframe();
    };
    auto decoder = media::VideoDecoder::Create(vd, st);
    if (!decoder) FLUVI_ERROR("app", "video decoder unavailable: {}", st.message);

    audio::OpusPlayerConfig ac;
    ac.deviceId = settings.audio.deviceId;
    ac.volume = settings.audio.volume;
    auto audioOut = audio::OpusPlayer::Create(ac, st);
    if (!audioOut) FLUVI_WARN("app", "audio output unavailable: {}", st.message);

    gpu::DeviceTextureAllocator allocator(*device);
    const gpu::Fsr4Tier fsr4Tier = gpu::ClassifyFsr4(gpu::CapsFromDevice(*device));
    gpu::ChainOptions co;
    co.config = gpu::ResolveChainConfig(settings.video, fsr4Tier);
    auto chain = gpu::QualityChain::Create(device->D3D(), allocator, co, st);
    if (!chain) FLUVI_ERROR("app", "quality chain unavailable: {}", st.message);
    gpu::NewFrameGate gate(settings.video.newFramesOnly && settings.video.mode == StreamMode::Play);
    FLUVI_INFO("app", "FSR 4 tier: {}", gpu::TierName(fsr4Tier));

    // The chain's output, at the window size (recreated on resize, after the GPU is idle).
    std::unique_ptr<gpu::Texture> out;
    auto makeOut = [&] {
        out.reset();
        gpu::TextureDesc td;
        td.width = swap->Width();
        td.height = swap->Height();
        td.format = gpu::kOutputFormat;
        td.shaderRead = false;
        td.unorderedAccess = true;
        td.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        td.debugName = L"fluvi stream out";
        Status ts;
        out = device->CreateTexture(td, ts);
        if (!out) FLUVI_ERROR("app", "stream output texture: {}", ts.message);
    };
    makeOut();

    app::AppHooks hooks;
    hooks.stream.media.video = [&](std::vector<uint8_t>&& unit, uint32_t rtpTimestamp, int64_t arrivalQpc) {
        if (decoder && feeding.load()) decoder->Push(std::move(unit), rtpTimestamp, arrivalQpc);
    };
    hooks.stream.media.audio = [&](const uint8_t* data, size_t size, uint32_t rtpTimestamp) {
        if (audioOut && feeding.load()) audioOut->Push(data, size, rtpTimestamp);
    };
    hooks.stream.connected = [&](xhome::WebRtcSession& session, const xhome::StreamConfig&) {
        if (decoder) decoder->Reset();
        if (audioOut) audioOut->Flush();
        {
            std::lock_guard lock(sessionMutex);
            liveSession = &session;
        }
        streamReset = true;
        feeding = true;
    };
    hooks.stream.closing = [&] {
        feeding = false;
        std::lock_guard lock(sessionMutex);
        liveSession = nullptr;
    };
    hooks.modeChanged = [&](StreamMode m) { mode = m; };
    hooks.settingsApplied = [&](const AppSettings& s) {
        std::lock_guard lock(pendingMutex);
        pendingSettings = s;
    };

    app::AppController app(*ui, settings, loaded.raw, settingsFile, saveInitial);
    app.SetHooks(std::move(hooks));
    app.Start();

    media::DecodedFramePtr shown;      // the picture `out` holds (kept until the GPU is done with it)
    bool haveOutput = false;           // `out` holds a processed picture for the current stream and size
    bool wasStreaming = false;
    int64_t lastStatsQpc = 0;
    uint64_t presentsSinceStats = 0;

    // ---- Liso (FG): FSR 3.1 interpolation at the window size, created on first use ----
    // A new real frame N: the chain writes N into `out`, FG writes the N-1/N in-between into `fgOut`, the in-between
    // is presented at once and N half a content interval later (or right away if N+1 arrives first).
    std::unique_ptr<gpu::FrameGenerator> fg;
    std::unique_ptr<gpu::Texture> fgOut;
    bool fgFailed = false;
    std::string fgError;
    bool fgReset = true;
    bool realPending = false;
    int64_t realDueQpc = 0;
    media::DecodedFramePtr stash;      // a new frame that arrived while the real one was still pending
    StreamMode lastMode = mode.load();
    auto dropFg = [&] {
        if (!fg && !fgOut) return;
        device->WaitIdle();
        fg.reset();
        fgOut.reset();
    };
    int statsTicks = 0;
    int64_t lastPresentQpc = 0;

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
            makeOut();
            haveOutput = false;
            dropFg();                  // recreated at the new size below
            fgReset = true;
            realPending = false;
        }

        // Settings and mode changes from the app (any thread) -> the chain, the gate and the audio, here.
        std::optional<AppSettings> changed;
        {
            std::lock_guard lock(pendingMutex);
            changed.swap(pendingSettings);
        }
        if (changed) {
            settings = *changed;
            if (chain) chain->SetConfig(gpu::ResolveChainConfig(settings.video, fsr4Tier));
            if (audioOut) audioOut->SetVolume(settings.audio.volume);
        }
        gate.SetNewFramesOnly(settings.video.newFramesOnly && mode.load() == StreamMode::Play);

        // Controller -> keys for the UI (not while the stream owns the pad), View + Menu -> overlay.
        const double nowMs = time::ElapsedMs(start, time::NowQpc());
        const app::PadNavResult nav_ = nav.Update(app.PadState(), nowMs, app.PadNavEnabled());
        for (const app::NavKey& k : nav_.keys) {
            const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(k.vk, MAPVK_VK_TO_VSC)) << 16;
            const LPARAM lp = 1 | scan | (k.repeat ? (1 << 30) : 0) | (k.down ? 0 : (LPARAM(3) << 30));
            ui->HandleMessage(window.hwnd(), k.down ? WM_KEYDOWN : WM_KEYUP, k.vk, lp);
        }
        if (nav_.toggleOverlay) app.ToggleOverlayFromPad();

        // ---- stream picture: the newest decoded frame, if any ----
        const bool streaming = app.Streaming() && decoder && chain && out;
        if (streaming != wasStreaming || streamReset.exchange(false)) {
            haveOutput = false;
            shown.reset();
            if (chain) chain->ResetHistory();
            wasStreaming = streaming;
            stash.reset();
            fgReset = true;
            realPending = false;
        }

        // Liso: Jogar <-> Liso resets the pacing; the generator is made once per size (seconds the first time).
        const bool smoothWanted = mode.load() == StreamMode::Smooth;
        if (mode.load() != lastMode) {
            lastMode = mode.load();
            fgReset = true;
            realPending = false;
        }
        if (fgFailed && (fg || fgOut)) dropFg();
        if (streaming && smoothWanted && !fgFailed && (!fg || fg->Width() != swap->Width() || fg->Height() != swap->Height())) {
            dropFg();
            Status fs;
            fg = gpu::FrameGenerator::Create(device->D3D(), swap->Width(), swap->Height(), gpu::kOutputFormat, fs);
            if (fg) {
                gpu::TextureDesc td;
                td.width = swap->Width();
                td.height = swap->Height();
                td.format = gpu::kOutputFormat;
                td.shaderRead = false;
                td.unorderedAccess = true;
                td.initialState = D3D12_RESOURCE_STATE_COMMON;
                td.debugName = L"fluvi fg out";
                fgOut = device->CreateTexture(td, fs);
            }
            if (!fg || !fgOut) {
                fgFailed = true;
                fgError = fs.message;
                FLUVI_ERROR("fg", "modo Liso indisponível: {}", fs.message);
                dropFg();
            }
            fgReset = true;
        }
        const bool smooth = streaming && smoothWanted && fg && fgOut && !fgFailed;

        media::DecodedFramePtr fresh = std::move(stash);
        gpu::FrameDecision decision{false, false};
        bool presentReal = false;
        if (streaming) {
            while (auto f = decoder->TryPop()) {
                if (smooth && !f->isNew && haveOutput) continue;   // Liso: a repeat carries nothing new
                fresh = std::move(f);
            }
            const bool uiAnimating = ui->OverlayVisible() || ui->CurrentScreen() != ui::Screen::Stream;
            if (smooth && realPending) {
                // The real frame is due half a content interval after the in-between one; a new frame cuts the wait.
                while (!fresh) {
                    const double left = time::ElapsedMs(time::NowQpc(), realDueQpc);
                    if (left <= 0.25) break;
                    if (left > 1.5) {
                        if (auto f = decoder->WaitPop(static_cast<uint32_t>(std::min(left - 1.0, 8.0))); f && f->isNew)
                            fresh = std::move(f);
                    } else {
                        Sleep(0);
                    }
                }
                presentReal = true;
                stash = std::move(fresh);   // processed on the next pass, after the real frame is out
                fresh = nullptr;
            } else {
                // A still picture (a menu: every frame a repeat) must not starve the UI: F1/F2 and the corner notice
                // are handled in ui->Update(), so a UI-only frame goes out at least every 50 ms.
                const bool uiDue = time::ElapsedMs(lastPresentQpc, time::NowQpc()) > 50.0;
                if (!fresh && !uiAnimating && haveOutput && !uiDue) {
                    // Nothing to show: wait for the decoder instead of spinning (the loop still services the pad).
                    fresh = decoder->WaitPop(8);
                    if (!fresh) continue;
                }
                if (fresh && smooth && !fresh->isNew && haveOutput) fresh = nullptr;   // Liso: a repeat carries nothing new
                if (fresh) decision = gate.OnFrame(fresh->isNew, nowMs, !haveOutput);
                if (!decision.process && !uiAnimating && haveOutput && !uiDue) continue;   // a repeat, "only new frames" on
            }
        }

        if (!swap->WaitForNextFrame(100)) continue;

        ui->Update();
        const uint32_t i = swap->CurrentIndex();
        auto* list = ctx->Begin();
        bool processed = false;
        bool showGenerated = false;
        float contentMs = 33.333f;
        if (streaming && fresh && decision.process) {
            if (Status cs = chain->Configure(fresh->width, fresh->height, swap->Width(), swap->Height()); !cs)
                FLUVI_WARN("app", "chain configure: {}", cs.message);
            fresh->WaitOnQueue(ctx->Queue());
            ID3D12DescriptorHeap* heap = allocator.ShaderHeap();
            list->SetDescriptorHeaps(1, &heap);
            gpu::Nv12Params np;
            np.matrix = fresh->bt601 ? gpu::YuvMatrix::Bt601 : gpu::YuvMatrix::Bt709;
            np.fullRange = fresh->fullRange;
            np.visibleWidth = fresh->width;
            np.visibleHeight = fresh->height;
            processed = chain->Record(list, fresh->texture, np, out->View());
            haveOutput = haveOutput || processed;
            if (processed && smooth) {
                if (fresh->contentDuration90k) contentMs = std::clamp(fresh->contentDuration90k / 90.0f, 8.0f, 50.0f);
                Transition(list, out->View().resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                const bool wasReset = fgReset;
                if (fg->Generate(list, out->View().resource, fgOut->View().resource, fgReset, contentMs)) {
                    showGenerated = !wasReset;   // after a reset the output is not an in-between frame
                    fgReset = false;
                } else {
                    fgFailed = true;
                    fgError = "o FSR FG falhou (ver fluvi.log)";
                }
                Transition(list, out->View().resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        if (streaming && haveOutput) {
            // The chain's output (same size and format as the back buffer, bars included) becomes the frame; in Liso
            // the in-between frame goes first.
            ID3D12Resource* src = showGenerated ? fgOut->View().resource : out->View().resource;
            const D3D12_RESOURCE_STATES srcState = showGenerated ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            Transition(list, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(swap->BackBuffer(i), src);
            Transition(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
            Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else {
            Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float bed[4] = {0.027f, 0.078f, 0.098f, 1.0f};   // theme --bed
            list->ClearRenderTargetView(swap->Rtv(i), bed, 0, nullptr);
        }
        ui->Record(list, swap->BackBuffer(i), swap->Rtv(i), i);
        Transition(list, swap->BackBuffer(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        const uint64_t submitted = ctx->Submit();
        if (processed) {
            fresh->ReleaseAfter(ctx->Fence(), submitted);
            if (shown) shown->ReleaseAfter(ctx->Fence(), submitted);
            shown = fresh;
        }
        // A new picture goes out at once (tearing / VRR); UI-only frames keep vsync. Wave 3 adds the present clock.
        const gpu::PresentRequest pr = (processed || presentReal) ? gpu::PresentRequest{0, true} : gpu::PresentRequest{1, false};
        if (Status ps = swap->Present(pr); !ps) {
            FLUVI_ERROR("app", "present: {}", ps.message);
            if (FAILED(device->CheckRemoved())) { Fatal("A placa de vídeo foi reiniciada pelo Windows. Abra o Fluvi de novo."); rc = 2; }
            break;
        }
        lastPresentQpc = time::NowQpc();
        if (showGenerated) {
            realPending = true;
            realDueQpc = time::NowQpc() + time::MsToQpc(contentMs * 0.5);
        } else if (presentReal || processed) {
            realPending = false;
        }
        ++presentsSinceStats;
        if (processed) {
            std::lock_guard lock(sessionMutex);
            if (liveSession) media::ReportPresented(*liveSession, *fresh, time::NowQpc());
        }

        // Overlay numbers, twice a second while streaming.
        const int64_t nowQpc = time::NowQpc();
        const double sinceStats = time::ElapsedMs(lastStatsQpc, nowQpc);
        if (streaming && sinceStats >= 500) {
            OverlayStats os;
            decoder->FillOverlayStats(os);
            os.mode = mode.load();
            os.outputWidth = swap->Width();
            os.outputHeight = swap->Height();
            os.presentedFps = presentsSinceStats * 1000.0 / sinceStats;
            const gpu::PresentStats ps = swap->Stats();
            const gpu::DisplayInfo di = swap->Display();
            os.presentMode = ps.mode;
            os.refreshHz = di.refreshHz;
            os.vrr = di.vrrEnabled;
            os.tearing = di.tearingSupported;
            if (shown) os.latencyMs = time::ElapsedMs(shown->arrivalQpc, ps.lastPresentQpc ? ps.lastPresentQpc : nowQpc);
            {
                std::lock_guard lock(sessionMutex);
                if (liveSession) os.rttMs = liveSession->stats().rttMs;
            }
            if (os.warning.empty() && !chain->Fsr4Fallback().empty()) os.warning = "FSR 4 indisponível, usando EASU";
            if (smoothWanted && fgFailed) os.warning = "Liso indisponível: " + fgError;
            ui->SetStats(os);
            // Latency in the log every ~5 s (the overlay shows the same): where the time goes, for field tests.
            if (shown && ++statsTicks % 10 == 0) {
                const int64_t presented = ps.lastPresentQpc ? ps.lastPresentQpc : nowQpc;
                FLUVI_INFO("latency", "chegada->decode {:.1f} ms, decode->tela {:.1f} ms, chegada->tela {:.1f} ms | tela {:.0f} fps, "
                           "conteúdo {:.0f}/stream {:.0f} fps, modo {} ({}), rtt {:.0f} ms, perdidos {}",
                           time::ElapsedMs(shown->arrivalQpc, shown->decodedQpc), time::ElapsedMs(shown->decodedQpc, presented),
                           time::ElapsedMs(shown->arrivalQpc, presented), os.presentedFps, os.gameFps, os.streamFps,
                           smooth ? "Liso" : "Jogar", static_cast<int>(os.presentMode), os.rttMs, os.framesDropped);
            }
            lastStatsQpc = nowQpc;
            presentsSinceStats = 0;
        }
    }

    // Order: logic and network first (callbacks into the UI stop), then the GPU objects, then the window.
    app.Shutdown();
    window.SetUi(nullptr);
    device->WaitIdle();
    fg.reset();
    fgOut.reset();
    stash.reset();
    shown.reset();
    decoder.reset();
    chain.reset();
    out.reset();
    audioOut.reset();
    ui.reset();
    ctx.reset();
    swap.reset();
    device.reset();
    window.Stop();
    xhome::shutdownWebRtc();
    timeEndPeriod(1);
    return rc;
}
