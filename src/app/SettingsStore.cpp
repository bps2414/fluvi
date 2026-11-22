// SPDX-License-Identifier: GPL-3.0-only
#include "SettingsStore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <windows.h>

#include "fluvi/core/Log.h"

namespace fluvi::app {

using json = nlohmann::json;

namespace {

constexpr int kVersion = 1;

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Enum <-> name tables. An unknown name keeps the field's default.
template <class E, size_t N>
struct Names {
    const char* names[N];
    const char* Name(E e) const { return names[std::min(static_cast<size_t>(e), N - 1)]; }
    E Parse(const std::string& s, E fallback) const {
        for (size_t i = 0; i < N; ++i)
            if (s == names[i]) return static_cast<E>(i);
        return fallback;
    }
};
constexpr Names<StreamResolution, 3> kResolution{{"720p", "1080p", "1440p"}};
constexpr Names<StreamMode, 2> kMode{{"play", "smooth"}};
constexpr Names<QualityPreset, 3> kPreset{{"light", "standard", "max"}};
constexpr Names<UpscalerChoice, 3> kUpscaler{{"auto", "easu", "fsr4"}};

// Readers: take the value only when it has the right JSON type, else leave `out` (the default) alone.
const json* Section(const json& root, const char* name) {
    const auto it = root.find(name);
    return it != root.end() && it->is_object() ? &*it : nullptr;
}
void Read(const json& o, const char* key, bool& out) {
    if (const auto it = o.find(key); it != o.end() && it->is_boolean()) out = it->get<bool>();
}
void ReadFloat01(const json& o, const char* key, float& out) {
    if (const auto it = o.find(key); it != o.end() && it->is_number()) {
        const double v = it->get<double>();
        if (std::isfinite(v)) out = static_cast<float>(std::clamp(v, 0.0, 1.0));
    }
}
void ReadUint(const json& o, const char* key, uint32_t& out, uint32_t lo, uint32_t hi) {
    if (const auto it = o.find(key); it != o.end() && it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        if (v >= lo && v <= hi) out = static_cast<uint32_t>(v);
    }
}
template <class E, size_t N>
void ReadEnum(const json& o, const char* key, const Names<E, N>& names, E& out) {
    if (const auto it = o.find(key); it != o.end() && it->is_string()) out = names.Parse(it->get<std::string>(), out);
}
double Round4(double v) { return std::round(v * 10000.0) / 10000.0; }

} // namespace

std::filesystem::path SettingsPath() {
    wchar_t override[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"FLUVI_APP_DATA", override, MAX_PATH) > 0) return std::filesystem::path(override) / L"settings.json";
    PWSTR raw = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) base = raw;
    CoTaskMemFree(raw);
    if (base.empty()) base = std::filesystem::current_path();
    return base / L"Fluvi" / L"settings.json";
}

AppSettings ParseSettings(std::string_view text, bool* valid) {
    AppSettings s;
    const json root = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    const bool ok = root.is_object();
    if (valid) *valid = ok;
    if (!ok) return s;

    if (const json* v = Section(root, "video")) {
        ReadEnum(*v, "resolution", kResolution, s.video.resolution);
        ReadUint(*v, "bitrateKbps", s.video.bitrateKbps, 0, 200000);
        ReadEnum(*v, "mode", kMode, s.video.mode);
        ReadFloat01(*v, "deblock", s.video.deblock);
        ReadFloat01(*v, "sharpness", s.video.sharpness);
        Read(*v, "fullscreen", s.video.fullscreen);
        Read(*v, "switchRefreshInFullscreen", s.video.switchRefreshInFullscreen);
        ReadEnum(*v, "preset", kPreset, s.video.preset);
        Read(*v, "presetDetected", s.video.presetDetected);
        ReadEnum(*v, "upscaler", kUpscaler, s.video.upscaler);
        Read(*v, "newFramesOnly", s.video.newFramesOnly);
    }
    if (const json* c = Section(root, "controller")) {
        Read(*c, "rumble", s.controller.rumble);
        uint32_t strength = s.controller.rumbleStrength;
        ReadUint(*c, "rumbleStrength", strength, 0, 100);
        s.controller.rumbleStrength = static_cast<uint8_t>(strength);
    }
    if (const json* a = Section(root, "audio")) {
        ReadFloat01(*a, "volume", s.audio.volume);
        if (const auto it = a->find("deviceId"); it != a->end() && it->is_string()) s.audio.deviceId = FromUtf8(it->get<std::string>());
    }
    if (const json* h = Section(root, "hotkeys")) {
        ReadUint(*h, "toggleOverlay", s.hotkeys.toggleOverlay, 1, 254);
        ReadUint(*h, "toggleMode", s.hotkeys.toggleMode, 1, 254);
        ReadUint(*h, "toggleFullscreen", s.hotkeys.toggleFullscreen, 1, 254);
        ReadUint(*h, "toggleDebug", s.hotkeys.toggleDebug, 1, 254);
    }
    return s;
}

