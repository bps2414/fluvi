// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: RmlUi context, fonts, documents, layout scale, screen changes (crossfade, into and out of the game) and
// the render pass over the caller's frame.
#include "UiImpl.h"

#include <algorithm>
#include <format>

#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

namespace fluvi::ui {
namespace {

constexpr double kIdleFadeSeconds = 0.35;
constexpr auto kSplashMin = std::chrono::milliseconds(900);
// Durations of the RCSS motion tokens the code waits on (--d-cross, --d-bed, --d-veil-out, --d-veil-in).
constexpr auto kCross = std::chrono::milliseconds(180);
constexpr auto kToBed = std::chrono::milliseconds(200);
constexpr auto kLift = std::chrono::milliseconds(350);
constexpr auto kDrop = std::chrono::milliseconds(250);
constexpr auto kToast = std::chrono::seconds(2);

bool IsGame(Screen s) { return s == Screen::Stream; }

} // namespace

// ---- Thread-safe setters: queued, applied in Update() ----

void UiImpl::SetCallbacks(UiCallbacks callbacks) {
    std::lock_guard lock(mutex_);
    pending_.callbacks = std::move(callbacks);
}

void UiImpl::ShowScreen(Screen screen) {
    screen_.store(screen);
    if (screen != Screen::Settings) returnTo_.store(screen);
    transition_.store(true);   // until ApplyScreen() has the new screen up
}

void UiImpl::SetSignIn(const SignInModel& model) {
    std::lock_guard lock(mutex_);
    pending_.sign = model;
}

void UiImpl::SetConsoles(const ConsolesModel& model) {
    std::lock_guard lock(mutex_);
    pending_.cons = model;
}

void UiImpl::SetConnecting(const ConnectingModel& model) {
    std::lock_guard lock(mutex_);
    pending_.conn = model;
}

void UiImpl::SetStats(const OverlayStats& stats) {
    std::lock_guard lock(mutex_);
    pending_.stats = stats;
}

void UiImpl::SetSettings(const AppSettings& settings) {
    hkOverlay_.store(settings.hotkeys.toggleOverlay);
    hkMode_.store(settings.hotkeys.toggleMode);
    hkHud_.store(settings.hotkeys.cycleHud);
    std::lock_guard lock(mutex_);
    pending_.settings = settings;
}

void UiImpl::Resize(uint32_t width, uint32_t height, float dpiScale) {
    std::lock_guard lock(mutex_);
    pending_.size = {width, height};
    pending_.dpi = dpiScale;
}

void UiImpl::ReloadDocuments() {
    std::lock_guard lock(mutex_);
    pending_.reload = true;
}

void UiImpl::SetSplashStatus(const std::string& status) {
    std::lock_guard lock(mutex_);
    pending_.splash = status;
}

void UiImpl::SetWindowChrome(const WindowChromeModel& model) {
    {
        std::lock_guard lock(chromeMutex_);
        chromeVisible_ = model.visible;
    }
    std::lock_guard lock(mutex_);
    pending_.chrome = model;
}

void UiImpl::LanguageChanged() {
    std::lock_guard lock(mutex_);
    pending_.language = true;
}

// ---- Lifetime ----

bool UiImpl::Init(const UiInitDesc& desc, Status& status) {
    if (!desc.device || !desc.queue || desc.width == 0 || desc.height == 0) {
        status = Status::Fail("ui: device, queue and size are required");
        return false;
    }
    assets_ = desc.assetsDir;
    i18nDir_ = assets_.parent_path() / "i18n";
    hwnd_ = desc.hwnd;
    if (!std::filesystem::exists(assets_ / "theme.rcss")) {
        status = Status::Fail(std::format("ui: assets not found in {}", assets_.string()));
        return false;
    }
    // The app loads the language before creating the UI; a host that did not (tests, tools) gets the default one.
    if (!i18n::Has("common.back")) {
        Status langStatus;
        if (!i18n::Load(i18nDir_, i18n::ResolveLanguage("auto", i18nDir_), langStatus))
            FLUVI_WARN("ui", "language files: {}", langStatus.message);
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

    context_ = Rml::CreateContext("fluvi", {static_cast<int>(desc.width), static_cast<int>(desc.height)});
    if (!context_) {
        status = Status::Fail("ui: could not create the RmlUi context");
        return false;
    }
    ApplySize(desc.width, desc.height, desc.dpiScale);
    if (!BuildModel()) {
        status = Status::Fail("ui: could not build the data model");
        return false;
    }
    LoadLanguageNames();
    FillSettings();
    FillStreamFlags();
    CheckCalm();
    if (!LoadDocuments()) {
        status = Status::Fail("ui: could not load the screens (*.rml)");
        return false;
    }
    // The first screen is shown by the first Update(), so whatever the app asked for before it appears directly.
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
        bool fallback;   // also used for glyphs the requested family lacks (arrows, icons)
    };
    const Face faces[] = {
        {"AtkinsonHyperlegibleNext-Regular.ttf", "Atkinson Hyperlegible Next", Rml::Style::FontWeight::Normal, true},
        {"AtkinsonHyperlegibleNext-Medium.ttf", "Atkinson Hyperlegible Next", Rml::Style::FontWeight(500), false},
        {"AtkinsonHyperlegibleNext-SemiBold.ttf", "Atkinson Hyperlegible Next", Rml::Style::FontWeight(600), false},
        {"BricolageGrotesque-Bold.ttf", "Bricolage Grotesque", Rml::Style::FontWeight::Bold, true},
        {"BricolageGrotesque-ExtraBold.ttf", "Bricolage Grotesque", Rml::Style::FontWeight(800), false},
        {"BricolageGrotesqueCondensed-Bold.ttf", "Bricolage Condensed", Rml::Style::FontWeight::Bold, false},
        {"BricolageGrotesqueCondensed-ExtraBold.ttf", "Bricolage Condensed", Rml::Style::FontWeight(800), false},
        {"Phosphor-Bold.ttf", "Phosphor", Rml::Style::FontWeight::Normal, true},
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
    for (Screen s : {Screen::Settings, Screen::Stream}) {
        if (Rml::ElementDocument* d = Doc(s)) {
            d->AddEventListener(Rml::EventId::Click, &listener_);
            d->AddEventListener(Rml::EventId::Keydown, &listener_);
        }
    }
    SetDocClass("calm", calm_);
    SetDocClass("pad", padInput_);
    SetDocClass("inactive", !chrome_.active);
    shownValid_ = false;
    phase_ = Phase::Idle;
    leaving_.reset();
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

void UiImpl::SetDocClass(const char* name, bool on) {
    for (Rml::ElementDocument* d : docs_)
        if (d) d->SetClass(name, on);
}

void UiImpl::ApplySize(uint32_t width, uint32_t height, float dpi) {
    width_ = width;
    height_ = height;
    dpi_ = dpi;
    context_->SetDimensions({static_cast<int>(width), static_cast<int>(height)});
    context_->SetDensityIndependentPixelRatio(DpRatio(width, height, dpi));
    renderer_->SetViewport(static_cast<int>(width), static_cast<int>(height));
}

void UiImpl::CheckCalm() {
    BOOL animations = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) animations = TRUE;
    const bool calm = animations == FALSE;
    if (calm != calm_ || !shownValid_) {
        calm_ = calm;
        SetDocClass("calm", calm_);
    }
}

void UiImpl::Toast(std::string text) {
    ui_.toast = std::move(text);
    ui_.toast_on = true;
    toastUntil_ = Clock::now() + kToast;
    if (model_) model_.DirtyVariable("ui");
}

// ---- Screens and transitions ----

void UiImpl::ShowDoc(Screen screen) {
    Rml::ElementDocument* d = Doc(screen);
    if (!d) return;
    d->SetClass("leaving", false);
    d->SetClass("to-bed", false);
    d->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
    if (screen == Screen::Splash) {
        splashSince_ = Clock::now();
        splashSkip_ = false;
    }
    if (screen == Screen::Settings) wantFocus_.clear();
    if (screen == Screen::Consoles && cons_.list.size() > 1) {
        for (size_t i = 0; i < consoles_.size(); ++i)
            if (consoles_[i].id == lastPlayedId_) wantFocus_ = std::format("console-{}", i);
        if (wantFocus_.empty()) wantFocus_ = "console-0";
    }
}

void UiImpl::HideDoc(Screen screen) {
    Rml::ElementDocument* d = Doc(screen);
    if (!d) return;
    d->Hide();
    d->SetClass("leaving", false);
    d->SetClass("to-bed", false);
    if (screen == Screen::Stream)
        if (Rml::Element* veil = d->GetElementById("veil")) {
            veil->SetClass("lift", false);
            veil->SetClass("drop", false);
        }
}

void UiImpl::StartTransition(Screen from, Screen to) {
    const auto now = Clock::now();
    if (calm_) {
        HideDoc(from);
        ShowDoc(to);
        shown_ = to;
        if (IsGame(to)) OnStreamEntered();
        return;
    }
    if (IsGame(to) && from != Screen::Settings) {
        // Into the game: the screen sinks into the river bed, then the video rises from under a bed-coloured veil.
        if (Rml::ElementDocument* d = Doc(from)) d->SetClass("to-bed", true);
        leaving_ = from;
        shown_ = to;
        phase_ = Phase::ToBed;
        phaseEnd_ = now + kToBed;
        return;
    }
    if (IsGame(from) && to != Screen::Settings) {
        // Out of the game: the veil comes back over the last frame, then the next screen arrives on the bed.
        if (Rml::ElementDocument* d = Doc(from))
            if (Rml::Element* veil = d->GetElementById("veil")) veil->SetClass("drop", true);
        leaving_ = from;
        shown_ = to;
        phase_ = Phase::Drop;
        phaseEnd_ = now + kDrop;
        return;
    }
    // Crossfade: the new screen arrives underneath while the old one fades out on top.
    ShowDoc(to);
    if (Rml::ElementDocument* d = Doc(from)) {
        d->PullToFront();
        d->SetClass("leaving", true);
    }
    leaving_ = from;
    shown_ = to;
    phase_ = Phase::Crossfade;
    phaseEnd_ = now + kCross;
    if (IsGame(to)) OnStreamEntered();
}

void UiImpl::FinishPhase(bool instant) {
    const Phase phase = phase_;
    phase_ = Phase::Idle;
    switch (phase) {
    case Phase::Idle: break;
    case Phase::Crossfade:
    case Phase::Drop:
        if (leaving_) HideDoc(*leaving_);
        leaving_.reset();
        if (phase == Phase::Drop) ShowDoc(shown_);
        break;
    case Phase::ToBed:
        if (leaving_) HideDoc(*leaving_);
        leaving_.reset();
        ShowDoc(shown_);
        OnStreamEntered();
        if (!instant)
            if (Rml::ElementDocument* d = Doc(shown_))
                if (Rml::Element* veil = d->GetElementById("veil")) {
                    veil->SetClass("lift", true);
                    phase_ = Phase::Lift;
                    phaseEnd_ = Clock::now() + kLift;
                }
        break;
    case Phase::Lift:
        if (Rml::ElementDocument* d = Doc(shown_))
            if (Rml::Element* veil = d->GetElementById("veil")) veil->SetClass("lift", false);
        break;
    }
}

void UiImpl::ApplyScreen() {
    const auto now = Clock::now();
    if (phase_ != Phase::Idle && now >= phaseEnd_) FinishPhase(false);

    Screen want = screen_.load();
    const bool overlay = overlay_.load();
    if (!shownValid_) {
        for (size_t i = 0; i < docs_.size(); ++i)
            if (docs_[i] && i != static_cast<size_t>(want)) HideDoc(static_cast<Screen>(i));
        ShowDoc(want);
        shown_ = want;
        shownValid_ = true;
        overlayShown_ = false;
        if (IsGame(want)) OnStreamEntered();
    } else if (want != shown_) {
        // The splash stays until its entrance has played (or a key skipped it).
        const bool holdSplash = shown_ == Screen::Splash && phase_ == Phase::Idle && !splashSkip_ && now - splashSince_ < kSplashMin;
        if (!holdSplash) {
            if (phase_ != Phase::Idle) FinishPhase(true);
            if (want != shown_) StartTransition(shown_, want);
        }
    }

    if (IsGame(shown_) && phase_ == Phase::Idle && overlay != overlayShown_) {
        // Opening the overlay focuses its first control so the D-pad/arrows work immediately.
        if (overlay) wantFocus_ = "mode-play";
        overlayShown_ = overlay;
    } else if (!IsGame(shown_)) {
        overlayShown_ = false;
    }
    transition_.store(phase_ != Phase::Idle || screen_.load() != shown_);
    FillStreamFlags();
}

// ---- Frame ----

void UiImpl::Update() {
    if (!context_) return;
    Pending p;
    {
        std::lock_guard lock(mutex_);
        p = std::move(pending_);
        pending_ = Pending{};
    }
    const Screen shownBefore = shown_;
    const bool modelChanged = p.sign || p.cons || p.conn || p.settings || p.reload || p.language || p.chrome || !p.input.empty();
    if (p.callbacks) cb_ = std::move(*p.callbacks);
    const float dpi = p.dpi.value_or(dpi_);
    if (p.size && p.size->first > 0 && p.size->second > 0 && (p.size->first != width_ || p.size->second != height_ || dpi != dpi_))
        ApplySize(p.size->first, p.size->second, dpi);
    if (p.calmCheck) CheckCalm();
    if (p.settings) {
        settings_ = *p.settings;
        set_.loaded = true;
        if (!modeKnown_) mode_ = settings_.video.mode;
        FillSettings();
        FillStreamFlags();
    }
    if (p.chrome) {
        chrome_.visible = p.chrome->visible;
        chrome_.maximized = p.chrome->maximized;
        chrome_.active = p.chrome->active;
        SetDocClass("inactive", !chrome_.active);
        FillChrome();
    }
    if (p.splash) {
        splashStatus_ = *p.splash;
        splash_.status = splashStatus_;
        model_.DirtyVariable("splash");
    }
    if (p.sign) FillSign(*p.sign);
    if (p.cons) FillCons(*p.cons);
    if (p.conn) FillConn(*p.conn);
    if (p.stats) FillStats(*p.stats);
    if (p.language) {
        LoadLanguageNames();
        RefillText();
        p.reload = true;   // the [key] text of every document is translated when it loads
    }
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

    const auto now = Clock::now();
    if (ui_.toast_on && now >= toastUntil_) {
        ui_.toast_on = false;
        model_.DirtyVariable("ui");
    }
    TickStream();
    // Idle on the stream: nothing visible once the fades are over, so Record() can skip the whole UI pass.
    const bool idle = IsGame(shown_) && phase_ == Phase::Idle && !overlay_.load() && stream_.hud == 0 && !ui_.toast_on &&
                      !ui_.confirm_quit;
    if (idle && !idle_) idleSince_ = now;
    idle_ = idle;
    skipRecord_ = idle && std::chrono::duration<double>(now - idleSince_).count() > kIdleFadeSeconds;

    context_->Update();
    KeepFocus(modelChanged || shown_ != shownBefore);
    // Focus pulls a document to the front: the screen fading out must stay on top until it is gone.
    if (phase_ == Phase::Crossfade && leaving_)
        if (Rml::ElementDocument* d = Doc(*leaving_)) d->PullToFront();
    CacheChrome();
}

void UiImpl::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t frameIndex) {
    if (!context_ || !renderer_ || skipRecord_) return;
    if (!renderer_->BeginFrame(list, target, rtv, frameIndex)) return;
    context_->Render();
    renderer_->EndFrame();
}

std::unique_ptr<Ui> Ui::Create(const UiInitDesc& desc, Status& status) {
    auto ui = std::make_unique<UiImpl>();
    if (!ui->Init(desc, status)) return nullptr;
    status = Status::Ok();
    return ui;
}

} // namespace fluvi::ui
