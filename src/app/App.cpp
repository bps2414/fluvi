// SPDX-License-Identifier: GPL-3.0-only
#include "App.h"

#include <algorithm>
#include <chrono>

#include "Models.h"
#include "fluvi/core/Log.h"

namespace fluvi::app {

using namespace std::chrono_literals;

namespace {
// The Xbox gamertag is not exposed by xhome::Account yet; the chip says whose login this is without inventing a name.
constexpr const char* kAccountChip = "Conta Microsoft";
}

AppController::AppController(ui::Ui& ui, AppSettings settings, std::string rawSettings, std::filesystem::path settingsFile, bool saveInitial)
    : ui_(ui), settings_(settings), saver_(std::move(settingsFile), std::move(rawSettings)) {
    fullscreen_ = settings.video.fullscreen;
    fullscreenKey_ = settings.hotkeys.toggleFullscreen;
    if (saveInitial) saver_.Request(settings);

    transport_ = xhome::makeWinHttpTransport(&quit_);
    account_ = std::make_unique<xhome::Account>(transport_, xhome::dataDirectory() / "account.bin");

    XhomeSessionDeps deps;
    deps.api = [this] { return EnsureApi(); };
    deps.transferToken = [this] { return account_->transferToken(); };
    controller_ = MakeXhomeStreamController(std::move(deps));
    pad_ = input::GamepadReader::Create();
}

AppController::~AppController() { Shutdown(); }

void AppController::SetHooks(AppHooks hooks) { hooks_ = std::move(hooks); }

GamepadState AppController::PadState() const {
    std::lock_guard lock(padMutex_);
    return padState_;
}

AppSettings AppController::Settings() const {
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void AppController::Start() {
    if (started_) return;
    started_ = true;

    // Stream events and hooks. The rumble default sends the console's vibration to the pad.
    StreamHooks sh = hooks_.stream;
    if (!sh.media.rumble) {
        sh.media.rumble = [this](const Rumble& r) {
            const AppSettings s = Settings();
            if (pad_ && s.controller.rumble) pad_->SetRumble(0, r, s.controller.rumbleStrength);
        };
    }
    controller_->setHooks(std::move(sh));
    StreamEvents ev;
    ev.stage = [this](StreamStage s) { OnStage(s); };
    ev.failed = [this](const std::string& r) { OnStreamFailed(r); };
    ev.ended = [this](const std::string& r) { OnStreamEnded(r); };
    controller_->setEvents(std::move(ev));

    InstallCallbacks();
    ui_.SetSettings(Settings());

    if (pad_ && !pad_->Start([this](const input::GamepadEvent& e) { OnPad(e); }))
        FLUVI_WARN("app", "pad reader did not start; the UI keeps working with keyboard and mouse");

    if (account_->signedIn()) {
        ui_.ShowScreen(ui::Screen::Consoles);
        RefreshConsoles();
    } else {
        ui_.SetSignIn({});
        ui_.ShowScreen(ui::Screen::SignIn);
    }
}

void AppController::Shutdown() {
    if (!started_ && !pad_) return;
    quit_ = true;   // aborts the HTTP requests in flight
    { std::lock_guard lock(tokenMutex_); if (signInToken_) signInToken_->Cancel(); if (wakeToken_) wakeToken_->Cancel(); }
    if (controller_) controller_->cancel();
    if (pad_) pad_->Stop();
    tasks_.JoinAll();
    controller_.reset();   // joins the attempt threads (they close the session and tell the service)
    saver_.Flush();
    started_ = false;
    pad_.reset();
}

void AppController::InstallCallbacks() {
    ui::UiCallbacks cb;
    cb.signIn = [this] { StartSignIn(); };
    cb.cancelSignIn = [this] { CancelSignIn(); };
    cb.signOut = [this] { SignOut(); };
    cb.refreshConsoles = [this] { RefreshConsoles(); };
    cb.wake = [this](const std::string& id) { Wake(id); };
    cb.connect = [this](const std::string& id) { Connect(id); };
    cb.cancelConnect = [this] { CancelConnect(); };
    cb.disconnect = [this] { Disconnect(); };
    cb.settingsChanged = [this](const AppSettings& s) { SettingsChanged(s); };
    cb.modeChanged = [this](StreamMode m) {
        FLUVI_INFO("app", "mode -> {}", m == StreamMode::Smooth ? "Liso" : "Jogar");
        if (hooks_.modeChanged) hooks_.modeChanged(m);   // HOOK(chain): switch the pipeline (not persisted: it is per session)
    };
    cb.quit = [this] { quit_ = true; };
    ui_.SetCallbacks(std::move(cb));
}

// ---- settings ----------------------------------------------------------------------------------------------------

void AppController::SettingsChanged(const AppSettings& s) {
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = s;
    }
    fullscreen_ = s.video.fullscreen;
    saver_.Request(s);
    if (hooks_.settingsApplied) hooks_.settingsApplied(s);
}

void AppController::ToggleFullscreen() {
    AppSettings s = Settings();
    s.video.fullscreen = !s.video.fullscreen;
    SettingsChanged(s);
    ui_.SetSettings(s);
}

void AppController::ToggleOverlayFromPad() {
    if (ui_.CurrentScreen() == ui::Screen::Stream) ui_.SetOverlayVisible(!ui_.OverlayVisible());
}

// ---- sign-in -----------------------------------------------------------------------------------------------------

void AppController::StartSignIn() {
    auto token = std::make_shared<CancelToken>();
    {
        std::lock_guard lock(tokenMutex_);
        if (signInToken_) signInToken_->Cancel();
        signInToken_ = token;
    }
    ui::SignInModel pending;
    pending.busy = true;   // the screen shows the code / QR skeletons until the code arrives
    ui_.SetSignIn(pending);

    tasks_.Run([this, token] {
        const auto fail = [&](const std::string& message) {
            if (!token->Cancelled()) ui_.SetSignIn({{}, {}, message, false});
        };
        xhome::DeviceCode code;
        try {
            code = account_->beginSignIn();
        } catch (const std::exception& e) {
            fail(DescribeError(e));
            return;
        }
        if (token->Cancelled()) return;
        ui::SignInModel shown;
        shown.userCode = code.userCode;
        shown.verificationUri = code.verificationUri;
        shown.status = "Esperando você confirmar no outro aparelho";
        shown.busy = true;
        ui_.SetSignIn(shown);

        int interval = std::max(code.intervalSeconds, 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(code.expiresInSeconds, 30));
        int errors = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!token->Sleep(std::chrono::seconds(interval))) return;
            std::string detail;
            xhome::SignInPoll result;
            try {
                result = account_->pollSignIn(code, &detail);
                errors = 0;
            } catch (const xhome::NetworkError& e) {
                if (++errors >= 6) { fail(DescribeError(e)); return; }   // a few dropped polls are fine
                continue;
            } catch (const std::exception& e) {
                fail(DescribeError(e));
                return;
            }
            if (token->Cancelled()) return;
            switch (result) {
            case xhome::SignInPoll::Pending: break;
            case xhome::SignInPoll::SlowDown: interval += 5; break;
            case xhome::SignInPoll::Done:
                ui_.SetSignIn({});
                OnSignedIn();
                return;
            default:
                fail(DescribeSignInFailure(result, detail));
                return;
            }
        }
        fail(DescribeSignInFailure(xhome::SignInPoll::Expired, {}));
    });
}

