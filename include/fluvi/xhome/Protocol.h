// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), include/veyra/xbox/Protocol.h @ c6633121. Changes: fluvi::GamepadState/Rumble instead of Veyra's frame types, resolution messages added.
// The data-channel protocol of Xbox home streaming, as Greenlight's player speaks it
// (unknownskl/greenlight packages/player, MIT): the JSON handshake and messages on "message" and
// "control", and the little-endian binary reports on "input". Pure functions only, tested offline.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fluvi/common/Gamepad.h"

namespace fluvi::xhome {

enum ReportType : uint16_t {
    ReportMetadata = 1, ReportGamepad = 2, ReportPointer = 4, ReportClientMetadata = 8, ReportServerMetadata = 16,
    ReportMouse = 32, ReportKeyboard = 64, ReportVibration = 128,
};

namespace detail {
inline void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
inline void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i)); }
inline void putDouble(std::vector<uint8_t>& b, size_t at, double v) { uint64_t u; std::memcpy(&u, &v, 8); for (int i = 0; i < 8; ++i) b[at + size_t(i)] = uint8_t(u >> (8 * i)); }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
} // namespace detail

// Header: report type (u16), sequence (u32), timestamp in ms (f64) = 14 bytes.
inline std::vector<uint8_t> clientMetadataReport(uint32_t sequence, double timeMs, uint8_t maxTouchPoints = 1) {
    std::vector<uint8_t> b(15, 0);
    detail::put16(b, 0, ReportClientMetadata);
    detail::put32(b, 2, sequence);
    detail::putDouble(b, 6, timeMs);
    b[14] = maxTouchPoints;
    return b;
}

// Greenlight 58e832a1: input/packet.ts _writeMetadataData. Preserve the actual
// RTP key and seven u32 fields; times (ms) share the session's monotonic origin.
struct VideoFeedback {
    uint32_t rtpTimestamp = 0, arrivalMs = 0, submittedMs = 0, decodedMs = 0, renderedMs = 0;
};
inline std::vector<uint8_t> videoFeedbackReport(uint32_t sequence, double timeMs, const VideoFeedback& f) {
    std::vector<uint8_t> b(43, 0);
    detail::put16(b, 0, ReportMetadata); detail::put32(b, 2, sequence); detail::putDouble(b, 6, timeMs); b[14] = 1;
    const auto sent = uint32_t(uint64_t(timeMs));
    const uint32_t values[] = {f.rtpTimestamp, f.arrivalMs, f.submittedMs, f.decodedMs, f.renderedMs, sent, sent};
    for (size_t i = 0; i < 7; ++i) detail::put32(b, 15 + 4 * i, values[i]);
    return b;
}

// One pad of a gamepad report.
struct PadEntry {
    uint8_t index = 0;
    GamepadState state;
};

// One report with up to a few gamepads: count (u8), then per pad 23 bytes: index, buttons, 4 axes,
// 2 triggers, physicality (u32 LE 1) and virtual physicality (u32 written big-endian 1, as the
// reference client does). GamepadState already uses the report's conventions (up-positive sticks,
// 0..65535 triggers, report button bits), so the fields are copied as they are.
inline std::vector<uint8_t> gamepadReport(uint32_t sequence, double timeMs, std::span<const PadEntry> pads) {
    std::vector<uint8_t> b(14 + 1 + 23 * pads.size(), 0);
    detail::put16(b, 0, ReportGamepad);
    detail::put32(b, 2, sequence);
    detail::putDouble(b, 6, timeMs);
    size_t at = 14;
    b[at++] = uint8_t(pads.size());
    for (const auto& p : pads) {
        const GamepadState& s = p.state;
        b[at++] = p.index;
        detail::put16(b, at, s.buttons);
        detail::put16(b, at + 2, uint16_t(s.leftX));
        detail::put16(b, at + 4, uint16_t(s.leftY));
        detail::put16(b, at + 6, uint16_t(s.rightX));
        detail::put16(b, at + 8, uint16_t(s.rightY));
        detail::put16(b, at + 10, s.leftTrigger);
        detail::put16(b, at + 12, s.rightTrigger);
        detail::put32(b, at + 14, 1);
        b[at + 18] = 0; b[at + 19] = 0; b[at + 20] = 0; b[at + 21] = 1;   // big-endian 1
        at += 22;
    }
    return b;
}
inline std::vector<uint8_t> gamepadReport(uint32_t sequence, double timeMs, const GamepadState& pad, uint8_t index = 0) {
    const PadEntry entry{index, pad};
    return gamepadReport(sequence, timeMs, std::span<const PadEntry>(&entry, 1));
}

// Vibration report (type 128) -> fluvi::Rumble. `gamepad`, when given, receives the pad index.
inline std::optional<Rumble> parseRumble(const uint8_t* p, size_t n, uint8_t* gamepad = nullptr) {
    if (n < 13 || p[0] != ReportVibration) return std::nullopt;
    Rumble v;
    if (gamepad) *gamepad = p[3];
    v.leftMotor = p[4]; v.rightMotor = p[5]; v.leftTrigger = p[6]; v.rightTrigger = p[7];
    v.durationMs = detail::get16(p + 8);
    v.delayMs = detail::get16(p + 10);
    v.repeat = p[12];
    return v;
}

// Server metadata: height then width (u32 LE each) after the 2-byte header.
inline bool parseServerMetadata(const uint8_t* p, size_t n, uint32_t* width, uint32_t* height) {
    if (n < 10 || p[0] != ReportServerMetadata) return false;
    *height = detail::get32(p + 2);
    *width = detail::get32(p + 6);
    return true;
}

// --- JSON messages (Protocol.cpp). Each returns the exact text sent on the channel.

// "message" channel wrapper around a content object (already serialised).
std::string wrapMessage(const std::string& target, const std::string& contentJson, const std::string& id);
// Content of /streaming/characteristics/dimensionschanged for a stream of width x height.
std::string dimensionsContent(uint32_t width, uint32_t height);

// Control channel, Greenlight PR #1752 (renderer/lib/streamresolution.ts): both are needed for 1440p.
std::string videoPreferenceMessage(uint32_t width, uint32_t height, uint32_t maxFps = 60);
std::string userRequestedResolutionUpdateMessage(uint32_t height);

// X-MS-Device-Info header of the play request, with the display size of the stream.
std::string deviceInfoJson(uint32_t width, uint32_t height);

// Random RFC 4122 version 4 UUID, lower case.
std::string newUuid4();

} // namespace fluvi::xhome
