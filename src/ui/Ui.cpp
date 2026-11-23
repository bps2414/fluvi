// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: RmlUi context + data model "fluvi" + the DX12 renderer drawing over the caller's frame.
#include "fluvi/ui/Ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include <RmlUi/Core.h>

#include "fluvi/core/Log.h"

#pragma warning(push, 0)
#include "RmlUi_Renderer_DX12.h"
#pragma warning(pop)
#include "RmlUi_Platform_Win32_Fluvi.h"

namespace fluvi::ui {
namespace {

using QrMatrix = std::vector<std::vector<bool>>;

// windowsx.h's GET_X/Y_LPARAM, without its macros that clash with RmlUi's Element API (GetFirstChild...).
int MouseX(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }
int MouseY(LPARAM lp) { return static_cast<int>(static_cast<short>(HIWORD(lp))); }

// ---- View models bound to the "fluvi" data model (names match the *.rml files) ----

struct SignVm {
    Rml::String code, uri_short, status, qr;
    bool busy = false, error = false, has_code = false, has_qr = false, pending = false;   // pending: code/QR requested, not here yet (skeleton)
};

struct ConsoleVm {
    Rml::String id, name, type, power, action;
    bool on = false, busy = false;
};

struct ConsVm {
    std::vector<ConsoleVm> list;
    Rml::String account, initial, error;
    bool has_account = false, refreshing = false, loading = true;   // loading: no list yet (before the first model, or refreshing an empty one)
};

struct ConnVm {
    Rml::String console, title, detail;
    int stage = 0;
    bool failed = false, loading = true;   // loading: console name not known yet
};

struct StreamVm {
    bool overlay = false, corner = false, smooth = false, flip = false, vrr_on = false, has_stats = false;
    Rml::String mode, mode_hint, latency, rtt, fps_game, fps_stream, fps_gen, fps_present, res, res_out, bitrate, present, vrr,
        refresh, timing, loss, warning, corner_line, hk_overlay, hk_mode;
};

struct SetVm {
    Rml::String resolution, bitrate, mode, deblock, sharpness, strength, volume, hk_overlay, hk_mode, hk_full, hk_debug, preset, upscaler;
    int deblock_pct = 0, sharpness_pct = 0, strength_pct = 0, volume_pct = 0;
    bool fullscreen_on = false, switchhz_on = false, rumble_on = false, newframes_on = false, loaded = false;   // loaded: the app sent the settings
};

// ---- Formatting (pt-BR: decimal comma) ----

std::string Num(double v, int decimals) {
    std::string s = decimals == 0 ? std::format("{:.0f}", v) : std::format("{:.{}f}", v, decimals);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string Percent(float v01) { return std::format("{}%", static_cast<int>(std::lround(v01 * 100.0f))); }

std::string KeyName(uint32_t vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    switch (vk) {
    case VK_ESCAPE: return "Esc";
    case VK_TAB: return "Tab";
    case VK_SPACE: return "Espaço";
    case VK_RETURN: return "Enter";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_PAUSE: return "Pause";
    case VK_SCROLL: return "Scroll Lock";
    default: return std::format("0x{:02X}", vk);
    }
}

const char* ResolutionName(StreamResolution r) {
    switch (r) {
    case StreamResolution::R720p: return "720p";
    case StreamResolution::R1080p: return "1080p";
    default: return "1440p";
    }
}

const char* ModeName(StreamMode m) { return m == StreamMode::Smooth ? "Liso" : "Jogar"; }
const char* PresetName(QualityPreset p) { return p == QualityPreset::Light ? "Leve" : p == QualityPreset::Max ? "Máximo" : "Padrão"; }
const char* UpscalerName(UpscalerChoice u) { return u == UpscalerChoice::Easu ? "EASU" : u == UpscalerChoice::Fsr4 ? "FSR 4" : "Automática"; }

std::string ShortUri(std::string uri) {
    for (const char* prefix : {"https://", "http://"})
        if (uri.rfind(prefix, 0) == 0) uri.erase(0, std::char_traits<char>::length(prefix));
    if (uri.rfind("www.", 0) == 0) uri.erase(0, 4);
    if (!uri.empty() && uri.back() == '/') uri.pop_back();
    return uri;
}

// First character of a UTF-8 string, upper-cased when ASCII.
std::string Initial(const std::string& s) {
    if (s.empty()) return {};
    const auto c = static_cast<unsigned char>(s[0]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    len = std::min(len, s.size());
    std::string out = s.substr(0, len);
    if (len == 1) out[0] = static_cast<char>(std::toupper(c));
    return out;
}

// Bitrate choices offered in settings (kbps, 0 = automatic).
constexpr std::array<uint32_t, 7> kBitrates = {0, 10000, 15000, 20000, 30000, 40000, 50000};

// ---- RmlUi system interface: clock, log, `qr:` sources pass through untouched ----

class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT: FLUVI_ERROR("rmlui", "{}", message); break;
        case Rml::Log::LT_WARNING: FLUVI_WARN("rmlui", "{}", message); break;
        default: FLUVI_DEBUG("rmlui", "{}", message); break;
        }
        return true;
    }
    void JoinPath(Rml::String& translated, const Rml::String& document_path, const Rml::String& path) override {
        if (path.rfind("qr:", 0) == 0) {
            translated = path;
            return;
        }
        Rml::SystemInterface::JoinPath(translated, document_path, path);
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// ---- Renderer: the vendored DX12 backend + generated QR textures ----

class Renderer final : public RenderInterface_DX12 {
public:
    Renderer(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, uint32_t frames, const std::map<int, QrMatrix>& qrs)
        : RenderInterface_DX12(device, queue, format, frames), qrs_(qrs) {}

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        if (source.rfind("qr:", 0) != 0) return RenderInterface_DX12::LoadTexture(dimensions, source);
        const auto it = qrs_.find(std::atoi(source.c_str() + 3));
        if (it == qrs_.end() || it->second.empty()) return {};
        const QrMatrix& m = it->second;
        constexpr int kQuiet = 2;
        const int modules = static_cast<int>(m.size()) + 2 * kQuiet;
        const int scale = std::max(1, 560 / modules);
        const int px = modules * scale;
        std::vector<Rml::byte> rgba(static_cast<size_t>(px) * static_cast<size_t>(px) * 4, 0xFF);
        for (int y = 0; y < px; ++y) {
            const int my = y / scale - kQuiet;
            for (int x = 0; x < px; ++x) {
                const int mx = x / scale - kQuiet;
                const bool dark = my >= 0 && mx >= 0 && my < static_cast<int>(m.size()) && mx < static_cast<int>(m.size()) &&
                                  m[static_cast<size_t>(my)][static_cast<size_t>(mx)];
                if (!dark) continue;
                Rml::byte* p = &rgba[(static_cast<size_t>(y) * static_cast<size_t>(px) + static_cast<size_t>(x)) * 4];
                p[0] = 0x0B;
                p[1] = 0x16;
                p[2] = 0x1C;
            }
        }
        dimensions = {px, px};
        return GenerateTexture({rgba.data(), rgba.size()}, dimensions);
    }

private:
    const std::map<int, QrMatrix>& qrs_;
};

// ---- Input queued on the window thread, replayed on the render thread ----

enum class Hotkey : uint8_t { ToggleOverlay, ToggleMode, Reload, Back };

struct InputEvent {
    enum class Kind : uint8_t { MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Char, Leave, Hotkey } kind;
    int a = 0, b = 0, mods = 0;
    float wheel = 0;
};

struct Pending {
    std::optional<UiCallbacks> callbacks;
    std::optional<SignInModel> sign;
    std::optional<ConsolesModel> cons;
    std::optional<ConnectingModel> conn;
    std::optional<OverlayStats> stats;
    std::optional<AppSettings> settings;
    std::optional<std::pair<uint32_t, uint32_t>> size;
    bool reload = false;
    std::vector<InputEvent> input;
};

constexpr std::array<const char*, 5> kDocFiles = {"signin.rml", "consoles.rml", "connecting.rml", "stream.rml", "settings.rml"};
constexpr double kIdleFadeSeconds = 0.35;

class UiImpl;

// Settings rows: click (arrow with data-dir, or the row itself = +1 with wrap) and Left/Right on the focused row.
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