void AppController::CancelSignIn() {
    {
        std::lock_guard lock(tokenMutex_);
        if (signInToken_) signInToken_->Cancel();
    }
    ui_.SetSignIn({});
}

void AppController::OnSignedIn() {
    FLUVI_INFO("app", "signed in");
    ui_.ShowScreen(ui::Screen::Consoles);
    RefreshConsoles();
}

void AppController::GoToSignIn(const std::string& message) {
    {
        std::lock_guard lock(tokenMutex_);
        if (signInToken_) signInToken_->Cancel();
        if (wakeToken_) wakeToken_->Cancel();
    }
    if (controller_) controller_->cancel();
    streaming_ = false;
    {
        std::lock_guard lock(apiMutex_);
        api_.reset();
        apiToken_.clear();
    }
    {
        std::lock_guard lock(consolesMutex_);
        consoles_.clear();
        busyId_.clear();
        consolesError_.clear();
        refreshing_ = false;
    }
    ui::SignInModel m;
    m.status = message;
    ui_.SetSignIn(m);
    ui_.ShowScreen(ui::Screen::SignIn);
}

void AppController::SignOut() {
    account_->signOut();   // forgets the tokens and removes the file
    GoToSignIn({});
}

bool AppController::HandleExpiredLogin(const std::exception& e) {
    if (account_->signedIn()) return false;
    FLUVI_WARN("app", "saved login is gone: {}", e.what());
    GoToSignIn("Sua sessão venceu. Entre de novo.");
    return true;
}

