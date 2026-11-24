// SPDX-License-Identifier: GPL-3.0-only
#include "App.h"

#include <algorithm>
#include <chrono>

#include "Models.h"
#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

namespace fluvi::app {

using namespace std::chrono_literals;

namespace {
constexpr int kWakePolls = 30;          // wake: poll the console list every kWakePollSeconds, up to 90 s
constexpr int kWakePollSeconds = 3;

// The Xbox gamertag is not exposed by xhome::Account yet; the chip says whose login this is without inventing a name.
std::string AccountChip() { return i18n::Tr("app.account.chip"); }

std::string WakeTimeoutText() { return i18n::Tr("app.err.wakeTimeout", {{"seconds", std::to_string(kWakePolls * kWakePollSeconds)}}); }

bool SamePlacement(const WindowSettings& a, const WindowSettings& b) {
    return a.hasPlacement == b.hasPlacement && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
           a.maximized == b.maximized;
}
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

    // The splash is up while the stored login is checked; the UI keeps it long enough for its entrance to play.
    if (ui_.CurrentScreen() != ui::Screen::Splash) ui_.ShowScreen(ui::Screen::Splash);
    ui_.SetSplashStatus(i18n::Tr("app.splash.checking"));
    if (account_->signedIn()) {
        tasks_.Run([this] { CheckStoredAccount(); });
    } else {
        ui_.SetSignIn({});
        ui_.ShowScreen(ui::Screen::SignIn);
    }
}

// Splash task: refreshes the saved login if it is stale (network), then moves to the consoles. An expired login goes to
// sign-in (HandleExpiredLogin); being offline still shows the list, with its error and Refresh.
void AppController::CheckStoredAccount() {
    try {
        EnsureApi();
    } catch (const std::exception& e) {
        if (HandleExpiredLogin(e)) return;
        FLUVI_WARN("app", "account check: {}", e.what());
    }
    if (quit_.load()) return;
    RefreshConsoles();
    ui_.ShowScreen(ui::Screen::Consoles);
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
    cb.wakeAndConnect = [this](const std::string& id) { WakeAndConnect(id); };
    cb.minimize = [this] { if (hooks_.minimize) hooks_.minimize(); };
    cb.toggleMaximize = [this] { if (hooks_.toggleMaximize) hooks_.toggleMaximize(); };
    ui_.SetCallbacks(std::move(cb));
}

// ---- settings ----------------------------------------------------------------------------------------------------

void AppController::SettingsChanged(const AppSettings& incoming) {
    AppSettings s = incoming;
    {
        std::lock_guard lock(settingsMutex_);
        s.window = settings_.window;   // the placement belongs to the window; the UI's copy may be stale
        settings_ = s;
    }
    fullscreen_ = s.video.fullscreen;
    saver_.Request(s);
    if (hooks_.settingsApplied) hooks_.settingsApplied(s);
}

void AppController::SaveWindowPlacement(const WindowSettings& placement) {
    AppSettings s;
    {
        std::lock_guard lock(settingsMutex_);
        if (SamePlacement(settings_.window, placement)) return;
        settings_.window = placement;
        s = settings_;
    }
    saver_.Request(s);
}

void AppController::OnLanguageChanged() { PublishConsoles(); }

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
        shown.status = i18n::Tr("app.signin.waiting");
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
    showStreamPending_ = false;
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
    streaming_ = false;   // after ShowScreen, so the render loop can keep the last video frame through the transition
}

void AppController::SignOut() {
    account_->signOut();   // forgets the tokens and removes the file
    GoToSignIn({});
}