    void SetCallbacks(UiCallbacks callbacks) override {
        std::lock_guard lock(mutex_);
        pending_.callbacks = std::move(callbacks);
    }
    void ShowScreen(Screen screen) override {
        screen_.store(screen);
        if (screen != Screen::Settings) returnTo_.store(screen);
    }
    Screen CurrentScreen() const override { return screen_.load(); }
    void SetSignIn(const SignInModel& model) override {
        std::lock_guard lock(mutex_);
        pending_.sign = model;
    }
    void SetConsoles(const ConsolesModel& model) override {
        std::lock_guard lock(mutex_);
        pending_.cons = model;
    }
    void SetConnecting(const ConnectingModel& model) override {
        std::lock_guard lock(mutex_);
        pending_.conn = model;
    }
    void SetStats(const OverlayStats& stats) override {
        std::lock_guard lock(mutex_);
        pending_.stats = stats;
    }
    void SetSettings(const AppSettings& settings) override {
        hkOverlay_.store(settings.hotkeys.toggleOverlay);
        hkMode_.store(settings.hotkeys.toggleMode);
        std::lock_guard lock(mutex_);
        pending_.settings = settings;
    }
    void SetOverlayVisible(bool visible) override { overlay_.store(visible); }
    bool OverlayVisible() const override { return overlay_.load(); }
    bool WantsInput() const override { return screen_.load() != Screen::Stream || overlay_.load(); }

    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    void Resize(uint32_t width, uint32_t height, float dpiScale) override {
        (void)dpiScale;   // layout scales with the window (designed at 1920x1080), see ApplySize
        std::lock_guard lock(mutex_);
        pending_.size = {width, height};
    }
    void Update() override;
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t frameIndex) override;
    void ReloadDocuments() override {
        std::lock_guard lock(mutex_);
        pending_.reload = true;
    }

    // Called by SettingsListener (render thread, inside Update).
    void StepSetting(const Rml::String& key, int dir, bool wrap);

private:
    void Shutdown();
    bool LoadFonts();
    bool BuildModel();
    bool LoadDocuments();
    void CloseDocuments();
    void ApplySize(uint32_t width, uint32_t height);
    void ApplyScreen();
    void Enqueue(const InputEvent& e) {
        std::lock_guard lock(mutex_);
        if (pending_.input.size() < 512) pending_.input.push_back(e);
    }
    void Replay(const InputEvent& e);
    void OnHotkey(Hotkey key);
    void SetMode(StreamMode mode);

    void FillSign(const SignInModel& m);
    void FillCons(const ConsolesModel& m);
    void FillConn(const ConnectingModel& m);
    void FillStats(const OverlayStats& s);
    void FillSettings();
    void FillStreamFlags();

    std::filesystem::path assets_;
    SystemInterface system_;
    std::unique_ptr<Renderer> renderer_;
    bool rmlInitialised_ = false;
    Rml::Context* context_ = nullptr;
    Rml::DataModelHandle model_;
    std::array<Rml::ElementDocument*, kDocFiles.size()> docs_{};
    SettingsListener listener_;
    std::map<int, QrMatrix> qrs_;
    int qrSerial_ = 0;
    std::string qrText_;

    // Cross-thread state.
    std::mutex mutex_;
    Pending pending_;
    std::atomic<Screen> screen_{Screen::SignIn};
    std::atomic<Screen> returnTo_{Screen::SignIn};   // where "Voltar" in settings goes
    std::atomic<bool> overlay_{false};
    std::atomic<uint32_t> hkOverlay_{VK_F1}, hkMode_{VK_F2};