// ---- consoles ----------------------------------------------------------------------------------------------------

std::shared_ptr<xhome::StreamApi> AppController::EnsureApi() {
    const xhome::StreamingAccess access = account_->streamingAccess();   // network when the token is stale
    std::lock_guard lock(apiMutex_);
    if (!api_ || apiToken_ != access.gsToken) {
        api_ = std::make_shared<xhome::StreamApi>(transport_, access.host, access.gsToken);
        api_->setWakeAuthorization([this] { return account_->webAuthorization(); });
        apiToken_ = access.gsToken;
    }
    return api_;
}

void AppController::PublishConsoles() {
    ConsolesContext ctx;
    std::vector<xhome::Console> list;
    {
        std::lock_guard lock(consolesMutex_);
        list = consoles_;
        ctx.busyId = busyId_;
        ctx.error = consolesError_;
        ctx.refreshing = refreshing_;
    }
    ctx.accountName = kAccountChip;
    ui_.SetConsoles(MapConsoles(list, ctx));
}

void AppController::RefreshConsoles() {
    {
        std::lock_guard lock(consolesMutex_);
        if (refreshing_) return;
        refreshing_ = true;
        consolesError_.clear();
    }
    PublishConsoles();
    tasks_.Run([this] {
        std::vector<xhome::Console> list;
        std::string error;
        try {
            list = EnsureApi()->consoles();
        } catch (const std::exception& e) {
            if (HandleExpiredLogin(e)) return;
            error = DescribeError(e);
            FLUVI_WARN("app", "consoles: {}", e.what());
        }
        {
            std::lock_guard lock(consolesMutex_);
            refreshing_ = false;
            if (error.empty()) consoles_ = std::move(list);
            consolesError_ = error;
        }
        PublishConsoles();
    });
}

// Wake = the service's power-on command, then poll the list until the console says "On" (<= 90 s). The user
// connects with the next pick: starting the stream is an explicit act.
void AppController::Wake(const std::string& id) {
    auto token = std::make_shared<CancelToken>();
    {
        std::lock_guard lock(consolesMutex_);
        if (!busyId_.empty()) return;
        busyId_ = id;
        consolesError_.clear();
    }
    {
        std::lock_guard lock(tokenMutex_);
        if (wakeToken_) wakeToken_->Cancel();
        wakeToken_ = token;
    }
    PublishConsoles();
    tasks_.Run([this, id, token] {
        std::string error;
        try {
            EnsureApi()->wake(id);
            for (int i = 0; i < 30 && !token->Cancelled(); ++i) {
                if (!token->Sleep(3s)) return;
                const auto list = EnsureApi()->consoles();
                bool on = false;
                for (const auto& c : list) if (c.serverId == id && MapPower(c.powerState) == ui::PowerState::On) on = true;
                { std::lock_guard lock(consolesMutex_); consoles_ = list; }
                if (on) break;
                PublishConsoles();
                if (i == 29) error = "O console não ligou em 90 segundos. Confira a energia e a rede dele.";
            }
        } catch (const std::exception& e) {
            if (HandleExpiredLogin(e)) return;
            error = DescribeError(e);
        }
        if (token->Cancelled()) return;
        {
            std::lock_guard lock(consolesMutex_);
            busyId_.clear();
            consolesError_ = error;
        }
        PublishConsoles();
    });
}

