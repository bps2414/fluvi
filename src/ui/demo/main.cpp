// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: demo of the six screens with fake data over an animated "video" background.
//
// Window mode (default): keys 1-6 switch screens (Entrar, Consoles, Conectando, Stream, Configurações, Abertura);
// F1 overlay, F2 Jogar/Liso, F3 stats in the corner, F5 reload. Arguments: [seconds] [--screen N]. With seconds, it
// walks through every screen (overlay open on the stream), then exits and reports D3D12 debug-layer errors; exit
// code 2 if there were any or the device was removed.
//
// `--shots <dir>`: no window. Renders every screen and state offscreen at 1280x720, 1920x1080 and 1120x630 and
// writes them as PNG (plus a few in en-US at 1280x720), then exits with the same error report.
#include <windows.h>
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "../UiHelpers.h"
#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/SwapChain.h"
#include "fluvi/ui/Ui.h"

using namespace fluvi;
using Microsoft::WRL::ComPtr;

namespace {

ui::Ui* g_ui = nullptr;
bool g_quit = false, g_resized = false;
int g_screenKey = -1;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp >= '1' && wp <= '6') {
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
    }
    m.busy = busy;
    return m;
}

ui::ConsolesModel FakeConsoles(bool loading, int count = 3, ui::PowerState first = ui::PowerState::On) {
    ui::ConsolesModel m;
    m.refreshing = loading;
    if (loading) return m;   // first refresh: no account, no list yet (skeletons)
    m.accountName = "Remanso Azul";
    const std::vector<ui::ConsoleItem> all = {
        {"F4000A1B2C3D", "Sala", "Series X", first, false},
        {"F4000E5F6A7B", "Quarto", "Series S", ui::PowerState::Standby, false},
        {"F4000C8D9E0F", "Escritório", "One X", ui::PowerState::Off, false},
    };
    m.consoles.assign(all.begin(), all.begin() + std::clamp(count, 0, 3));
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
    s.latencyMs = 14.0 + 3.0 * std::sin(t * 1.3) + (std::fmod(t, 4.0) < 0.3 ? 9.0 : 0.0);
    s.rttMs = 6.0 + std::sin(t);
    s.decodeMs = 2.1;
    s.gpuMs = 1.4;
    s.packetsLost = 0;
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

std::unique_ptr<gpu::Device> CreateDevice() {
    Status st;
    gpu::DeviceDesc dd;
    dd.debugLayer = true;   // needs the Graphics Tools optional feature; fall back without it
    auto device = gpu::Device::Create(dd, st);
    if (!device) {
        std::fprintf(stderr, "debug layer unavailable (%s), running without it\n", st.message.c_str());
        device = gpu::Device::Create({}, st);
    }
    if (!device) std::fprintf(stderr, "device: %s\n", st.message.c_str());
    return device;
}

// ---- --shots: offscreen render + PNG readback ----

bool WritePng(const std::filesystem::path& path, uint32_t w, uint32_t h, uint32_t pitch, const uint8_t* rgba) {
    HRESULT hr = S_OK;
    auto check = [&](HRESULT r, const char* step) {
        if (FAILED(r) && SUCCEEDED(hr)) {
            hr = r;
            std::fprintf(stderr, "png %s: 0x%08lX\n", step, static_cast<unsigned long>(r));
        }
        return SUCCEEDED(hr);
    };
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (!check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "factory") ||
        !check(factory->CreateStream(&stream), "stream") || !check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "file") ||
        !check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "encoder") ||
        !check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "init") || !check(encoder->CreateNewFrame(&frame, nullptr), "frame") ||
        !check(frame->Initialize(nullptr), "frame init") || !check(frame->SetSize(w, h), "size"))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!check(frame->SetPixelFormat(&format), "format") || format != GUID_WICPixelFormat32bppBGRA) return false;
    std::vector<uint8_t> rows(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * pitch;
        uint8_t* dst = &rows[static_cast<size_t>(y) * w * 4];
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;   // the frame is opaque on screen
        }
    }
    return check(frame->WritePixels(h, w * 4, static_cast<UINT>(rows.size()), rows.data()), "pixels") && check(frame->Commit(), "commit") &&
           check(encoder->Commit(), "encoder commit");
}

class Shooter {
public:
    struct Size {
        uint32_t w, h;
    };

    Shooter(gpu::Device& device, std::filesystem::path dir) : device_(device), dir_(std::move(dir)) {}

