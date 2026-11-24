// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: internal declarations shared by UiImpl.cpp (context, render, screens, transitions), UiModel.cpp (data
// model, formatting), UiInput.cpp (messages, hotkeys, focus, title-bar hit test) and UiSettings.cpp (settings rows).
#pragma once
#include "fluvi/ui/Ui.h"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core.h>

#include "UiStats.h"
#include "UiSystem.h"

namespace fluvi::ui {

using Clock = std::chrono::steady_clock;

// ---- View models bound to the "fluvi" data model (names match the *.rml files) ----

struct UiVm {
    Rml::String toast;
    bool toast_on = false, confirm_quit = false;
};

struct ChromeVm {
    bool visible = true, maximized = false, active = true;
};

struct SplashVm {
    Rml::String status;
};

struct SignVm {
    Rml::String code, uri_short, status, qr;
    bool busy = false, error = false, has_code = false, has_qr = false, pending = false;   // pending: code/QR requested, not here yet
};

struct ConsoleVm {
    Rml::String id, name, type, power, action;
    bool on = false, busy = false;
};

struct ConsVm {
    std::vector<ConsoleVm> list;
    Rml::String account, initial, error;
    bool has_account = false, refreshing = false, loading = true, single = false, menu = false;   // loading: no list yet
};

struct ConnVm {
    Rml::String console, title, detail;
    int stage = 0;
    bool failed = false, loading = true;   // loading: console name not known yet
};

struct StreamVm {
    bool overlay = false, smooth = false, flip = false, vrr_on = false, has_stats = false, details = false;
    int band = 0, hud = 0, corner = 0;
    std::vector<int> graph;
    Rml::String console, session, mode, smooth_desc, latency, latency_cap, band_word, fps_game, fps_stream, fps_gen,
        fps_present, fps_line, res, res_out, bitrate, present, vrr, refresh, timing, loss, loss_short, warning, hk_overlay,
        hk_mode, hk_hud;
};

struct SetVm {
    Rml::String resolution, bitrate, mode, deblock, sharpness, strength, volume, hk_overlay, hk_mode, hk_hud, hk_full,
        hk_debug, preset, upscaler, hud, corner, language, version;
    int deblock_pct = 0, sharpness_pct = 0, strength_pct = 0, volume_pct = 0, section = 0;
    bool fullscreen_on = false, switchhz_on = false, rumble_on = false, newframes_on = false, loaded = false;   // loaded: the app sent the settings
};

// ---- Input queued on the window thread, replayed on the render thread ----

enum class Hotkey : uint8_t { ToggleOverlay, ToggleMode, CycleHud, Reload, Back };

struct InputEvent {
    enum class Kind : uint8_t { MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Char, Leave, Hotkey } kind;
    int a = 0, b = 0, mods = 0;
    float wheel = 0;
    bool pad = false;   // a key synthesised from the controller (sent from another thread than the window's)
};

struct Pending {
    std::optional<UiCallbacks> callbacks;
    std::optional<SignInModel> sign;
    std::optional<ConsolesModel> cons;
    std::optional<ConnectingModel> conn;
    std::optional<OverlayStats> stats;
    std::optional<AppSettings> settings;
    std::optional<std::string> splash;
    std::optional<WindowChromeModel> chrome;
    std::optional<std::pair<uint32_t, uint32_t>> size;
    std::optional<float> dpi;
    bool reload = false;
    bool language = false;
    bool calmCheck = false;
    std::vector<InputEvent> input;
};

inline constexpr size_t kScreenCount = 6;
// Indexed by Screen.
inline constexpr std::array<const char*, kScreenCount> kDocFiles = {"signin.rml", "consoles.rml", "connecting.rml",
                                                                    "stream.rml", "settings.rml", "splash.rml"};

// Screen changes: a crossfade, or the choreography in and out of the game (see Screen in Ui.h).
enum class Phase : uint8_t { Idle, Crossfade, ToBed, Lift, Drop };

// Title-bar rectangles in client pixels, cached after each layout for HitTest() on the window thread.
struct ChromeRects {
    bool valid = false;
    Rml::Rectanglef bar, min, max, close;
};

class UiImpl;

// Settings rows (settings screen and the overlay's quick settings): click (arrow with data-dir, or the row itself
// = +1 with wrap) and Left/Right on the focused row. Also the settings section nav (Right enters the rows).
class SettingsListener final : public Rml::EventListener {
public:
    explicit SettingsListener(UiImpl& ui) : ui_(ui) {}
    void ProcessEvent(Rml::Event& event) override;

private:
    UiImpl& ui_;
};

class UiImpl final : public Ui {
public:
    UiImpl() : listener_(*this) {}
    ~UiImpl() override { Shutdown(); }

    bool Init(const UiInitDesc& desc, Status& status);

    // ---- Ui (UiImpl.cpp unless noted) ----
    void SetCallbacks(UiCallbacks callbacks) override;
    void ShowScreen(Screen screen) override;
    Screen CurrentScreen() const override { return screen_.load(); }
    void SetSignIn(const SignInModel& model) override;
    void SetConsoles(const ConsolesModel& model) override;
    void SetConnecting(const ConnectingModel& model) override;
    void SetStats(const OverlayStats& stats) override;
    void SetSettings(const AppSettings& settings) override;
    void SetOverlayVisible(bool visible) override { overlay_.store(visible); }
    bool OverlayVisible() const override { return overlay_.load(); }
    bool WantsInput() const override { return screen_.load() != Screen::Stream || overlay_.load() || dialog_.load(); }
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;   // UiInput.cpp
    void Resize(uint32_t width, uint32_t height, float dpiScale) override;
    void Update() override;
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t frameIndex) override;
    void ReloadDocuments() override;
    void SetSplashStatus(const std::string& status) override;
    void SetWindowChrome(const WindowChromeModel& model) override;
    WindowHit HitTest(int x, int y) const override;   // UiInput.cpp
    bool InTransition() const override { return transition_.load(); }
    void LanguageChanged() override;

