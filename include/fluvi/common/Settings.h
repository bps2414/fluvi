// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: user settings. The UI edits them, wave 2 persists them as JSON in %LOCALAPPDATA%\Fluvi\settings.json,
// the pipeline/session read them. Plain data, copyable.
#pragma once
#include <cstdint>
#include <string>

namespace fluvi {

enum class StreamResolution : uint8_t { R720p, R1080p, R1440p };
enum class StreamMode : uint8_t { Play, Smooth };   // "Jogar" (no FG, lowest latency) / "Liso" (FG 30->60)

// Quality presets by GPU power ("leve / padrão / máximo", PLANO §1). The pipeline maps them (ResolveChainConfig in
// fluvi/gpu/QualityChain.h): Light = EASU + cheap downscale + no deblock; Standard = FSR 4 only on GPUs where it is
// cheap, Lanczos3 downscale; Max = FSR 4 wherever the GPU can run it.
enum class QualityPreset : uint8_t { Light, Standard, Max };
// Upscaler override for the "bigger than the stream" path. Auto = the preset decides.
enum class UpscalerChoice : uint8_t { Auto, Easu, Fsr4 };

inline uint32_t StreamWidth(StreamResolution r) { return r == StreamResolution::R720p ? 1280u : r == StreamResolution::R1080p ? 1920u : 2560u; }
inline uint32_t StreamHeight(StreamResolution r) { return r == StreamResolution::R720p ? 720u : r == StreamResolution::R1080p ? 1080u : 1440u; }

struct VideoSettings {
    StreamResolution resolution = StreamResolution::R1440p;
    uint32_t bitrateKbps = 0;          // 0 = the highest the console accepts (xhome decides the cap)
    StreamMode mode = StreamMode::Play;
    float deblock = 0.5f;              // 0 = off .. 1 = strong
    float sharpness = 0.6f;            // 0 = off .. 1 = max (pipeline maps to RCAS stops)
    bool fullscreen = true;            // borderless fullscreen (never exclusive)
    bool switchRefreshInFullscreen = false;   // no VRR: switch the monitor to 120 Hz in fullscreen, restore on exit
    // Wave 2 (agent G); persisted by the app (src/app/SettingsStore.cpp).
    QualityPreset preset = QualityPreset::Standard;   // first run: the app sets it from DetectPreset() (fluvi/gpu/QualityChain.h)
    bool presetDetected = false;       // true once DetectPreset() has chosen `preset` for this machine (first run done)
    UpscalerChoice upscaler = UpscalerChoice::Auto;
    bool newFramesOnly = false;        // Play mode: present only new frames, at the content cadence (for Lossless Scaling)
};

struct ControllerSettings {
    bool rumble = true;
    uint8_t rumbleStrength = 100;      // percent
};

struct AudioSettings {
    float volume = 1.0f;               // 0..1
    std::wstring deviceId;             // empty = default render endpoint
};

// Virtual-key codes (VK_*). Controller shortcut for the overlay: hold View + Menu.
struct HotkeySettings {
    uint32_t toggleOverlay = 0x70;     // VK_F1
    uint32_t toggleMode = 0x71;        // VK_F2  (Jogar <-> Liso)
    uint32_t toggleFullscreen = 0x7A;  // VK_F11
    uint32_t toggleDebug = 0x7B;       // VK_F12 (Dear ImGui panel)
};

struct AppSettings {
    VideoSettings video;
    ControllerSettings controller;
    AudioSettings audio;
    HotkeySettings hotkeys;
};

} // namespace fluvi
