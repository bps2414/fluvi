// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: RmlUi 6.3 drawn inside our D3D12 frame (same swapchain as the stream, no extra window).
// Screens live in assets/ui/*.rml + *.rcss, editable without rebuild.
//
// Threading: HandleMessage() is called on the window thread and only enqueues. The Set*() model setters
// are thread-safe (copied under a lock). Update() and Record() run on the render thread; callbacks fire
// from inside Update() on the render thread and must not block.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include "fluvi/common/Settings.h"
#include "fluvi/common/Stats.h"
#include "fluvi/core/Status.h"

namespace fluvi::ui {

// Splash is the first screen. When the app moves on from it, the UI keeps it up until its entrance has played
// (at least 900 ms, or until a key/button is pressed) and then crossfades; the app does not need to time it.
// Screen changes crossfade (InTransition() is true meanwhile); with Windows animations off they cut.
// Entering Stream: show it only after the first video frame has been presented under the Connecting screen.
// Leaving Stream: keep presenting the last video frame while InTransition() is true (the veil fades in over it).
enum class Screen : uint8_t { SignIn, Consoles, Connecting, Stream, Settings, Splash };

// Custom window chrome: the UI draws the title bar; the window asks it what is under the cursor.
enum class WindowHit : uint8_t { Client, Caption, Minimize, Maximize, Close };

struct WindowChromeModel {
    bool visible = true;            // false in fullscreen and on the Stream screen
    bool maximized = false;         // the maximize button shows "restore"
    bool active = true;             // window has focus (title bar dims when false)
};

struct SignInModel {
    std::string userCode;           // "ABCD-EFGH", shown large
    std::string verificationUri;    // https://www.microsoft.com/link (QR encodes it)
    std::string status;             // "Aguardando login..." / error text
    bool busy = false;
};

enum class PowerState : uint8_t { Unknown, On, Standby, Off };

struct ConsoleItem {
    std::string id;                 // serverId
    std::string name;
    std::string type;               // "Xbox Series X"
    PowerState power = PowerState::Unknown;
    bool busy = false;              // waking / connecting
};

struct ConsolesModel {
    std::vector<ConsoleItem> consoles;
    std::string accountName;        // gamertag, may be empty
    bool refreshing = false;
    std::string error;
};

enum class ConnectStage : uint8_t { Provisioning, ReadyToConnect, Connecting, Connected, Failed };

struct ConnectingModel {
    std::string consoleName;
    ConnectStage stage = ConnectStage::Provisioning;
    std::string detail;             // error text when Failed
};

struct UiCallbacks {
    std::function<void()> signIn;                         // start the device-code sign-in
    std::function<void()> cancelSignIn;
    std::function<void()> signOut;
    std::function<void()> refreshConsoles;
    std::function<void(const std::string& id)> wake;
    std::function<void(const std::string& id)> connect;
    std::function<void()> cancelConnect;
    std::function<void()> disconnect;                     // from the overlay
    std::function<void(const AppSettings&)> settingsChanged;
    std::function<void(StreamMode)> modeChanged;          // overlay toggle or hotkey
    std::function<void()> quit;                           // already confirmed by the UI when a stream is live
    std::function<void(const std::string& id)> wakeAndConnect;   // "Ligar e jogar": wake, then connect when on
    std::function<void()> minimize;                       // title bar buttons
    std::function<void()> toggleMaximize;
};

struct UiInitDesc {
    HWND hwnd = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;     // direct queue the frame is submitted on
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 0, height = 0;
    uint32_t framesInFlight = 3;             // = swapchain buffer count; per-frame resources ring
    std::filesystem::path assetsDir;         // folder with *.rml, *.rcss, fonts/
    float dpiScale = 1.0f;
};

class Ui {
public:
    static std::unique_ptr<Ui> Create(const UiInitDesc& desc, Status& status);
    virtual ~Ui() = default;

    virtual void SetCallbacks(UiCallbacks callbacks) = 0;

    virtual void ShowScreen(Screen screen) = 0;
    virtual Screen CurrentScreen() const = 0;
    virtual void SetSignIn(const SignInModel& model) = 0;
    virtual void SetConsoles(const ConsolesModel& model) = 0;
    virtual void SetConnecting(const ConnectingModel& model) = 0;
    virtual void SetStats(const OverlayStats& stats) = 0;
    virtual void SetSettings(const AppSettings& settings) = 0;

    // The in-game overlay (frosted glass over the live stream) on Screen::Stream.
    virtual void SetOverlayVisible(bool visible) = 0;
    virtual bool OverlayVisible() const = 0;
    // True when the UI wants the mouse/keyboard (a menu is open); otherwise input belongs to the game.
    virtual bool WantsInput() const = 0;

    // Window thread. Returns true if the UI consumed the message.
    // Title bar: the window answers WM_NCHITTEST with HitTest() and turns the non-client mouse messages over the
    // three buttons into client messages in client pixels (WM_MOUSEMOVE, WM_LBUTTONDOWN with capture so the release
    // arrives as WM_LBUTTONUP, WM_MOUSELEAVE). The UI acts on press + release like any button (callbacks minimize,
    // toggleMaximize, quit with its confirmation); Windows must not act on those buttons itself. Presses on the
    // caption go to DefWindowProc for dragging.
    virtual bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;
    virtual void Resize(uint32_t width, uint32_t height, float dpiScale) = 0;

    // Render thread, once per presented frame: input, data bindings, animations, callbacks.
    virtual void Update() = 0;
    // Draws on top of `target` (already holding the video frame), RENDER_TARGET state on entry and exit.
    // `frameIndex` selects the per-frame ring slot (swapchain CurrentIndex()). The list's descriptor heaps
    // are the UI's own after this call; the caller rebinds if it records more work afterwards.
    virtual void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t frameIndex) = 0;
    // Reload *.rml / *.rcss from disk (F5 in debug builds).
    virtual void ReloadDocuments() = 0;

    // Splash line under the logo ("Verificando sua conta"), already translated.
    virtual void SetSplashStatus(const std::string& status) = 0;
    virtual void SetWindowChrome(const WindowChromeModel& model) = 0;
    // Window thread (WM_NCHITTEST). Client-area pixel coordinates. Client when the chrome is hidden.
    virtual WindowHit HitTest(int x, int y) const = 0;
    // True while a screen change is animating (see Screen).
    virtual bool InTransition() const = 0;
    // Reloads text after fluvi::i18n::Load() switched the language (re-translates every document).
    virtual void LanguageChanged() = 0;
};

// QR code as a square module matrix (true = dark), for the sign-in screen. Exposed for tests.
std::vector<std::vector<bool>> EncodeQr(const std::string& text);

} // namespace fluvi::ui