    bool Init() {
        std::filesystem::create_directories(dir_);
        ctx_ = device_.CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, 3);
        Status st;
        for (const Size& s : sizes_) {
            gpu::TextureDesc td;
            td.width = s.w;
            td.height = s.h;
            td.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.shaderRead = false;
            td.renderTarget = true;
            td.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            td.debugName = L"ui shots target";
            auto t = device_.CreateTexture(td, st);
            if (!t) {
                std::fprintf(stderr, "target: %s\n", st.message.c_str());
                return false;
            }
            targets_.push_back(std::move(t));
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 1920ull * 1088 * 4 + 65536;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_.D3D()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&readback_))))
            return false;

        ui::UiInitDesc ud;
        ud.device = device_.D3D();
        ud.queue = ctx_->Queue();
        ud.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.width = sizes_[0].w;
        ud.height = sizes_[0].h;
        ud.framesInFlight = 3;
        ud.assetsDir = FindAssets();
        i18nDir_ = ud.assetsDir.parent_path() / "i18n";
        if (Status ls; !i18n::Load(i18nDir_, "pt-BR", ls)) std::fprintf(stderr, "language: %s\n", ls.message.c_str());
        ui_ = ui::Ui::Create(ud, st);
        if (!ui_) {
            std::fprintf(stderr, "ui: %s\n", st.message.c_str());
            return false;
        }
        ui::UiCallbacks cb;
        cb.modeChanged = [this](StreamMode m) { mode_ = m; };
        cb.settingsChanged = [this](const AppSettings& s) { settings_ = s; };
        ui_->SetCallbacks(cb);
        start_ = time::NowQpc();
        return true;
    }

    int Run() {
        ui::Ui& ui = *ui_;
        ui.SetWindowChrome({true, false, true});

        ui.SetSplashStatus("");
        ui.ShowScreen(ui::Screen::Splash);
        Hold(1600);
        Shoot("01-splash");

        ui.SetSignIn(FakeSignIn(false, false));
        Show(ui::Screen::SignIn);
        Shoot("02-signin-idle");
        ui.SetSignIn(FakeSignIn(true, true));
        Hold(250);
        Shoot("03-signin-loading");
        ui.SetSignIn(FakeSignIn(true, false));
        Hold(250);
        Shoot("04-signin-code");
        ui::SignInModel expired;
        expired.status = "O código expirou. Escolha Entrar para gerar outro.";
        ui.SetSignIn(expired);
        Hold(250);
        Shoot("05-signin-error");

        ui.SetConsoles(FakeConsoles(true));
        Show(ui::Screen::Consoles);
        Shoot("06-consoles-loading");
        ui.SetConsoles(FakeConsoles(false, 1));
        Hold(500);
        Shoot("07-consoles-one");
        ui.SetConsoles(FakeConsoles(false, 1, ui::PowerState::Standby));
        Hold(250);
        Shoot("08-consoles-one-asleep");
        ui.SetConsoles(FakeConsoles(false, 3));
        Hold(500);
        Shoot("09-consoles-shelf");
        ui.SetConsoles(FakeConsoles(false, 0));
        Hold(250);
        Shoot("10-consoles-empty");
        auto failing = FakeConsoles(false, 2);
        failing.refreshing = true;
        failing.error = "Não foi possível falar com o serviço. Confira a internet e escolha Atualizar.";
        ui.SetConsoles(failing);
        Hold(250);
        Shoot("11-consoles-error");
        ui.SetConsoles(FakeConsoles(false, 3));
        ui.SetWindowChrome({false, false, true});
        Hold(250);
        Shoot("12-consoles-fullscreen");
        ui.SetWindowChrome({true, false, true});

        ui.SetConnecting(FakeConnecting(0, true));
        Show(ui::Screen::Connecting);
        Shoot("13-connecting-loading");
        ui.SetConnecting(FakeConnecting(4.5, false));
        Hold(700);
        Shoot("14-connecting");
        ui.SetConnecting(FakeConnecting(9, false));
        Hold(400);
        Shoot("15-connecting-failed");

        Show(ui::Screen::Settings);
        Shoot("16-settings-loading");
        settings_.video.preset = QualityPreset::Max;
        settings_.video.bitrateKbps = 30000;
        ui.SetSettings(settings_);
        Hold(500);
        Shoot("17-settings-video");
        Key(VK_ESCAPE);   // from the rows to the nav
        const char* sections[] = {"18-settings-smooth", "19-settings-controller", "20-settings-audio", "21-settings-keys",
                                  "22-settings-stats", "23-settings-about"};
        for (const char* name : sections) {
            Key(VK_DOWN);
            Hold(450);
            Shoot(name);
        }

        // The game: the toast on arrival, then enough stats for the latency graph.
        ui.SetConnecting({"Sala", ui::ConnectStage::Connected, ""});
        Show(ui::Screen::Connecting);
        ui.SetWindowChrome({false, false, true});
        streaming_ = true;
        ui.ShowScreen(ui::Screen::Stream);
        Settle();
        Hold(300);
        Shoot("24-stream-arrival");
        Hold(6000);
        ui.SetOverlayVisible(true);
        Hold(500);
        Shoot("25-stream-overlay");
        Key(VK_F2);
        Hold(400);
        Shoot("26-stream-overlay-smooth");
        Key(VK_F2);
        ui.SetOverlayVisible(false);
        settings_.hud.mode = HudMode::Compact;
        settings_.hud.corner = HudCorner::TopLeft;
        ui.SetSettings(settings_);
        Hold(2200);   // past the mode toast
        Shoot("27-stream-hud-compact");
        settings_.hud.mode = HudMode::Full;
        settings_.hud.corner = HudCorner::BottomRight;
        ui.SetSettings(settings_);
        Hold(300);
        Shoot("28-stream-hud-full");

        // Quitting with a game running asks first (from settings opened over the game).
        ui.SetWindowChrome({true, false, true});
        Show(ui::Screen::Settings);
        const float dp = ui::DpRatio(sizes_[cur_].w, sizes_[cur_].h, 1.0f);
        Click(static_cast<int>(sizes_[cur_].w - 26 * dp), static_cast<int>(20 * dp));
        Hold(400);
        Shoot("29-quit-dialog");
        Key(VK_ESCAPE);
        streaming_ = false;

        // English, once per screen family.
        if (Status ls; !i18n::Load(i18nDir_, "en-US", ls)) std::fprintf(stderr, "language: %s\n", ls.message.c_str());
        ui.LanguageChanged();
        ui.SetSignIn(FakeSignIn(true, false));
        Show(ui::Screen::SignIn);
        Shoot("30-en-signin", true);
        ui.SetConsoles(FakeConsoles(false, 3));
        Show(ui::Screen::Consoles);
        Shoot("31-en-consoles", true);
        ui.SetConnecting(FakeConnecting(9, false));
        Show(ui::Screen::Connecting);
        Shoot("32-en-connecting-failed", true);
        Show(ui::Screen::Settings);
        Key(VK_ESCAPE);
        Hold(300);
        Shoot("33-en-settings", true);
        streaming_ = true;
        ui.SetWindowChrome({false, false, true});
        ui.ShowScreen(ui::Screen::Stream);
        Settle();
        ui.SetOverlayVisible(true);
        Hold(2500);
        Shoot("34-en-stream-overlay", true);
        streaming_ = false;

        device_.WaitIdle();
        std::printf("fluvi_ui_demo: %d shots in %s\n", written_, dir_.string().c_str());
        return failed_ ? 1 : 0;
    }

    void Shutdown() {
        device_.WaitIdle();
        ui_.reset();
    }