    // Render-thread state.
    UiCallbacks cb_;
    AppSettings settings_;
    std::vector<ConsoleItem> consoles_;
    Screen shown_ = Screen::SignIn;
    bool shownValid_ = false;
    bool overlayShown_ = false;
    StreamMode mode_ = StreamMode::Play;
    bool modeKnown_ = false;
    uint32_t width_ = 0, height_ = 0;
    std::chrono::steady_clock::time_point idleSince_{};
    std::chrono::steady_clock::time_point toastUntil_{};   // mode-change notice in the corner
    bool toastCorner_ = false;                             // the corner is on only because of that notice
    bool idle_ = false, skipRecord_ = false;
    bool pointerInput_ = false;   // last input was the mouse: do not pull focus back to autofocus

    SignVm sign_;
    ConsVm cons_;
    ConnVm conn_;
    StreamVm stream_;
    SetVm set_;
};

void SettingsListener::ProcessEvent(Rml::Event& event) {
    Rml::Element* el = event.GetTargetElement();
    int dir = 0;
    bool wrap = false;
    if (event.GetId() == Rml::EventId::Keydown) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == Rml::Input::KI_LEFT) dir = -1;
        else if (key == Rml::Input::KI_RIGHT) dir = 1;
        else return;
    } else {
        for (Rml::Element* e = el; e && !e->HasAttribute("data-setting"); e = e->GetParentNode())
            if (e->HasAttribute("data-dir")) {
                dir = e->GetAttribute<int>("data-dir", 0);
                break;
            }
        if (dir == 0) {
            dir = 1;
            wrap = true;
        }
    }
    while (el && !el->HasAttribute("data-setting")) el = el->GetParentNode();
    if (!el) return;
    ui_.StepSetting(el->GetAttribute<Rml::String>("data-setting", ""), dir, wrap);
    event.StopPropagation();
}

bool UiImpl::Init(const UiInitDesc& desc, Status& status) {
    if (!desc.device || !desc.queue || desc.width == 0 || desc.height == 0) {
        status = Status::Fail("ui: device, queue and size are required");
        return false;
    }
    assets_ = desc.assetsDir;
    if (!std::filesystem::exists(assets_ / "theme.rcss")) {
        status = Status::Fail(std::format("ui: assets not found in {}", assets_.string()));
        return false;
    }

    renderer_ = std::make_unique<Renderer>(desc.device, desc.queue, desc.rtvFormat, desc.framesInFlight, qrs_);
    if (!*renderer_) {
        status = Status::Fail("ui: RmlUi DX12 renderer failed to initialise");
        renderer_.reset();
        return false;
    }

    Rml::SetSystemInterface(&system_);
    Rml::SetRenderInterface(renderer_.get());
    if (!Rml::Initialise()) {
        status = Status::Fail("ui: Rml::Initialise failed");
        return false;
    }
    rmlInitialised_ = true;
    if (!LoadFonts()) {
        status = Status::Fail("ui: could not load fonts from assets/ui/fonts");
        return false;
    }

    width_ = desc.width;
    height_ = desc.height;
    context_ = Rml::CreateContext("fluvi", {static_cast<int>(width_), static_cast<int>(height_)});
    if (!context_) {
        status = Status::Fail("ui: could not create the RmlUi context");
        return false;
    }
    ApplySize(width_, height_);
    if (!BuildModel()) {
        status = Status::Fail("ui: could not build the data model");
        return false;
    }
    FillSettings();
    FillStreamFlags();
    if (!LoadDocuments()) {
        status = Status::Fail("ui: could not load the screens (*.rml)");
        return false;
    }
    ApplyScreen();
    return true;
}

void UiImpl::Shutdown() {
    if (renderer_) renderer_->Flush();
    if (context_) {
        CloseDocuments();
        Rml::RemoveContext(context_->GetName());
        context_ = nullptr;
    }
    if (rmlInitialised_) {
        Rml::Shutdown();
        rmlInitialised_ = false;
    }
    renderer_.reset();
}

bool UiImpl::LoadFonts() {
    struct Face {
        const char* file;
        const char* family;
        Rml::Style::FontWeight weight;
        bool fallback;
    };
    const Face faces[] = {
        {"Inter-Regular.ttf", "Inter", Rml::Style::FontWeight::Normal, true},
        {"Inter-Medium.ttf", "Inter", Rml::Style::FontWeight(500), false},
        {"Inter-SemiBold.ttf", "Inter", Rml::Style::FontWeight(600), false},
        {"Inter-Bold.ttf", "Inter", Rml::Style::FontWeight::Bold, false},
        {"InterDisplay-Bold.ttf", "Inter Display", Rml::Style::FontWeight::Bold, false},
    };
    bool ok = true;
    for (const Face& f : faces) {
        const auto path = (assets_ / "fonts" / f.file).generic_u8string();
        if (!Rml::LoadFontFace(Rml::String(path.begin(), path.end()), f.family, Rml::Style::FontStyle::Normal, f.weight, f.fallback)) {
            FLUVI_ERROR("ui", "font load failed: {}", f.file);
            ok = false;
        }
    }
    return ok;
}

