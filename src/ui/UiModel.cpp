// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: the "fluvi" data model: view-model structs, event callbacks, and the app models turned into text.
#include "UiImpl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>

#include <shellapi.h>

#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

namespace fluvi::ui {
namespace {

using i18n::FormatNumber;
using i18n::Tr;

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

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Dash() { return "–"; }

// Bars in the latency graph: one per quarter second over the 10 s window.
constexpr size_t kGraphBars = 40;

} // namespace

std::string KeyName(uint32_t vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    switch (vk) {
    case VK_ESCAPE: return "Esc";
    case VK_TAB: return "Tab";
    case VK_SPACE: return Tr("common.key_space");
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

std::string ModeName(StreamMode m) { return Tr(m == StreamMode::Smooth ? "common.mode_smooth" : "common.mode_play"); }

bool UiImpl::BuildModel() {
    Rml::DataModelConstructor c = context_->CreateDataModel("fluvi");
    if (!c) return false;

    if (auto s = c.RegisterStruct<UiVm>()) {
        s.RegisterMember("toast", &UiVm::toast);
        s.RegisterMember("toast_on", &UiVm::toast_on);
        s.RegisterMember("confirm_quit", &UiVm::confirm_quit);
    }
    if (auto s = c.RegisterStruct<ChromeVm>()) {
        s.RegisterMember("visible", &ChromeVm::visible);
        s.RegisterMember("maximized", &ChromeVm::maximized);
        s.RegisterMember("active", &ChromeVm::active);
    }
    if (auto s = c.RegisterStruct<SplashVm>()) s.RegisterMember("status", &SplashVm::status);
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
        s.RegisterMember("single", &ConsVm::single);
        s.RegisterMember("menu", &ConsVm::menu);
    }
    if (auto s = c.RegisterStruct<ConnVm>()) {
        s.RegisterMember("console", &ConnVm::console);
        s.RegisterMember("title", &ConnVm::title);
        s.RegisterMember("detail", &ConnVm::detail);
        s.RegisterMember("stage", &ConnVm::stage);
        s.RegisterMember("failed", &ConnVm::failed);
        s.RegisterMember("loading", &ConnVm::loading);
    }
    c.RegisterArray<std::vector<int>>();
    if (auto s = c.RegisterStruct<StreamVm>()) {
        s.RegisterMember("overlay", &StreamVm::overlay);
        s.RegisterMember("smooth", &StreamVm::smooth);
        s.RegisterMember("flip", &StreamVm::flip);
        s.RegisterMember("vrr_on", &StreamVm::vrr_on);
        s.RegisterMember("has_stats", &StreamVm::has_stats);
        s.RegisterMember("details", &StreamVm::details);
        s.RegisterMember("band", &StreamVm::band);
        s.RegisterMember("hud", &StreamVm::hud);
        s.RegisterMember("corner", &StreamVm::corner);
        s.RegisterMember("graph", &StreamVm::graph);
        s.RegisterMember("console", &StreamVm::console);
        s.RegisterMember("session", &StreamVm::session);
        s.RegisterMember("mode", &StreamVm::mode);
        s.RegisterMember("smooth_desc", &StreamVm::smooth_desc);
        s.RegisterMember("latency", &StreamVm::latency);
        s.RegisterMember("latency_cap", &StreamVm::latency_cap);
        s.RegisterMember("band_word", &StreamVm::band_word);
        s.RegisterMember("fps_game", &StreamVm::fps_game);
        s.RegisterMember("fps_stream", &StreamVm::fps_stream);
        s.RegisterMember("fps_gen", &StreamVm::fps_gen);
        s.RegisterMember("fps_present", &StreamVm::fps_present);
        s.RegisterMember("fps_line", &StreamVm::fps_line);
        s.RegisterMember("res", &StreamVm::res);
        s.RegisterMember("res_out", &StreamVm::res_out);
        s.RegisterMember("bitrate", &StreamVm::bitrate);
        s.RegisterMember("present", &StreamVm::present);
        s.RegisterMember("vrr", &StreamVm::vrr);
        s.RegisterMember("refresh", &StreamVm::refresh);
        s.RegisterMember("timing", &StreamVm::timing);
        s.RegisterMember("loss", &StreamVm::loss);
        s.RegisterMember("loss_short", &StreamVm::loss_short);
        s.RegisterMember("warning", &StreamVm::warning);
        s.RegisterMember("hk_overlay", &StreamVm::hk_overlay);
        s.RegisterMember("hk_mode", &StreamVm::hk_mode);
        s.RegisterMember("hk_hud", &StreamVm::hk_hud);
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
        s.RegisterMember("hk_hud", &SetVm::hk_hud);
        s.RegisterMember("hk_full", &SetVm::hk_full);
        s.RegisterMember("hk_debug", &SetVm::hk_debug);
        s.RegisterMember("hud", &SetVm::hud);
        s.RegisterMember("corner", &SetVm::corner);
        s.RegisterMember("language", &SetVm::language);
        s.RegisterMember("version", &SetVm::version);
        s.RegisterMember("section", &SetVm::section);
    }
    c.Bind("ui", &ui_);
    c.Bind("chrome", &chrome_);
    c.Bind("splash", &splash_);
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
    on("open_browser", [this](Args) { OpenBrowser(); });
    on("copy_code", [this](Args) { CopyCode(); });
    on("quit", [this](Args) { RequestQuit(); });
    on("confirm_quit", [this](Args) {
        ui_.confirm_quit = false;
        dialog_.store(false);
        model_.DirtyVariable("ui");
        if (cb_.quit) cb_.quit();
    });
    on("cancel_quit", [this](Args) {
        ui_.confirm_quit = false;
        dialog_.store(false);
        model_.DirtyVariable("ui");
    });
    on("chrome_min", [this](Args) { if (cb_.minimize) cb_.minimize(); });
    on("chrome_max", [this](Args) { if (cb_.toggleMaximize) cb_.toggleMaximize(); });
    on("open_settings", [this](Args) {
        if (screen_.load() != Screen::Settings) returnTo_.store(screen_.load());
        screen_.store(Screen::Settings);
        transition_.store(true);
    });
    on("close_settings", [this](Args) {
        screen_.store(returnTo_.load());
        transition_.store(true);
    });
    on("section", [this](Args args) {
        if (args.empty()) return;
        const int section = std::clamp(args[0].Get<int>(), 0, 6);
        if (section == set_.section) return;
        set_.section = section;
        model_.DirtyVariable("set");
    });
    on("refresh_consoles", [this](Args) { if (cb_.refreshConsoles && !cons_.refreshing) cb_.refreshConsoles(); });
    on("account_menu", [this](Args) {
        cons_.menu = !cons_.menu;
        if (cons_.menu) wantFocus_ = "menu-switch";
        model_.DirtyVariable("cons");
    });
    on("switch_account", [this](Args) {
        cons_.menu = false;
        model_.DirtyVariable("cons");
        // Signing out goes back to the sign-in screen synchronously; the new sign-in starts from there.
        if (cb_.signOut) cb_.signOut();
        if (cb_.signIn) cb_.signIn();
    });
    on("sign_out", [this](Args) {
        cons_.menu = false;
        model_.DirtyVariable("cons");
        if (cb_.signOut) cb_.signOut();
    });
    on("pick", [this](Args args) {
        if (!args.empty()) Pick(args[0].Get<Rml::String>());
    });
    on("cancel_connect", [this](Args) { if (cb_.cancelConnect) cb_.cancelConnect(); });
    on("retry_connect", [this](Args) { RetryConnect(); });
    on("set_mode", [this](Args args) {
        if (!args.empty()) SetMode(args[0].Get<int>() == 1 ? StreamMode::Smooth : StreamMode::Play);
    });
    on("toggle_details", [this](Args) {
        stream_.details = !stream_.details;
        model_.DirtyVariable("stream");
    });
    on("disconnect", [this](Args) { if (cb_.disconnect) cb_.disconnect(); });

    model_ = c.GetModelHandle();
    return true;
}

// ---- Actions ----

void UiImpl::Pick(const std::string& id) {
    const auto it = std::find_if(consoles_.begin(), consoles_.end(), [&](const ConsoleItem& c) { return c.id == id; });
    if (it == consoles_.end() || it->busy) return;
    connectId_ = id;
    lastPlayedId_ = id;
    if (it->power == PowerState::On) {
        connectWake_ = false;
        if (cb_.connect) cb_.connect(id);
    } else if (cb_.wakeAndConnect) {
        connectWake_ = true;
        cb_.wakeAndConnect(id);
    } else if (cb_.wake) {
        connectWake_ = false;
        cb_.wake(id);
    }
}

void UiImpl::RetryConnect() {
    if (connectId_.empty()) {
        if (cb_.cancelConnect) cb_.cancelConnect();
        return;
    }
    if (connectWake_ && cb_.wakeAndConnect) cb_.wakeAndConnect(connectId_);
    else if (cb_.connect) cb_.connect(connectId_);
}

void UiImpl::RequestQuit() {
    // Quitting from a live game asks first: one stray click must not end the session.
    if (screen_.load() == Screen::Stream || returnTo_.load() == Screen::Stream) {
        ui_.confirm_quit = true;
        dialog_.store(true);
        wantFocus_ = "dlg-stay";
        model_.DirtyVariable("ui");
        return;
    }
    if (cb_.quit) cb_.quit();
}

void UiImpl::OpenBrowser() {
    if (!lastSign_ || lastSign_->verificationUri.empty()) return;
    std::string url = lastSign_->verificationUri;
    if (!lastSign_->userCode.empty()) url += "?otc=" + lastSign_->userCode;   // the page opens with the code filled in
    const std::wstring wide = Wide(url);
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) FLUVI_WARN("ui", "could not open the browser ({})", static_cast<long long>(result));
}

void UiImpl::CopyCode() {
    if (!lastSign_ || lastSign_->userCode.empty()) return;
    const std::wstring text = Wide(lastSign_->userCode);
    if (!OpenClipboard(hwnd_)) {
        FLUVI_WARN("ui", "clipboard busy");
        return;
    }
    bool ok = false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            std::memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    if (ok) Toast(Tr("signin.copied"));
}

void UiImpl::SetMode(StreamMode mode) {
    const bool changed = mode != mode_;
    mode_ = mode;
    modeKnown_ = true;
    FillStreamFlags();
    if (changed && screen_.load() == Screen::Stream) Toast(Tr("stream.toast_mode", {{"mode", ModeName(mode)}}));
    if (changed && cb_.modeChanged) cb_.modeChanged(mode);
}

void UiImpl::CycleHud() {
    HudMode& hud = settings_.hud.mode;
    hud = hud == HudMode::Off ? HudMode::Compact : hud == HudMode::Compact ? HudMode::Full : HudMode::Off;
    FillSettings();
    FillStreamFlags();
    if (cb_.settingsChanged) cb_.settingsChanged(settings_);
}

void UiImpl::OnStreamEntered() {
    streamSince_ = Clock::now();
    sessionSecond_ = -1;
    health_.Reset();
    stream_.graph.assign(kGraphBars, 0);
    stream_.console = conn_.console;
    stream_.session = FormatSessionTime(0);
    FillStreamFlags();
    const std::string mode = ModeName(mode_);
    Toast(conn_.console.empty() ? Tr("stream.toast_connected_plain", {{"mode", mode}})
                                : Tr("stream.toast_connected", {{"console", conn_.console}, {"mode", mode}}));
}

void UiImpl::TickStream() {
    if (shown_ != Screen::Stream) return;
    const double seconds = std::chrono::duration<double>(Clock::now() - streamSince_).count();
    const auto whole = static_cast<long long>(seconds);
    if (whole == sessionSecond_) return;
    sessionSecond_ = whole;
    stream_.session = FormatSessionTime(seconds);
    if (!health_.Empty()) stream_.graph = health_.Bars(seconds, kGraphBars);
    model_.DirtyVariable("stream");
}

// ---- Model -> view model ----

void UiImpl::FillChrome() { model_.DirtyVariable("chrome"); }

void UiImpl::FillSign(const SignInModel& m) {
    lastSign_ = m;
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
    lastCons_ = m;
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
        case PowerState::On: v.power = Tr("consoles.power_on"); break;
        case PowerState::Standby: v.power = Tr("consoles.power_standby"); break;
        case PowerState::Off: v.power = Tr("consoles.power_off"); break;
        default: v.power = Tr("consoles.power_unknown"); break;
        }
        v.action = Tr(v.on ? "consoles.play" : "consoles.wake_play");
        cons_.list.push_back(std::move(v));
    }
    cons_.single = cons_.list.size() == 1;
    cons_.account = m.accountName;
    cons_.initial = Initial(m.accountName);
    cons_.has_account = !m.accountName.empty();
    if (!cons_.has_account) cons_.menu = false;
    cons_.refreshing = m.refreshing;
    cons_.loading = m.refreshing && m.consoles.empty();   // first model received: only a refresh shows placeholders now
    cons_.error = m.error;
    model_.DirtyVariable("cons");
}