private:
    double Seconds() const { return time::ElapsedMs(start_, time::NowQpc()) / 1000.0; }

    void Frame() {
        const double t = Seconds();
        if (streaming_ && t - lastStats_ >= 0.1) {
            lastStats_ = t;
            ui_->SetStats(FakeStats(t, mode_));
        }
        ui_->Update();
        gpu::Texture& target = *targets_[cur_];
        auto* list = ctx_->Begin();
        DrawFakeVideo(list, target.Rtv(), sizes_[cur_].w, sizes_[cur_].h, 2.0);
        ui_->Record(list, target.View().resource, target.Rtv(), frame_++ % 3);
        ctx_->Wait(ctx_->Submit());
    }

    void Hold(int ms) {
        const double until = Seconds() + ms / 1000.0;
        do {
            Frame();
            Sleep(8);
        } while (Seconds() < until);
    }

    void Settle() {
        for (int i = 0; i < 600 && ui_->InTransition(); ++i) {
            Frame();
            Sleep(5);
        }
    }

    // Shows a screen and waits for its arrival to finish playing.
    void Show(ui::Screen s) {
        ui_->ShowScreen(s);
        Settle();
        Hold(650);
    }

    void Key(WPARAM vk) {
        ui_->HandleMessage(nullptr, WM_KEYDOWN, vk, 0);
        ui_->HandleMessage(nullptr, WM_KEYUP, vk, LPARAM(0xC0000000));
        Hold(60);
    }

    void Click(int x, int y) {
        const LPARAM at = MAKELPARAM(x, y);
        ui_->HandleMessage(nullptr, WM_MOUSEMOVE, 0, at);
        ui_->HandleMessage(nullptr, WM_LBUTTONDOWN, MK_LBUTTON, at);
        ui_->HandleMessage(nullptr, WM_LBUTTONUP, 0, at);
        Hold(60);
        ui_->HandleMessage(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(1, 300));   // move away: no hover in the shot
    }

    void Use(size_t i) {
        if (i == cur_) return;
        cur_ = i;
        ui_->Resize(sizes_[i].w, sizes_[i].h, 1.0f);
        Hold(120);
    }

    void Shoot(const std::string& name, bool firstSizeOnly = false) {
        const size_t back = cur_;
        for (size_t i = 0; i < (firstSizeOnly ? 1 : sizes_.size()); ++i) {
            Use(i);
            Frame();
            Save(std::format("{}-{}x{}.png", name, sizes_[i].w, sizes_[i].h));
        }
        Use(back);
    }

    void Save(const std::string& file) {
        gpu::Texture& target = *targets_[cur_];
        ID3D12Resource* res = target.View().resource;
        const D3D12_RESOURCE_DESC desc = res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        device_.D3D()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        auto* list = ctx_->Begin();
        Transition(list, res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{readback_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ctx_->Wait(ctx_->Submit());
        void* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(fp.Footprint.RowPitch) * fp.Footprint.Height};
        if (FAILED(readback_->Map(0, &range, &data))) {
            failed_ = true;
            return;
        }
        const bool ok = WritePng(dir_ / file, fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch, static_cast<const uint8_t*>(data));
        const D3D12_RANGE none{0, 0};
        readback_->Unmap(0, &none);
        if (ok) ++written_;
        else {
            failed_ = true;
            std::fprintf(stderr, "could not write %s\n", file.c_str());
        }
    }

    gpu::Device& device_;
    std::filesystem::path dir_, i18nDir_;
    std::unique_ptr<gpu::CommandContext> ctx_;
    std::vector<std::unique_ptr<gpu::Texture>> targets_;
    ComPtr<ID3D12Resource> readback_;
    std::unique_ptr<ui::Ui> ui_;
    const std::vector<Size> sizes_ = {{1280, 720}, {1920, 1080}, {1120, 630}};
    size_t cur_ = 0;
    uint32_t frame_ = 0;
    int64_t start_ = 0;
    double lastStats_ = -1.0;
    bool streaming_ = false, failed_ = false;
    int written_ = 0;
    StreamMode mode_ = StreamMode::Play;
    AppSettings settings_;
};