bool UiImpl::BuildModel() {
    Rml::DataModelConstructor c = context_->CreateDataModel("fluvi");
    if (!c) return false;

    if (auto s = c.RegisterStruct<SignVm>()) {
        s.RegisterMember("code", &SignVm::code);
        s.RegisterMember("uri_short", &SignVm::uri_short);
        s.RegisterMember("status", &SignVm::status);
        s.RegisterMember("qr", &SignVm::qr);
        s.RegisterMember("busy", &SignVm::busy);
        s.RegisterMember("error", &SignVm::error);
        s.RegisterMember("has_code", &SignVm::has_code);
        s.RegisterMember("has_qr", &SignVm::has_qr);
        s.RegisterMember("pending", &SignVm::pending);
    }
    if (auto s = c.RegisterStruct<ConsoleVm>()) {
        s.RegisterMember("id", &ConsoleVm::id);
        s.RegisterMember("name", &ConsoleVm::name);
        s.RegisterMember("type", &ConsoleVm::type);
        s.RegisterMember("power", &ConsoleVm::power);
        s.RegisterMember("action", &ConsoleVm::action);
        s.RegisterMember("on", &ConsoleVm::on);
        s.RegisterMember("busy", &ConsoleVm::busy);
    }
    c.RegisterArray<std::vector<ConsoleVm>>();
    if (auto s = c.RegisterStruct<ConsVm>()) {
        s.RegisterMember("list", &ConsVm::list);
        s.RegisterMember("account", &ConsVm::account);
        s.RegisterMember("initial", &ConsVm::initial);
        s.RegisterMember("error", &ConsVm::error);
        s.RegisterMember("has_account", &ConsVm::has_account);
        s.RegisterMember("refreshing", &ConsVm::refreshing);
        s.RegisterMember("loading", &ConsVm::loading);
    }
    if (auto s = c.RegisterStruct<ConnVm>()) {
        s.RegisterMember("console", &ConnVm::console);
        s.RegisterMember("title", &ConnVm::title);
        s.RegisterMember("detail", &ConnVm::detail);
        s.RegisterMember("stage", &ConnVm::stage);
        s.RegisterMember("failed", &ConnVm::failed);
        s.RegisterMember("loading", &ConnVm::loading);
    }
    if (auto s = c.RegisterStruct<StreamVm>()) {
        s.RegisterMember("overlay", &StreamVm::overlay);
        s.RegisterMember("has_stats", &StreamVm::has_stats);
        s.RegisterMember("corner", &StreamVm::corner);
        s.RegisterMember("smooth", &StreamVm::smooth);
        s.RegisterMember("flip", &StreamVm::flip);
        s.RegisterMember("vrr_on", &StreamVm::vrr_on);
        s.RegisterMember("mode", &StreamVm::mode);
        s.RegisterMember("mode_hint", &StreamVm::mode_hint);
        s.RegisterMember("latency", &StreamVm::latency);
        s.RegisterMember("rtt", &StreamVm::rtt);
        s.RegisterMember("fps_game", &StreamVm::fps_game);
        s.RegisterMember("fps_stream", &StreamVm::fps_stream);
        s.RegisterMember("fps_gen", &StreamVm::fps_gen);
        s.RegisterMember("fps_present", &StreamVm::fps_present);
        s.RegisterMember("res", &StreamVm::res);
        s.RegisterMember("res_out", &StreamVm::res_out);
        s.RegisterMember("bitrate", &StreamVm::bitrate);
        s.RegisterMember("present", &StreamVm::present);
        s.RegisterMember("vrr", &StreamVm::vrr);
        s.RegisterMember("refresh", &StreamVm::refresh);
        s.RegisterMember("timing", &StreamVm::timing);
        s.RegisterMember("loss", &StreamVm::loss);
        s.RegisterMember("warning", &StreamVm::warning);
        s.RegisterMember("corner_line", &StreamVm::corner_line);
        s.RegisterMember("hk_overlay", &StreamVm::hk_overlay);
        s.RegisterMember("hk_mode", &StreamVm::hk_mode);
    }
    if (auto s = c.RegisterStruct<SetVm>()) {
        s.RegisterMember("resolution", &SetVm::resolution);
        s.RegisterMember("bitrate", &SetVm::bitrate);
        s.RegisterMember("mode", &SetVm::mode);
        s.RegisterMember("deblock", &SetVm::deblock);
        s.RegisterMember("deblock_pct", &SetVm::deblock_pct);
        s.RegisterMember("sharpness", &SetVm::sharpness);
        s.RegisterMember("sharpness_pct", &SetVm::sharpness_pct);
        s.RegisterMember("fullscreen_on", &SetVm::fullscreen_on);
        s.RegisterMember("switchhz_on", &SetVm::switchhz_on);
        s.RegisterMember("preset", &SetVm::preset);
        s.RegisterMember("upscaler", &SetVm::upscaler);
        s.RegisterMember("newframes_on", &SetVm::newframes_on);
        s.RegisterMember("rumble_on", &SetVm::rumble_on);
        s.RegisterMember("loaded", &SetVm::loaded);
        s.RegisterMember("strength", &SetVm::strength);
        s.RegisterMember("strength_pct", &SetVm::strength_pct);
        s.RegisterMember("volume", &SetVm::volume);
        s.RegisterMember("volume_pct", &SetVm::volume_pct);
        s.RegisterMember("hk_overlay", &SetVm::hk_overlay);
        s.RegisterMember("hk_mode", &SetVm::hk_mode);
        s.RegisterMember("hk_full", &SetVm::hk_full);
        s.RegisterMember("hk_debug", &SetVm::hk_debug);
    }
    c.Bind("sign", &sign_);
    c.Bind("cons", &cons_);
    c.Bind("conn", &conn_);
    c.Bind("stream", &stream_);
    c.Bind("set", &set_);

    using Args = const Rml::VariantList&;
    auto on = [&c](const char* name, auto fn) {
        c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, Args args) { fn(args); });
    };
    on("sign_in", [this](Args) { if (cb_.signIn) cb_.signIn(); });
    on("cancel_sign_in", [this](Args) { if (cb_.cancelSignIn) cb_.cancelSignIn(); });
    on("quit", [this](Args) { if (cb_.quit) cb_.quit(); });
    on("open_settings", [this](Args) {
        if (screen_.load() != Screen::Settings) returnTo_.store(screen_.load());
        screen_.store(Screen::Settings);
    });
    on("close_settings", [this](Args) { screen_.store(returnTo_.load()); });
    on("refresh_consoles", [this](Args) { if (cb_.refreshConsoles && !cons_.refreshing) cb_.refreshConsoles(); });
    on("sign_out", [this](Args) { if (cb_.signOut) cb_.signOut(); });
    on("pick", [this](Args args) {
        if (args.empty()) return;
        const Rml::String id = args[0].Get<Rml::String>();
        const auto it = std::find_if(consoles_.begin(), consoles_.end(), [&](const ConsoleItem& c) { return c.id == id; });
        if (it == consoles_.end() || it->busy) return;
        if (it->power == PowerState::On) {
            if (cb_.connect) cb_.connect(id);
        } else if (cb_.wake) {
            cb_.wake(id);
        }
    });
    on("cancel_connect", [this](Args) { if (cb_.cancelConnect) cb_.cancelConnect(); });
    on("set_mode", [this](Args args) {
        if (!args.empty()) SetMode(args[0].Get<int>() == 1 ? StreamMode::Smooth : StreamMode::Play);
    });
    on("toggle_corner", [this](Args) {
        stream_.corner = !stream_.corner;
        toastCorner_ = false;
        model_.DirtyVariable("stream");
    });
    on("disconnect", [this](Args) { if (cb_.disconnect) cb_.disconnect(); });

    model_ = c.GetModelHandle();
    return true;
}