void UiImpl::FillConn(const ConnectingModel& m) {
    lastConn_ = m;
    conn_.console = m.consoleName;
    conn_.loading = m.consoleName.empty();
    conn_.failed = m.stage == ConnectStage::Failed;
    switch (m.stage) {
    case ConnectStage::Provisioning:
        conn_.stage = 0;
        conn_.title = Tr("conn.title_session");
        conn_.detail = Tr("conn.detail_session");
        break;
    case ConnectStage::ReadyToConnect:
        conn_.stage = 1;
        conn_.title = Tr("conn.title_channel");
        conn_.detail = Tr("conn.detail_channel");
        break;
    case ConnectStage::Connecting:
        conn_.stage = 2;
        conn_.title = Tr("conn.title_route");
        conn_.detail = Tr("conn.detail_route");
        break;
    case ConnectStage::Connected:
        conn_.stage = 3;
        conn_.title = Tr("conn.title_connected");
        conn_.detail = Tr("conn.detail_connected");
        break;
    case ConnectStage::Failed:
        conn_.title = Tr("conn.title_failed");
        conn_.detail = m.detail.empty() ? Tr("conn.detail_failed") : m.detail;
        break;
    }
    model_.DirtyVariable("conn");
}

void UiImpl::FillStats(const OverlayStats& s) {
    lastStats_ = s;
    if (!modeKnown_ || s.mode != mode_) {
        mode_ = s.mode;
        modeKnown_ = true;
    }
    if (shown_ == Screen::Stream && s.latencyMs > 0) {
        health_.Add(std::chrono::duration<double>(Clock::now() - streamSince_).count(), s.latencyMs, s.rttMs, s.packetsLost);
    }
    auto fps = [](double v) { return v > 0 ? FormatNumber(v, 0) : Dash(); };
    const double total = std::max(0.0, s.latencyMs) + std::max(0.0, s.rttMs);
    stream_.latency = total > 0 ? FormatNumber(total, total < 10 ? 1 : 0) : Dash();
    stream_.latency_cap = s.latencyMs > 0 ? Tr("stream.latency_cap", {{"client", FormatNumber(s.latencyMs, s.latencyMs < 10 ? 1 : 0)},
                                                                      {"network", FormatNumber(std::max(0.0, s.rttMs), 0)}})
                                          : Tr("stream.latency_wait");
    const HealthBand band = health_.Empty() ? ClassifyHealth(total, 0.0, 0) : health_.Band();
    stream_.band = static_cast<int>(band);
    stream_.band_word = Tr(std::format("stream.band_{}", stream_.band));
    stream_.fps_game = fps(s.gameFps);
    stream_.fps_stream = fps(s.streamFps);
    stream_.fps_gen = s.mode == StreamMode::Smooth ? fps(s.generatedFps) : "0";
    stream_.fps_present = fps(s.presentedFps);
    stream_.fps_line = std::format("{} / {}", stream_.fps_game, stream_.fps_present);
    stream_.res = s.streamWidth ? std::format("{}×{}", s.streamWidth, s.streamHeight) : Dash();
    stream_.res_out = s.outputWidth ? std::format("{}×{}", s.outputWidth, s.outputHeight) : Dash();
    stream_.bitrate = s.bitrateMbps > 0 ? Tr("common.mbps", {{"n", FormatNumber(s.bitrateMbps, 1)}}) : Dash();
    switch (s.presentMode) {
    case PresentMode::IndependentFlip: stream_.present = Tr("stream.present_flip"); break;
    case PresentMode::HardwareOverlay: stream_.present = Tr("stream.present_overlay"); break;
    case PresentMode::Composed: stream_.present = Tr("stream.present_composed"); break;
    default: stream_.present = Tr("stream.present_detecting"); break;
    }
    if (s.tearing) stream_.present += Tr("stream.present_tearing");
    stream_.flip = s.presentMode == PresentMode::IndependentFlip || s.presentMode == PresentMode::HardwareOverlay;
    stream_.vrr = Tr(!s.vrr ? "stream.vrr_unknown" : *s.vrr ? "stream.vrr_on" : "stream.vrr_off");
    stream_.vrr_on = s.vrr.value_or(false);
    stream_.refresh = s.refreshHz > 0
                          ? FormatNumber(s.refreshHz, std::fabs(s.refreshHz - std::round(s.refreshHz)) > 0.005 ? 2 : 0) + " Hz"
                          : Dash();
    stream_.timing = std::format("{} / {} ms", FormatNumber(s.decodeMs, 1), FormatNumber(s.gpuMs, 1));
    const bool clean = s.packetsLost == 0 && s.framesDropped == 0;
    stream_.loss = clean ? Tr("stream.loss_none")
                         : std::format("{}, {}", i18n::TrCount("stream.packets", static_cast<int64_t>(s.packetsLost)),
                                       i18n::TrCount("stream.frames", static_cast<int64_t>(s.framesDropped)));
    stream_.loss_short = clean ? Tr("stream.loss_none") : std::format("{} / {}", s.packetsLost, s.framesDropped);
    stream_.warning = s.warning;
    stream_.has_stats = true;
    FillStreamFlags();
}

