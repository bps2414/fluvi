// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app logic: connects the UI (fluvi_ui) to xhome (sign-in, consoles, wake), the stream controller, the
// controller input and the saved settings. No window and no GPU in here: main.cpp owns those and drives
// this once per frame. Every network call runs on its own thread; the UI model setters are thread-safe.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SettingsStore.h"
#include "StreamController.h"
#include "Tasks.h"
#include "fluvi/common/Gamepad.h"
#include "fluvi/common/Settings.h"
#include "fluvi/input/GamepadReader.h"
#include "fluvi/ui/Ui.h"
#include "fluvi/xhome/Account.h"
#include "fluvi/xhome/Https.h"

namespace fluvi::app {

// Where main.cpp plugs the rest of the pipeline in. Set before Start(); callbacks may run on any thread.
struct AppHooks {
    StreamHooks stream;                                       // HOOK(decode) video/audio, HOOK(chain) connected/closing
    std::function<void(StreamMode)> modeChanged;              // overlay toggle or hotkey: Jogar <-> Liso in the pipeline
    std::function<void(const AppSettings&)> settingsApplied;  // every settings change (chain->SetConfig(ResolveChainConfig(...)))
    std::function<void()> streamStarted;                      // Connected: the UI is on Screen::Stream
    std::function<void()> streamStopped;                      // cancel / disconnect / ended / failed after being up
};

class AppController {
public:
    // `rawSettings`: the settings file text as loaded (unknown keys are kept on save). `saveInitial`: write the
    // settings even if nothing changes (first run, after the preset detection).
    AppController(ui::Ui& ui, AppSettings settings, std::string rawSettings, std::filesystem::path settingsFile, bool saveInitial);
    ~AppController();

    void SetHooks(AppHooks hooks);
    // Installs the UI callbacks, starts the pad reader, shows the first screen (consoles when a saved login exists,
    // otherwise sign-in) and starts loading.
    void Start();
    // Cancels every task and the stream, flushes the settings. Call before the UI / GPU are torn down.
    void Shutdown();

    // ---- read by the render loop ----
    bool QuitRequested() const { return quit_.load(); }
    bool Streaming() const { return streaming_.load(); }
    // Navigate the UI with the pad: always, except while the stream owns the pad (then only with a menu open).
    bool PadNavEnabled() const { return !streaming_.load() || ui_.WantsInput(); }
    GamepadState PadState() const;
    AppSettings Settings() const;
    bool DesiredFullscreen() const { return fullscreen_.load(); }
    uint32_t FullscreenKey() const { return fullscreenKey_.load(); }

    // F11 (or the hotkey): flips the setting, refreshes the UI row, saves.
    void ToggleFullscreen();
    // The window was closed.
    void RequestQuit() { quit_ = true; }

    // The overlay chord (View + Menu) from the pad. Only meaningful on the stream screen.
    void ToggleOverlayFromPad();

private:
    void InstallCallbacks();
    void StartSignIn();
    void CancelSignIn();
    void SignOut();
    void GoToSignIn(const std::string& message);
    void OnSignedIn();
    void RefreshConsoles();
    void PublishConsoles();
    void Wake(const std::string& id);
    void Connect(const std::string& id);
    void CancelConnect();
    void Disconnect();
    void OnStage(StreamStage stage);
    void OnStreamFailed(const std::string& reason);
    void OnStreamEnded(const std::string& reason);
    void SettingsChanged(const AppSettings& s);
    void OnPad(const input::GamepadEvent& e);
    std::shared_ptr<xhome::StreamApi> EnsureApi();
    // True when the error means the saved login is gone (and moved the UI to sign-in).
    bool HandleExpiredLogin(const std::exception& e);

    ui::Ui& ui_;
    AppHooks hooks_;
    std::atomic<bool> quit_{false}, streaming_{false}, fullscreen_{true};
    std::atomic<uint32_t> fullscreenKey_{0x7A};

    mutable std::mutex settingsMutex_;
    AppSettings settings_;
    SettingsSaver saver_;

    std::shared_ptr<xhome::HttpsTransport> transport_;
    std::unique_ptr<xhome::Account> account_;
    std::mutex apiMutex_;
    std::shared_ptr<xhome::StreamApi> api_;
    std::string apiToken_;

    // Consoles screen state.
    std::mutex consolesMutex_;
    std::vector<xhome::Console> consoles_;
    std::string busyId_, consolesError_;
    bool refreshing_ = false;

    std::mutex tokenMutex_;
    CancelTokenPtr signInToken_, wakeToken_;
    std::string connectingName_;
    std::mutex connectMutex_;

    std::unique_ptr<StreamController> controller_;
    std::unique_ptr<input::GamepadReader> pad_;
    mutable std::mutex padMutex_;
    GamepadState padState_;
    bool padForwarding_ = false;

    TaskSet tasks_;
    bool started_ = false;
};

} // namespace fluvi::app