bool UiImpl::LoadDocuments() {
    bool ok = true;
    for (size_t i = 0; i < kDocFiles.size(); ++i) {
        const auto path = (assets_ / kDocFiles[i]).generic_u8string();
        docs_[i] = context_->LoadDocument(Rml::String(path.begin(), path.end()));
        if (!docs_[i]) {
            FLUVI_ERROR("ui", "could not load {}", kDocFiles[i]);
            ok = false;
        }
    }
    if (Rml::ElementDocument* s = docs_[static_cast<size_t>(Screen::Settings)]) {
        s->AddEventListener(Rml::EventId::Click, &listener_);
        s->AddEventListener(Rml::EventId::Keydown, &listener_);
    }
    shownValid_ = false;
    return ok;
}

void UiImpl::CloseDocuments() {
    for (auto*& d : docs_) {
        if (!d) continue;
        d->RemoveEventListener(Rml::EventId::Click, &listener_);
        d->RemoveEventListener(Rml::EventId::Keydown, &listener_);
        d->Close();
        d = nullptr;
    }
}

void UiImpl::ApplySize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    context_->SetDimensions({static_cast<int>(width), static_cast<int>(height)});
    // Screens are laid out in dp for 1920x1080 and scale with the window (a TV-style UI, read from the couch).
    const float ratio = std::min(static_cast<float>(width) / 1920.0f, static_cast<float>(height) / 1080.0f);
    context_->SetDensityIndependentPixelRatio(std::max(0.4f, ratio));
    renderer_->SetViewport(static_cast<int>(width), static_cast<int>(height));
}

void UiImpl::ApplyScreen() {
    const Screen want = screen_.load();
    const bool overlay = overlay_.load();
    if (shownValid_ && want == shown_ && overlay == overlayShown_) return;

    const bool screenChanged = !shownValid_ || want != shown_;
    if (screenChanged) {
        for (size_t i = 0; i < docs_.size(); ++i) {
            if (!docs_[i]) continue;
            const bool visible = i == static_cast<size_t>(want);
            if (visible) docs_[i]->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
            else docs_[i]->Hide();
        }
    }
    if (want == Screen::Stream && overlay && (!overlayShown_ || screenChanged)) {
        // Opening the overlay focuses its first control so the D-pad/arrows work immediately.
        if (auto* d = docs_[static_cast<size_t>(Screen::Stream)]) d->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
    }
    shown_ = want;
    overlayShown_ = overlay;
    shownValid_ = true;
    FillStreamFlags();
}

bool UiImpl::HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool wants = WantsInput();
    const Screen screen = screen_.load();
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const auto vk = static_cast<uint32_t>(wParam);
        if (screen == Screen::Stream && vk == hkOverlay_.load()) {
            if ((lParam & (1 << 30)) == 0) Enqueue({InputEvent::Kind::Hotkey, static_cast<int>(Hotkey::ToggleOverlay)});
            return true;
        }
        if (vk == hkMode_.load() && screen != Screen::Stream) FLUVI_INFO("ui", "mode key ignored: not on the stream screen");
        if (screen == Screen::Stream && vk == hkMode_.load()) {
            if ((lParam & (1 << 30)) == 0) Enqueue({InputEvent::Kind::Hotkey, static_cast<int>(Hotkey::ToggleMode)});
            return true;
        }
#ifndef NDEBUG
        if (vk == VK_F5) {
            Enqueue({InputEvent::Kind::Hotkey, static_cast<int>(Hotkey::Reload)});
            return true;
        }
