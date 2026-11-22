// SPDX-License-Identifier: GPL-3.0-only
// ui on real hardware (label gpu): every screen records over an offscreen target, input reaches the settings rows and
// the overlay hotkeys, and the device survives.
#include <doctest/doctest.h>

#include <windows.h>

#include <filesystem>
#include <optional>

#include "fluvi/gpu/Device.h"
#include "fluvi/ui/Ui.h"

using namespace fluvi;

namespace {

struct Fixture {
    std::unique_ptr<gpu::Device> device;
    std::unique_ptr<gpu::CommandContext> ctx;
    std::unique_ptr<gpu::Texture> target;
    std::unique_ptr<ui::Ui> ui;
    uint32_t frame = 0;

    Fixture() {
        Status st;
        gpu::DeviceDesc dd;
        dd.debugLayer = true;   // needs the Graphics Tools optional feature; fall back without it
        device = gpu::Device::Create(dd, st);
        if (!device) device = gpu::Device::Create({}, st);
        REQUIRE_MESSAGE(device, st.message);
        ctx = device->CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, 3);
        gpu::TextureDesc td;
        td.width = 1280;
        td.height = 720;
        td.format = gpu::kOutputFormat;
        td.shaderRead = false;
        td.renderTarget = true;
        td.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        td.debugName = L"ui test target";
        target = device->CreateTexture(td, st);
        REQUIRE_MESSAGE(target, st.message);

        ui::UiInitDesc ud;
        ud.device = device->D3D();
        ud.queue = ctx->Queue();
        ud.rtvFormat = gpu::kOutputFormat;
        ud.width = td.width;
        ud.height = td.height;
        ud.framesInFlight = 3;
        ud.assetsDir = std::filesystem::current_path() / "assets/ui";
        ui = ui::Ui::Create(ud, st);
        REQUIRE_MESSAGE(ui, st.message);
    }
    ~Fixture() {
        if (device) device->WaitIdle();
        ui.reset();
    }

    void Frames(int n) {
        for (int i = 0; i < n; ++i) {
            ui->Update();
            auto* list = ctx->Begin();
            const float clear[4] = {0.2f, 0.3f, 0.4f, 1};
            list->ClearRenderTargetView(target->Rtv(), clear, 0, nullptr);
            ui->Record(list, target->View().resource, target->Rtv(), frame++ % 3);
            ctx->Wait(ctx->Submit());
        }
    }
    void Key(WPARAM vk) {
        CHECK(ui->HandleMessage(nullptr, WM_KEYDOWN, vk, 0));
        ui->HandleMessage(nullptr, WM_KEYUP, vk, LPARAM(0xC0000000));
    }
};

} // namespace

TEST_CASE("every screen renders and the device survives") {
    Fixture f;
    ui::SignInModel sign;
    sign.userCode = "B7KQ-M4XZ";
    sign.verificationUri = "https://www.microsoft.com/link";
    sign.busy = true;
    f.ui->SetSignIn(sign);
    ui::ConsolesModel cons;
    cons.accountName = "Remanso";
    cons.consoles = {{"a", "Sala", "Xbox Series X", ui::PowerState::On, false}, {"b", "Quarto", "Xbox Series S", ui::PowerState::Off, true}};
    f.ui->SetConsoles(cons);
    f.ui->SetConnecting({"Sala", ui::ConnectStage::Connecting, ""});
    OverlayStats stats;
    stats.latencyMs = 12;
    stats.vrr = true;
    f.ui->SetStats(stats);
    f.ui->SetSettings(AppSettings{});

    for (auto s : {ui::Screen::SignIn, ui::Screen::Consoles, ui::Screen::Connecting, ui::Screen::Stream, ui::Screen::Settings}) {
        f.ui->ShowScreen(s);
        f.ui->SetOverlayVisible(s == ui::Screen::Stream);
        f.Frames(8);
        CHECK(f.ui->CurrentScreen() == s);
    }
    f.ui->ReloadDocuments();
    f.Frames(4);
    CHECK(SUCCEEDED(f.device->CheckRemoved()));
}