// ---- connecting / streaming ---------------------------------------------------------------------------------------

void AppController::Connect(const std::string& id) {
    {
        std::lock_guard lock(connectMutex_);
        connectingName_.clear();
    }
    {
        std::lock_guard lock(consolesMutex_);
        for (const auto& c : consoles_) {
            if (c.serverId != id) continue;
            const ui::ConsoleItem item = MapConsoles({c}, {}).consoles.front();
            std::lock_guard nameLock(connectMutex_);
            connectingName_ = item.name;
        }
    }
    ui::ConnectingModel m;
    { std::lock_guard lock(connectMutex_); m.consoleName = connectingName_; }
    m.stage = ui::ConnectStage::Provisioning;
    ui_.SetConnecting(m);
    ui_.ShowScreen(ui::Screen::Connecting);
    controller_->start(id, Settings());
}

void AppController::OnStage(StreamStage stage) {
    ui::ConnectingModel m;
    { std::lock_guard lock(connectMutex_); m.consoleName = connectingName_; }
    m.stage = MapStage(stage);
    ui_.SetConnecting(m);
    if (stage == StreamStage::Connected) {
        streaming_ = true;
        ui_.SetOverlayVisible(false);
        ui_.ShowScreen(ui::Screen::Stream);
        if (hooks_.streamStarted) hooks_.streamStarted();
    }
}

void AppController::OnStreamFailed(const std::string& reason) {
    ui::ConnectingModel m;
    { std::lock_guard lock(connectMutex_); m.consoleName = connectingName_; }
    m.stage = ui::ConnectStage::Failed;
    m.detail = reason;
    const bool wasStreaming = streaming_.exchange(false);
    ui_.SetConnecting(m);   // the screen stays up until the user backs out (Esc -> CancelConnect)
    if (wasStreaming) {
        ui_.ShowScreen(ui::Screen::Connecting);
        if (hooks_.streamStopped) hooks_.streamStopped();
    }
}

void AppController::OnStreamEnded(const std::string& reason) {
    // HOOK(reconnect): xhome::reconnectableDisconnect(reason) + ReconnectBudget (DisconnectPolicy.h) decide whether
    // a retry could go here; for now every end goes back to the consoles list with the reason.
    FLUVI_INFO("app", "stream ended: {}", reason);
    streaming_ = false;
    if (hooks_.streamStopped) hooks_.streamStopped();
    {
        std::lock_guard lock(consolesMutex_);
        consolesError_ = DescribeEnded(reason);
    }
    ui_.SetOverlayVisible(false);
    ui_.ShowScreen(ui::Screen::Consoles);
    PublishConsoles();
    RefreshConsoles();
}

void AppController::CancelConnect() {
    controller_->cancel();
    const bool wasStreaming = streaming_.exchange(false);
    if (wasStreaming && hooks_.streamStopped) hooks_.streamStopped();
    ui_.ShowScreen(ui::Screen::Consoles);
    RefreshConsoles();
}

void AppController::Disconnect() {
    CancelConnect();
}

// ---- pad -----------------------------------------------------------------------------------------------------------

// Reader thread. While streaming (and no menu open) the state goes to the console; the View + Menu chord is ours.
void AppController::OnPad(const input::GamepadEvent& e) {
    {
        std::lock_guard lock(padMutex_);
        padState_ = e.state;
    }
    const bool chord = (e.state.buttons & (BtnView | BtnMenu)) == (BtnView | BtnMenu);
    const bool forward = streaming_.load() && !ui_.WantsInput() && !chord;
    bool wasForwarding;
    {
        std::lock_guard lock(padMutex_);
        wasForwarding = padForwarding_;
        padForwarding_ = forward;
    }
    if (forward) controller_->sendGamepad(e.state, e.index);
    else if (wasForwarding) controller_->sendGamepad(GamepadState{}, e.index);   // let go of everything on the console
}

} // namespace fluvi::app