    // ---- Called by SettingsListener (render thread, inside Update) ----
    void StepSetting(const Rml::String& key, int dir, bool wrap);   // UiSettings.cpp
    void FocusFirstRow();                                            // UiSettings.cpp

private:
    // UiImpl.cpp: lifetime, documents, layout, screens and transitions
    void Shutdown();
    bool LoadFonts();
    bool LoadDocuments();
    void CloseDocuments();
    void ApplySize(uint32_t width, uint32_t height, float dpi);
    void ApplyScreen();
    void StartTransition(Screen from, Screen to);
    void FinishPhase(bool instant);
    void ShowDoc(Screen screen);
    void HideDoc(Screen screen);
    Rml::ElementDocument* Doc(Screen screen) const { return docs_[static_cast<size_t>(screen)]; }
    void SetDocClass(const char* name, bool on);
    void Toast(std::string text);
    void CheckCalm();

    // UiModel.cpp: data model, events, model -> view model
    bool BuildModel();
    void FillSign(const SignInModel& m);
    void FillCons(const ConsolesModel& m);
    void FillConn(const ConnectingModel& m);
    void FillStats(const OverlayStats& s);
    void FillStreamFlags();
    void FillChrome();
    void RefillText();   // after a language change: every string the UI built in C++
    void Pick(const std::string& id);
    void RetryConnect();
    void RequestQuit();
    void OpenBrowser();
    void CopyCode();
    void SetMode(StreamMode mode);
    void CycleHud();
    void OnStreamEntered();
    void TickStream();

    // UiInput.cpp: input replay, hotkeys, focus, title-bar rectangles
    void Enqueue(const InputEvent& e);
    void Replay(const InputEvent& e);
    void OnHotkey(Hotkey key);
    void KeepFocus(bool changed);
    void CacheChrome();

    // UiSettings.cpp
    void FillSettings();
    void LoadLanguageNames();

    std::filesystem::path assets_, i18nDir_;
    HWND hwnd_ = nullptr;
    SystemInterface system_;
    std::unique_ptr<Renderer> renderer_;
    bool rmlInitialised_ = false;
    Rml::Context* context_ = nullptr;
    Rml::DataModelHandle model_;
    std::array<Rml::ElementDocument*, kScreenCount> docs_{};
    SettingsListener listener_;
    std::map<int, QrMatrix> qrs_;
    int qrSerial_ = 0;
    std::string qrText_;

    // Cross-thread state.
    std::mutex mutex_;
    Pending pending_;
    std::atomic<Screen> screen_{Screen::Splash};
    std::atomic<Screen> returnTo_{Screen::SignIn};   // where "Voltar" in settings goes
    std::atomic<bool> overlay_{false};
    std::atomic<bool> dialog_{false};       // the quit confirmation is open (it takes input even over the game)
    std::atomic<bool> transition_{false};
    std::atomic<uint32_t> hkOverlay_{VK_F1}, hkMode_{VK_F2}, hkHud_{VK_F3};
    mutable std::mutex chromeMutex_;
    ChromeRects chromeRects_;
    bool chromeVisible_ = true;             // under chromeMutex_

    // Render-thread state.
    UiCallbacks cb_;
    AppSettings settings_;
    std::vector<ConsoleItem> consoles_;
    std::optional<SignInModel> lastSign_;
    std::optional<ConsolesModel> lastCons_;
    std::optional<ConnectingModel> lastConn_;
    std::optional<OverlayStats> lastStats_;
    std::string splashStatus_;
    Screen shown_ = Screen::Splash;         // the screen on top (or arriving)
    bool shownValid_ = false;
    bool overlayShown_ = false;
    Phase phase_ = Phase::Idle;
    Clock::time_point phaseEnd_{};
    std::optional<Screen> leaving_;         // the screen fading or veiled out
    Clock::time_point splashSince_{};
    bool splashSkip_ = false;
    bool calm_ = false;                     // Windows animations off: screen changes cut
    bool padInput_ = false;                 // last key came from the controller: hints show its buttons
    std::string wantFocus_;                 // element id to focus after the next layout
    std::string connectId_;                 // console of the last connect / wake-and-connect (for "Tentar de novo")
    bool connectWake_ = false;
    std::string lastPlayedId_;              // focused first on the console shelf
    StreamMode mode_ = StreamMode::Play;
    bool modeKnown_ = false;
    uint32_t width_ = 0, height_ = 0;
    float dpi_ = 1.0f;
    Clock::time_point idleSince_{};
    Clock::time_point toastUntil_{};
    Clock::time_point streamSince_{};
    long long sessionSecond_ = -1;
    bool idle_ = false, skipRecord_ = false;
    bool pointerInput_ = false;             // last input was the mouse: do not pull focus back to autofocus
    StreamHealth health_;
    std::vector<std::pair<std::string, std::string>> languages_;   // folder, own name ("pt-BR", "Português (Brasil)")

    UiVm ui_;
    ChromeVm chrome_;
    SplashVm splash_;
    SignVm sign_;
    ConsVm cons_;
    ConnVm conn_;
    StreamVm stream_;
    SetVm set_;
};

// Shared formatting (UiModel.cpp).
std::string KeyName(uint32_t vk);
std::string ModeName(StreamMode m);

} // namespace fluvi::ui