TEST_CASE("Right on the focused settings row steps the value and reports it") {
    Fixture f;
    std::optional<AppSettings> changed;
    ui::UiCallbacks cb;
    cb.settingsChanged = [&](const AppSettings& s) { changed = s; };
    f.ui->SetCallbacks(cb);
    AppSettings s;
    s.video.resolution = StreamResolution::R1080p;
    f.ui->SetSettings(s);
    f.ui->ShowScreen(ui::Screen::Settings);
    CHECK(f.ui->WantsInput());
    f.Frames(2);   // resolution row has autofocus
    f.Key(VK_RIGHT);
    f.Frames(2);
    REQUIRE(changed);
    CHECK(changed->video.resolution == StreamResolution::R1440p);
    f.Key(VK_RIGHT);   // clamps at the last choice
    f.Frames(2);
    CHECK(changed->video.resolution == StreamResolution::R1440p);
    f.Key(VK_LEFT);
    f.Frames(2);
    CHECK(changed->video.resolution == StreamResolution::R1080p);
    f.Key(VK_ESCAPE);
    f.Frames(1);
    CHECK(f.ui->CurrentScreen() == ui::Screen::SignIn);   // back to where settings was opened from
}

TEST_CASE("stream hotkeys toggle the overlay and the mode; input goes to the game when it is closed") {
    Fixture f;
    std::optional<StreamMode> mode;
    ui::UiCallbacks cb;
    cb.modeChanged = [&](StreamMode m) { mode = m; };
    f.ui->SetCallbacks(cb);
    f.ui->SetSettings(AppSettings{});
    f.ui->ShowScreen(ui::Screen::Stream);
    f.Frames(1);
    CHECK_FALSE(f.ui->WantsInput());
    CHECK_FALSE(f.ui->HandleMessage(nullptr, WM_KEYDOWN, 'W', 0));
    f.Key(VK_F1);
    f.Frames(2);
    CHECK(f.ui->OverlayVisible());
    CHECK(f.ui->WantsInput());
    f.Key(VK_F2);
    f.Frames(1);
    REQUIRE(mode);
    CHECK(*mode == StreamMode::Smooth);
    f.Key(VK_ESCAPE);
    f.Frames(1);
    CHECK_FALSE(f.ui->OverlayVisible());
    f.Frames(30);   // idle: Record() skips after the fade
    CHECK(SUCCEEDED(f.device->CheckRemoved()));
}

TEST_CASE("loading states (skeletons) render on every screen before any data arrives") {
    Fixture f;
    ui::SignInModel sign;
    sign.busy = true;   // code and QR requested, not here yet
    f.ui->SetSignIn(sign);
    ui::ConsolesModel cons;
    cons.refreshing = true;   // first refresh, empty list, no account
    f.ui->SetConsoles(cons);
    for (auto s : {ui::Screen::SignIn, ui::Screen::Consoles, ui::Screen::Connecting, ui::Screen::Stream, ui::Screen::Settings}) {
        f.ui->ShowScreen(s);
        f.ui->SetOverlayVisible(s == ui::Screen::Stream);
        f.Frames(6);
    }
    // Data arriving later replaces the placeholders; settings become editable once loaded.
    std::optional<AppSettings> changed;
    ui::UiCallbacks cb;
    cb.settingsChanged = [&](const AppSettings& s) { changed = s; };
    f.ui->SetCallbacks(cb);
    f.ui->SetSettings(AppSettings{});
    f.Frames(3);
    f.Key(VK_LEFT);   // resolution row gets focus once it exists
    f.Frames(2);
    REQUIRE(changed);
    CHECK(changed->video.resolution == StreamResolution::R1080p);
    CHECK(SUCCEEDED(f.device->CheckRemoved()));
}
