// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: settings rows (settings screen and the overlay's quick settings): values shown, stepping, language list.
#include "UiImpl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

#ifndef FLUVI_VERSION
#define FLUVI_VERSION "dev"
#endif

namespace fluvi::ui {
namespace {

using i18n::Tr;

// Bitrate choices offered in settings (kbps, 0 = automatic).
constexpr std::array<uint32_t, 7> kBitrates = {0, 10000, 15000, 20000, 30000, 40000, 50000};

std::string Percent(float v01) { return std::format("{}%", static_cast<int>(std::lround(v01 * 100.0f))); }

const char* ResolutionName(StreamResolution r) {
    switch (r) {
    case StreamResolution::R720p: return "720p";
    case StreamResolution::R1080p: return "1080p";
    default: return "1440p";
    }
}

std::string PresetName(QualityPreset p) {
    return Tr(p == QualityPreset::Light ? "settings.preset_light" : p == QualityPreset::Max ? "settings.preset_max" : "settings.preset_standard");
}

std::string UpscalerName(UpscalerChoice u) {
    return u == UpscalerChoice::Easu ? "EASU" : u == UpscalerChoice::Fsr4 ? "FSR 4" : Tr("settings.upscaler_auto");
}

std::string HudName(HudMode m) {
    return Tr(m == HudMode::Compact ? "settings.hud_compact" : m == HudMode::Full ? "settings.hud_full" : "settings.hud_off");
}

std::string CornerName(HudCorner c) {
    switch (c) {
    case HudCorner::TopRight: return Tr("settings.corner_tr");
    case HudCorner::BottomLeft: return Tr("settings.corner_bl");
    case HudCorner::BottomRight: return Tr("settings.corner_br");
    default: return Tr("settings.corner_tl");
    }
}

} // namespace

void SettingsListener::ProcessEvent(Rml::Event& event) {
    Rml::Element* el = event.GetTargetElement();
    int dir = 0;
    bool wrap = false;
    if (event.GetId() == Rml::EventId::Keydown) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == Rml::Input::KI_RIGHT && el && el->IsClassSet("navitem") && !el->HasAttribute("data-setting")) {
            ui_.FocusFirstRow();   // from a section in the nav into its rows
            event.StopPropagation();
            return;
        }
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

void UiImpl::FocusFirstRow() {
    Rml::ElementDocument* doc = Doc(Screen::Settings);
    if (!doc) return;
    Rml::ElementList rows;
    doc->QuerySelectorAll(rows, ".list .row");
    for (Rml::Element* row : rows)
        if (row->IsVisible(true)) {
            row->Focus(true);
            return;
        }
}

void UiImpl::LoadLanguageNames() {
    languages_.clear();
    for (const std::string& folder : i18n::AvailableLanguages(i18nDir_)) {
        i18n::Catalog catalog;
        Status status;
        std::string name = folder;
        if (catalog.AddFolder(i18nDir_ / folder, status)) {
            if (const std::string* own = catalog.Find("settings.language_name")) name = *own;
        } else {
            FLUVI_WARN("ui", "language {}: {}", folder, status.message);
        }
        languages_.emplace_back(folder, std::move(name));
    }
}

void UiImpl::FillSettings() {
    const AppSettings& s = settings_;
    set_.resolution = ResolutionName(s.video.resolution);
    set_.bitrate = s.video.bitrateKbps == 0 ? Tr("settings.bitrate_auto") : Tr("common.mbps", {{"n", std::to_string(s.video.bitrateKbps / 1000)}});
    set_.mode = ModeName(s.video.mode);
    set_.deblock = s.video.deblock <= 0.001f ? Tr("settings.deblock_off") : Percent(s.video.deblock);
    set_.deblock_pct = static_cast<int>(std::lround(s.video.deblock * 100.0f));
    set_.sharpness = s.video.sharpness <= 0.001f ? Tr("settings.sharpness_off") : Percent(s.video.sharpness);
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
    set_.hk_hud = KeyName(s.hotkeys.cycleHud);
    set_.hk_full = KeyName(s.hotkeys.toggleFullscreen);
    set_.hk_debug = KeyName(s.hotkeys.toggleDebug);
    set_.hud = HudName(s.hud.mode);
    set_.corner = CornerName(s.hud.corner);
    auto ownName = [this](const std::string& folder) {
        for (const auto& [f, name] : languages_)
            if (f == folder) return name;
        return folder;
    };
    if (s.language.empty() || s.language == "auto")
        set_.language = Tr("settings.language_auto", {{"name", ownName(i18n::ResolveLanguage("auto", i18nDir_))}});
    else
        set_.language = ownName(s.language);
    set_.version = Tr("settings.version", {{"version", FLUVI_VERSION}});
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
    } else if (key == "hud") {
        settings_.hud.mode = static_cast<HudMode>(step(static_cast<int>(settings_.hud.mode), 3));
    } else if (key == "corner") {
        settings_.hud.corner = static_cast<HudCorner>(step(static_cast<int>(settings_.hud.corner), 4));
    } else if (key == "language") {
        // "auto" first, then every language folder; the app loads the new one and calls LanguageChanged().
        std::vector<std::string> options{"auto"};
        for (const auto& [folder, name] : languages_) options.push_back(folder);
        int index = 0;
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i] == settings_.language) index = static_cast<int>(i);
        const int next = step(index, static_cast<int>(options.size()));
        if (next == index) return;
        settings_.language = options[static_cast<size_t>(next)];
    } else {
        return;
    }
    FillSettings();
    FillStreamFlags();
    if (cb_.settingsChanged) cb_.settingsChanged(settings_);
}

} // namespace fluvi::ui