std::string SerializeSettings(const AppSettings& s, std::string_view previous) {
    json root = json::parse(previous.begin(), previous.end(), nullptr, false);
    if (!root.is_object()) root = json::object();
    const auto section = [&root](const char* name) -> json& {
        json& j = root[name];
        if (!j.is_object()) j = json::object();
        return j;
    };
    root["version"] = kVersion;

    json& v = section("video");
    v["resolution"] = kResolution.Name(s.video.resolution);
    v["bitrateKbps"] = s.video.bitrateKbps;
    v["mode"] = kMode.Name(s.video.mode);
    v["deblock"] = Round4(s.video.deblock);
    v["sharpness"] = Round4(s.video.sharpness);
    v["fullscreen"] = s.video.fullscreen;
    v["switchRefreshInFullscreen"] = s.video.switchRefreshInFullscreen;
    v["preset"] = kPreset.Name(s.video.preset);
    v["presetDetected"] = s.video.presetDetected;
    v["upscaler"] = kUpscaler.Name(s.video.upscaler);
    v["newFramesOnly"] = s.video.newFramesOnly;

    json& c = section("controller");
    c["rumble"] = s.controller.rumble;
    c["rumbleStrength"] = s.controller.rumbleStrength;

    json& a = section("audio");
    a["volume"] = Round4(s.audio.volume);
    a["deviceId"] = ToUtf8(s.audio.deviceId);

    json& h = section("hotkeys");
    h["toggleOverlay"] = s.hotkeys.toggleOverlay;
    h["toggleMode"] = s.hotkeys.toggleMode;
    h["toggleFullscreen"] = s.hotkeys.toggleFullscreen;
    h["toggleDebug"] = s.hotkeys.toggleDebug;

    return root.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

LoadedSettings LoadSettings(const std::filesystem::path& file) {
    LoadedSettings out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;   // first run
    out.existed = true;
    out.raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.raw.size() >= 3 && out.raw.compare(0, 3, "\xEF\xBB\xBF") == 0) out.raw.erase(0, 3);   // BOM from a hand edit
    out.settings = ParseSettings(out.raw, &out.valid);
    if (!out.valid) FLUVI_WARN("app", "settings.json is not valid JSON; using the defaults");
    return out;
}

bool WriteTextFileAtomic(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            FLUVI_WARN("app", "cannot write {}", tmp.string());
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            FLUVI_WARN("app", "write to {} failed", tmp.string());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        FLUVI_WARN("app", "cannot replace {} (error {})", file.string(), GetLastError());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---- SettingsSaver ------------------------------------------------------------------------------------------------

SettingsSaver::SettingsSaver(std::filesystem::path file, std::string raw, std::chrono::milliseconds debounce)
    : file_(std::move(file)), raw_(std::move(raw)), debounce_(debounce) {
    thread_ = std::thread([this] { Run(); });
}

SettingsSaver::~SettingsSaver() {
    {
        std::unique_lock lock(mutex_);
        stop_ = true;
        if (dirty_) WriteLocked(lock);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SettingsSaver::Request(const AppSettings& settings) {
    {
        std::lock_guard lock(mutex_);
        pending_ = settings;
        dirty_ = true;
        due_ = std::chrono::steady_clock::now() + debounce_;
    }
    cv_.notify_all();
}

void SettingsSaver::Flush() {
    std::unique_lock lock(mutex_);
    if (dirty_) WriteLocked(lock);
}

unsigned SettingsSaver::Writes() const {
    std::lock_guard lock(mutex_);
    return writes_;
}

void SettingsSaver::WriteLocked(std::unique_lock<std::mutex>&) {
    dirty_ = false;
    std::string text = SerializeSettings(pending_, raw_);
    if (WriteTextFileAtomic(file_, text)) {
        raw_ = std::move(text);
        ++writes_;
    }
}

void SettingsSaver::Run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (!dirty_) {
            cv_.wait(lock);
            continue;
        }
        const auto due = due_;
        cv_.wait_until(lock, due);   // a newer Request() moves due_ and wakes this up
        if (!stop_ && dirty_ && std::chrono::steady_clock::now() >= due_) WriteLocked(lock);
    }
}

} // namespace fluvi::app