bool AppController::HandleExpiredLogin(const std::exception& e) {
    if (account_->signedIn()) return false;
    FLUVI_WARN("app", "saved login is gone: {}", e.what());
    GoToSignIn(i18n::Tr("app.signin.sessionExpired"));
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
    ctx.accountName = AccountChip();
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
            error = DescribeError(e);   // logs the raw detail
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
            for (int i = 0; i < kWakePolls && !token->Cancelled(); ++i) {
                if (!token->Sleep(std::chrono::seconds(kWakePollSeconds))) return;
                const auto list = EnsureApi()->consoles();
                bool on = false;
                for (const auto& c : list) if (c.serverId == id && MapPower(c.powerState) == ui::PowerState::On) on = true;
                { std::lock_guard lock(consolesMutex_); consoles_ = list; }
                if (on) break;
                PublishConsoles();
                if (i == kWakePolls - 1) error = WakeTimeoutText();
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

void AppController::SetConnectingName(const std::string& id) {
    std::string name;
    {
        std::lock_guard lock(consolesMutex_);
        for (const auto& c : consoles_)
            if (c.serverId == id) name = MapConsoles({c}, {}).consoles.front().name;
    }
    std::lock_guard lock(connectMutex_);
    connectingName_ = std::move(name);
}

// "Ligar e jogar": wake the console, show Connecting for it, poll until it reports On, then connect. Already on: connect.
void AppController::WakeAndConnect(const std::string& id) {
    bool alreadyOn = false;
    {
        std::lock_guard lock(consolesMutex_);
        if (!busyId_.empty()) return;   // a wake from the list is running
        for (const auto& c : consoles_)
            if (c.serverId == id && MapPower(c.powerState) == ui::PowerState::On) alreadyOn = true;
    }
    if (alreadyOn) {
        Connect(id);
        return;
    }
    auto token = std::make_shared<CancelToken>();
    {
        std::lock_guard lock(tokenMutex_);
        if (wakeToken_) wakeToken_->Cancel();
        wakeToken_ = token;
    }
    SetConnectingName(id);
    ui::ConnectingModel m;
    { std::lock_guard lock(connectMutex_); m.consoleName = connectingName_; }
    m.stage = ui::ConnectStage::Provisioning;
    ui_.SetConnecting(m);
    ui_.ShowScreen(ui::Screen::Connecting);
    tasks_.Run([this, id, token, m]() mutable {
        std::string error;
        bool on = false;
        try {
            EnsureApi()->wake(id);
            for (int i = 0; i < kWakePolls && !on; ++i) {
                if (!token->Sleep(std::chrono::seconds(kWakePollSeconds))) return;
                const auto list = EnsureApi()->consoles();
                for (const auto& c : list)
                    if (c.serverId == id && MapPower(c.powerState) == ui::PowerState::On) on = true;
                std::lock_guard lock(consolesMutex_);
                consoles_ = list;
            }
            if (!on) error = WakeTimeoutText();
        } catch (const std::exception& e) {
            if (HandleExpiredLogin(e)) return;
            error = DescribeError(e);
        }
        if (token->Cancelled()) return;
        if (!on) {
            m.stage = ui::ConnectStage::Failed;
            m.detail = error;
            ui_.SetConnecting(m);   // the screen offers retry / back
            return;
        }
        Connect(id);
    });
}

void AppController::Connect(const std::string& id) {
    SetConnectingName(id);
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
        // The render loop starts drawing video under the Connecting screen; the stream screen follows once a frame
        // was really presented (OnFirstFramePresented), so no black or half-decoded frame is ever visible.
        streaming_ = true;
        showStreamPending_ = true;
    }
}

void AppController::OnFirstFramePresented() {
    if (!showStreamPending_.exchange(false) || !streaming_.load()) return;
    ui_.SetOverlayVisible(false);
    ui_.ShowScreen(ui::Screen::Stream);
    if (hooks_.streamStarted) hooks_.streamStarted();
}

void AppController::ReportNoVideo() {
    if (!showStreamPending_.load()) return;
    FLUVI_WARN("app", "no video frame after the stream connected");
    OnStreamFailed(i18n::Tr("app.err.noVideo"));
}

void AppController::OnStreamFailed(const std::string& reason) {
    ui::ConnectingModel m;
    { std::lock_guard lock(connectMutex_); m.consoleName = connectingName_; }
    m.stage = ui::ConnectStage::Failed;
    m.detail = reason;
    showStreamPending_ = false;
    const bool wasStreaming = streaming_.load();
    ui_.SetConnecting(m);   // the screen stays up until the user backs out (Esc -> CancelConnect)
    if (wasStreaming) {
        ui_.ShowScreen(ui::Screen::Connecting);
        streaming_ = false;   // after ShowScreen: the render loop keeps the last frame while the screen changes
        if (hooks_.streamStopped) hooks_.streamStopped();
    }
}

void AppController::OnStreamEnded(const std::string& reason) {
    // HOOK(reconnect): xhome::reconnectableDisconnect(reason) + ReconnectBudget (DisconnectPolicy.h) decide whether
    // a retry could go here; for now every end goes back to the consoles list with the reason.
    FLUVI_INFO("app", "stream ended: {}", reason);
    showStreamPending_ = false;
    {
        std::lock_guard lock(consolesMutex_);
        consolesError_ = DescribeEnded(reason);
    }
    ui_.SetOverlayVisible(false);
    ui_.ShowScreen(ui::Screen::Consoles);
    streaming_ = false;   // after ShowScreen: the render loop keeps the last frame while the screen changes
    if (hooks_.streamStopped) hooks_.streamStopped();
    PublishConsoles();
    RefreshConsoles();
}

void AppController::CancelConnect() {
    controller_->cancel();
    {
        std::lock_guard lock(tokenMutex_);
        if (wakeToken_) wakeToken_->Cancel();   // a "Ligar e jogar" still waiting for the console
    }
    showStreamPending_ = false;
    const bool wasStreaming = streaming_.load();
    ui_.ShowScreen(ui::Screen::Consoles);
    streaming_ = false;   // after ShowScreen: the render loop keeps the last frame while the screen changes
    if (wasStreaming && hooks_.streamStopped) hooks_.streamStopped();
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
    const bool forward = streaming_.load() && !showStreamPending_.load() && !ui_.WantsInput() && !chord;
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