#endif
        if (!wants || msg == WM_SYSKEYDOWN) return false;
        if (vk == VK_ESCAPE) {
            if ((lParam & (1 << 30)) == 0) Enqueue({InputEvent::Kind::Hotkey, static_cast<int>(Hotkey::Back)});
            return true;
        }
        Enqueue({InputEvent::Kind::KeyDown, RmlWin32Fluvi::ConvertKey(static_cast<int>(vk)), 0, RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    }
    case WM_KEYUP:
        if (!wants) return false;
        Enqueue({InputEvent::Kind::KeyUp, RmlWin32Fluvi::ConvertKey(static_cast<int>(wParam)), 0, RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    case WM_CHAR:
        if (!wants) return false;
        if (wParam >= 32 && wParam != 127) Enqueue({InputEvent::Kind::Char, static_cast<int>(wParam)});
        return true;
    case WM_MOUSEMOVE:
        if (!wants) return false;
        Enqueue({InputEvent::Kind::MouseMove, MouseX(lParam), MouseY(lParam), RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        if (!wants) return false;
        const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
        Enqueue({InputEvent::Kind::MouseMove, MouseX(lParam), MouseY(lParam), RmlWin32Fluvi::GetKeyModifierState()});
        Enqueue({down ? InputEvent::Kind::MouseDown : InputEvent::Kind::MouseUp, button, 0, RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    }
    case WM_MOUSEWHEEL: {
        if (!wants) return false;
        InputEvent e{InputEvent::Kind::Wheel};
        e.wheel = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        e.mods = RmlWin32Fluvi::GetKeyModifierState();
        Enqueue(e);
        return true;
    }
    case WM_MOUSELEAVE:
        Enqueue({InputEvent::Kind::Leave});
        return false;
    default: return false;
    }
}

void UiImpl::Replay(const InputEvent& e) {
    using K = InputEvent::Kind;
    switch (e.kind) {
    case K::MouseMove: pointerInput_ = true; context_->ProcessMouseMove(e.a, e.b, e.mods); break;
    case K::MouseDown: context_->ProcessMouseButtonDown(e.a, e.mods); break;
    case K::MouseUp: context_->ProcessMouseButtonUp(e.a, e.mods); break;
    case K::Wheel: context_->ProcessMouseWheel(Rml::Vector2f(0.0f, e.wheel), e.mods); break;
    case K::KeyDown: pointerInput_ = false; context_->ProcessKeyDown(static_cast<Rml::Input::KeyIdentifier>(e.a), e.mods); break;
    case K::KeyUp: context_->ProcessKeyUp(static_cast<Rml::Input::KeyIdentifier>(e.a), e.mods); break;
    case K::Char: context_->ProcessTextInput(static_cast<Rml::Character>(e.a)); break;
    case K::Leave: context_->ProcessMouseLeave(); break;
    case K::Hotkey: OnHotkey(static_cast<Hotkey>(e.a)); break;
    }
}

void UiImpl::OnHotkey(Hotkey key) {
    const Screen screen = screen_.load();
    switch (key) {
    case Hotkey::ToggleOverlay:
        if (screen == Screen::Stream) overlay_.store(!overlay_.load());
        break;
    case Hotkey::ToggleMode:
        SetMode(mode_ == StreamMode::Smooth ? StreamMode::Play : StreamMode::Smooth);
        break;
    case Hotkey::Reload:
        ReloadDocuments();
        break;
    case Hotkey::Back:
        if (screen == Screen::Settings) screen_.store(returnTo_.load());
        else if (screen == Screen::Stream && overlay_.load()) overlay_.store(false);
        else if (screen == Screen::Connecting && cb_.cancelConnect) cb_.cancelConnect();
        break;
    }
}

void UiImpl::SetMode(StreamMode mode) {
    const bool changed = mode != mode_;
    if (changed && screen_.load() == Screen::Stream) {
        // Visible feedback without blocking the pad: the corner line (it names the mode) shows for 2 s.
        if (!stream_.corner) { stream_.corner = true; toastCorner_ = true; }
        toastUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    mode_ = mode;
    modeKnown_ = true;
    FillStreamFlags();
    if (changed && cb_.modeChanged) cb_.modeChanged(mode);
}

void UiImpl::Update() {
    if (!context_) return;
    Pending p;
    {
        std::lock_guard lock(mutex_);
        p = std::move(pending_);
        pending_ = Pending{};
    }
    const Screen shownBefore = shown_;
    const bool modelChanged = p.sign || p.cons || p.conn || p.settings || p.reload || !p.input.empty();
    if (p.callbacks) cb_ = std::move(*p.callbacks);
    if (p.size && (p.size->first != width_ || p.size->second != height_) && p.size->first > 0 && p.size->second > 0)
        ApplySize(p.size->first, p.size->second);
    if (p.settings) {
        settings_ = *p.settings;
        set_.loaded = true;
        if (!modeKnown_) mode_ = settings_.video.mode;
        FillSettings();
        FillStreamFlags();
    }
    if (p.sign) FillSign(*p.sign);
    if (p.cons) FillCons(*p.cons);
    if (p.conn) FillConn(*p.conn);
    if (p.stats) FillStats(*p.stats);
    if (p.reload) {
        Rml::Factory::ClearStyleSheetCache();
        Rml::Factory::ClearTemplateCache();
        CloseDocuments();
        context_->Update();   // closed documents are released on the next update
        LoadDocuments();
        FLUVI_INFO("ui", "documents reloaded");
    }

    ApplyScreen();
    for (const InputEvent& e : p.input) {
        Replay(e);
        ApplyScreen();   // a click may switch screens; later events go to the new one
    }

    if (toastCorner_ && std::chrono::steady_clock::now() >= toastUntil_) {
        toastCorner_ = false;
        stream_.corner = false;
        if (model_) model_.DirtyVariable("stream");
    }
    // Idle on the stream: nothing visible once the fade-out is over, so Record() can skip the whole UI pass.
    const bool idle = screen_.load() == Screen::Stream && !overlay_.load() && !stream_.corner;
    const auto now = std::chrono::steady_clock::now();
    if (idle && !idle_) idleSince_ = now;
    idle_ = idle;
    skipRecord_ = idle && std::chrono::duration<double>(now - idleSince_).count() > kIdleFadeSeconds;

    context_->Update();

    // Keep something focused for D-pad/keyboard use: autofocus targets often appear only after data arrives (data-if,
    // data-for), after the document was shown.
    if (Rml::ElementDocument* doc = docs_[static_cast<size_t>(shown_)]; doc && !pointerInput_ && (modelChanged || shown_ != shownBefore)) {
        Rml::Element* focus = context_->GetFocusElement();
        if (!focus || focus == doc || focus->GetOwnerDocument() != doc) doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
    }
}

void UiImpl::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t frameIndex) {
    if (!context_ || !renderer_ || skipRecord_) return;
    if (!renderer_->BeginFrame(list, target, rtv, frameIndex)) return;
    context_->Render();
    renderer_->EndFrame();
}

// ---- Model -> view model ----

void UiImpl::FillSign(const SignInModel& m) {
    sign_.code = m.userCode;
    sign_.has_code = !m.userCode.empty();
    sign_.uri_short = m.verificationUri.empty() ? "microsoft.com/link" : ShortUri(m.verificationUri);
    sign_.status = m.status;
    sign_.busy = m.busy;
    sign_.error = !m.busy && !m.status.empty();   // the model has no error flag: a status while idle is a failure
    if (m.verificationUri != qrText_) {
        qrText_ = m.verificationUri;
        QrMatrix qr = EncodeQr(qrText_);
        if (qr.empty()) {
            sign_.qr.clear();
        } else {
            qrs_[++qrSerial_] = std::move(qr);
            while (qrs_.size() > 2) qrs_.erase(qrs_.begin());
            sign_.qr = std::format("qr:{}", qrSerial_);
        }
    }
    sign_.has_qr = !sign_.qr.empty();
    sign_.pending = m.busy && (!sign_.has_code || !sign_.has_qr);
    model_.DirtyVariable("sign");
}

void UiImpl::FillCons(const ConsolesModel& m) {
    consoles_ = m.consoles;
    cons_.list.clear();
    for (const ConsoleItem& c : m.consoles) {
        ConsoleVm v;
        v.id = c.id;
        v.name = c.name;
        v.type = c.type;
        v.busy = c.busy;
        v.on = c.power == PowerState::On;
        switch (c.power) {
        case PowerState::On: v.power = "Ligado"; break;
        case PowerState::Standby: v.power = "Em espera"; break;
        case PowerState::Off: v.power = "Desligado"; break;
        default: v.power = "Sem resposta"; break;
        }
        v.action = v.on ? "Conectar" : "Ligar";
        cons_.list.push_back(std::move(v));
    }
    cons_.account = m.accountName;
    cons_.initial = Initial(m.accountName);
    cons_.has_account = !m.accountName.empty();
    cons_.refreshing = m.refreshing;
    cons_.loading = m.refreshing && m.consoles.empty();   // first model received: only a refresh shows placeholders now
    cons_.error = m.error;
    model_.DirtyVariable("cons");
}

void UiImpl::FillConn(const ConnectingModel& m) {
    conn_.console = m.consoleName;
    conn_.loading = m.consoleName.empty();
    conn_.failed = m.stage == ConnectStage::Failed;
    switch (m.stage) {
    case ConnectStage::Provisioning:
        conn_.stage = 0;
        conn_.title = "Preparando a sessão";
        conn_.detail = "O console está reservando uma sessão de Remote Play.";
        break;
    case ConnectStage::ReadyToConnect:
        conn_.stage = 1;
        conn_.title = "Sessão pronta";
        conn_.detail = "Abrindo o canal de vídeo, áudio e controle.";
        break;
    case ConnectStage::Connecting:
        conn_.stage = 2;
        conn_.title = "Conectando";
        conn_.detail = "Negociando a rota até o console.";
        break;
    case ConnectStage::Connected:
        conn_.stage = 3;
        conn_.title = "Conectado";
        conn_.detail = "A imagem já vai aparecer.";
        break;
    case ConnectStage::Failed:
        conn_.title = "Não foi possível conectar";
        conn_.detail = m.detail.empty() ? "O console não respondeu. Confira se ele está ligado e tente de novo." : m.detail;
        break;
    }
    model_.DirtyVariable("conn");
}

void UiImpl::FillStats(const OverlayStats& s) {
    if (!modeKnown_ || s.mode != mode_) {
        mode_ = s.mode;
        modeKnown_ = true;
    }
    auto fps = [](double v) { return v > 0 ? Num(v, 0) : std::string("–"); };
    stream_.latency = s.latencyMs > 0 ? Num(s.latencyMs, s.latencyMs < 10 ? 1 : 0) : "–";
    stream_.rtt = s.rttMs > 0 ? Num(s.rttMs, 0) + " ms" : "–";
    stream_.fps_game = fps(s.gameFps);
    stream_.fps_stream = fps(s.streamFps);
    stream_.fps_gen = s.mode == StreamMode::Smooth ? fps(s.generatedFps) : "0";
    stream_.fps_present = fps(s.presentedFps);
    stream_.res = s.streamWidth ? std::format("{}×{}", s.streamWidth, s.streamHeight) : "–";
    stream_.res_out = s.outputWidth ? std::format("{}×{}", s.outputWidth, s.outputHeight) : "–";
    stream_.bitrate = s.bitrateMbps > 0 ? Num(s.bitrateMbps, 1) + " Mb/s" : "–";
    switch (s.presentMode) {
    case PresentMode::IndependentFlip: stream_.present = "Independent flip"; break;
    case PresentMode::HardwareOverlay: stream_.present = "Overlay de hardware"; break;
    case PresentMode::Composed: stream_.present = "Composto pelo Windows"; break;
    default: stream_.present = "Detectando"; break;
    }
    if (s.tearing) stream_.present += ", com tearing";
    stream_.flip = s.presentMode == PresentMode::IndependentFlip || s.presentMode == PresentMode::HardwareOverlay;
    stream_.vrr = !s.vrr ? "Não detectado" : *s.vrr ? "Ativo" : "Desligado";
    stream_.vrr_on = s.vrr.value_or(false);
    stream_.refresh = s.refreshHz > 0 ? Num(s.refreshHz, std::fabs(s.refreshHz - std::round(s.refreshHz)) > 0.005 ? 2 : 0) + " Hz" : "–";
    stream_.timing = std::format("{} / {} ms", Num(s.decodeMs, 1), Num(s.gpuMs, 1));
    stream_.loss = s.packetsLost == 0 && s.framesDropped == 0
                       ? "Nenhuma"
                       : std::format("{} pacotes, {} quadros", s.packetsLost, s.framesDropped);
    stream_.warning = s.warning;
    stream_.has_stats = true;
    stream_.corner_line = std::format("{} ms   {} fps", stream_.latency, stream_.fps_present);
    FillStreamFlags();
}

void UiImpl::FillStreamFlags() {
    stream_.overlay = overlay_.load();
    stream_.smooth = mode_ == StreamMode::Smooth;
    stream_.mode = ModeName(mode_);
    stream_.mode_hint = mode_ == StreamMode::Smooth ? "Gera quadros intermediários: 30 viram 60 fps, com um pouco mais de latência."
                                                    : "Só os quadros do console, com a menor latência possível.";
    stream_.hk_overlay = KeyName(settings_.hotkeys.toggleOverlay);
    stream_.hk_mode = KeyName(settings_.hotkeys.toggleMode);
    if (model_) model_.DirtyVariable("stream");
}

void UiImpl::FillSettings() {
    const AppSettings& s = settings_;
    set_.resolution = ResolutionName(s.video.resolution);
    set_.bitrate = s.video.bitrateKbps == 0 ? "Automático" : std::format("{} Mb/s", s.video.bitrateKbps / 1000);
    set_.mode = ModeName(s.video.mode);
    set_.deblock = s.video.deblock <= 0.001f ? "Desligado" : Percent(s.video.deblock);
    set_.deblock_pct = static_cast<int>(std::lround(s.video.deblock * 100.0f));
    set_.sharpness = s.video.sharpness <= 0.001f ? "Desligada" : Percent(s.video.sharpness);
    set_.sharpness_pct = static_cast<int>(std::lround(s.video.sharpness * 100.0f));
    set_.fullscreen_on = s.video.fullscreen;
    set_.switchhz_on = s.video.switchRefreshInFullscreen;
    set_.preset = PresetName(s.video.preset);
    set_.upscaler = UpscalerName(s.video.upscaler);
    set_.newframes_on = s.video.newFramesOnly;
    set_.rumble_on = s.controller.rumble;
    set_.strength = std::format("{}%", s.controller.rumbleStrength);
    set_.strength_pct = s.controller.rumbleStrength;
    set_.volume = Percent(s.audio.volume);
    set_.volume_pct = static_cast<int>(std::lround(s.audio.volume * 100.0f));
    set_.hk_overlay = KeyName(s.hotkeys.toggleOverlay);
    set_.hk_mode = KeyName(s.hotkeys.toggleMode);
    set_.hk_full = KeyName(s.hotkeys.toggleFullscreen);
    set_.hk_debug = KeyName(s.hotkeys.toggleDebug);
    if (model_) model_.DirtyVariable("set");
}

void UiImpl::StepSetting(const Rml::String& key, int dir, bool wrap) {
    // Steps an index in [0, count) by dir, clamping (arrows, Left/Right) or wrapping (click/Enter on the row).
    auto step = [dir, wrap](int index, int count) {
        const int next = index + dir;
        if (wrap) return ((next % count) + count) % count;
        return std::clamp(next, 0, count - 1);
    };
    auto stepUnit = [&](float v) {   // 0..1 in tenths
        return static_cast<float>(step(static_cast<int>(std::lround(v * 10.0f)), 11)) / 10.0f;
    };
    VideoSettings& v = settings_.video;
    if (key == "resolution") {
        v.resolution = static_cast<StreamResolution>(step(static_cast<int>(v.resolution), 3));
    } else if (key == "bitrate") {
        int index = 0;
        for (size_t i = 0; i < kBitrates.size(); ++i)
            if (kBitrates[i] <= v.bitrateKbps) index = static_cast<int>(i);
        v.bitrateKbps = kBitrates[static_cast<size_t>(step(index, static_cast<int>(kBitrates.size())))];
    } else if (key == "mode") {
        v.mode = static_cast<StreamMode>(step(static_cast<int>(v.mode), 2));
    } else if (key == "deblock") {
        v.deblock = stepUnit(v.deblock);
    } else if (key == "sharpness") {
        v.sharpness = stepUnit(v.sharpness);
    } else if (key == "fullscreen") {
        v.fullscreen = wrap ? !v.fullscreen : dir > 0;
    } else if (key == "switchhz") {
        v.switchRefreshInFullscreen = wrap ? !v.switchRefreshInFullscreen : dir > 0;
    } else if (key == "preset") {
        v.preset = static_cast<QualityPreset>(step(static_cast<int>(v.preset), 3));
        v.presetDetected = true;   // the user chose: DetectPreset() must not overwrite it
    } else if (key == "upscaler") {
        v.upscaler = static_cast<UpscalerChoice>(step(static_cast<int>(v.upscaler), 3));
    } else if (key == "newframes") {
        v.newFramesOnly = wrap ? !v.newFramesOnly : dir > 0;
    } else if (key == "rumble") {
        settings_.controller.rumble = wrap ? !settings_.controller.rumble : dir > 0;
    } else if (key == "strength") {
        settings_.controller.rumbleStrength = static_cast<uint8_t>(step(settings_.controller.rumbleStrength / 10, 11) * 10);
    } else if (key == "volume") {
        settings_.audio.volume = static_cast<float>(step(static_cast<int>(std::lround(settings_.audio.volume * 20.0f)), 21)) / 20.0f;
    } else {
        return;
    }
    FillSettings();
    if (cb_.settingsChanged) cb_.settingsChanged(settings_);
}

} // namespace

std::unique_ptr<Ui> Ui::Create(const UiInitDesc& desc, Status& status) {
    auto ui = std::make_unique<UiImpl>();
    if (!ui->Init(desc, status)) return nullptr;
    status = Status::Ok();
    return ui;
}

} // namespace fluvi::ui