void UiImpl::FillStreamFlags() {
    stream_.overlay = overlay_.load();
    stream_.smooth = mode_ == StreamMode::Smooth;
    stream_.mode = ModeName(mode_);
    stream_.smooth_desc = Tr("stream.mode_smooth_d");
    stream_.hud = static_cast<int>(settings_.hud.mode);
    stream_.corner = static_cast<int>(settings_.hud.corner);
    stream_.hk_overlay = KeyName(settings_.hotkeys.toggleOverlay);
    stream_.hk_mode = KeyName(settings_.hotkeys.toggleMode);
    stream_.hk_hud = KeyName(settings_.hotkeys.cycleHud);
    if (model_) model_.DirtyVariable("stream");
}

void UiImpl::RefillText() {
    // Copies: the Fill* functions store their argument back into these.
    if (lastSign_) { const SignInModel m = *lastSign_; FillSign(m); }
    if (lastCons_) { const ConsolesModel m = *lastCons_; FillCons(m); }
    if (lastConn_) { const ConnectingModel m = *lastConn_; FillConn(m); }
    if (lastStats_) { const OverlayStats m = *lastStats_; FillStats(m); }
    FillSettings();
    FillStreamFlags();
    splash_.status = splashStatus_;
    model_.DirtyVariable("splash");
}

} // namespace fluvi::ui