int RunShots(const std::filesystem::path& dir) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    auto device = CreateDevice();
    if (!device) return 1;
    int rc = 1;
    {
        Shooter shooter(*device, dir);
        if (shooter.Init()) rc = shooter.Run();
        shooter.Shutdown();
    }
    const HRESULT removed = device->CheckRemoved();
    const int errors = ReportDebugLayer(device->D3D());
    std::printf("fluvi_ui_demo: device %s, %d D3D12 errors\n", SUCCEEDED(removed) ? "ok" : "REMOVED", errors);
    if (FAILED(removed) || errors > 0) rc = 2;
    CoUninitialize();
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    double runSeconds = 0;
    int startScreen = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shots") == 0 && i + 1 < argc) return RunShots(argv[++i]);
        if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) startScreen = std::clamp(std::atoi(argv[++i]) - 1, 0, 5);
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"fluvi_ui_demo  (1-6 telas, F1 overlay, F2 modo, F3 canto, F5 recarrega)",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    Status st;
    auto device = CreateDevice();
    if (!device) return 1;
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
    // The demo window keeps the system frame, so the UI's own title bar stays hidden.
    ui->SetWindowChrome({false, false, true});

    StreamMode mode = StreamMode::Play;
    bool signBusy = false;
    ui::UiCallbacks cb;
    cb.signIn = [&] { signBusy = true; FLUVI_INFO("demo", "signIn"); };
    cb.cancelSignIn = [&] { signBusy = false; FLUVI_INFO("demo", "cancelSignIn"); };
    cb.signOut = [&] { g_screenKey = 0; };
    cb.refreshConsoles = [] { FLUVI_INFO("demo", "refreshConsoles"); };
    cb.wake = [](const std::string& id) { FLUVI_INFO("demo", "wake {}", id); };
    cb.connect = [](const std::string& id) { FLUVI_INFO("demo", "connect {}", id); g_screenKey = 2; };
    cb.wakeAndConnect = [](const std::string& id) { FLUVI_INFO("demo", "wakeAndConnect {}", id); g_screenKey = 2; };
    cb.cancelConnect = [] { g_screenKey = 1; };
    cb.disconnect = [] { g_screenKey = 1; };
    cb.settingsChanged = [&](const AppSettings& s) { FLUVI_INFO("demo", "settings: res {} hud {}", int(s.video.resolution), int(s.hud.mode)); ui->SetSettings(s); };
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
            const int s = std::min(5, static_cast<int>(t / (runSeconds / 6.0)));
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
                ui->Resize(swap->Width(), swap->Height(), static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            }
        }
        if (time::ElapsedMs(lastModel, now) >= 250) {
            lastModel = now;
            const double loadSeconds = runSeconds > 0 ? runSeconds / 6.0 * 0.4 : 1.5;
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
