// SPDX-License-Identifier: GPL-3.0-only
// JSON messages of the xHome data channels. Shapes follow Greenlight (unknownskl/greenlight, MIT):
// packages/player message.ts / control.ts, and PR #1752 renderer/lib/streamresolution.ts for the resolution ones.
#include "fluvi/xhome/Protocol.h"

#include <nlohmann/json.hpp>
#include <random>

namespace fluvi::xhome {

using json = nlohmann::json;

std::string newUuid4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        const uint64_t r = rng();
        std::memcpy(b + i, &r, 8);
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    return s;
}

std::string wrapMessage(const std::string& target, const std::string& contentJson, const std::string& id) {
    return json{{"type", "Message"}, {"content", contentJson}, {"id", id}, {"target", target}, {"cv", ""}}.dump();
}

std::string dimensionsContent(uint32_t width, uint32_t height) {
    return json{{"horizontal", width}, {"vertical", height}, {"preferredWidth", width}, {"preferredHeight", height},
                {"safeAreaLeft", 0}, {"safeAreaTop", 0}, {"safeAreaRight", width}, {"safeAreaBottom", height},
                {"supportsCustomResolution", true}}.dump();
}

std::string videoPreferenceMessage(uint32_t width, uint32_t height, uint32_t maxFps) {
    return json{{"message", "VideoPreference"}, {"type", "VideoPreference"},
                {"resolution", {{"width", width}, {"height", height}}},
                {"dimensions", {{"width", width}, {"height", height}}},
                {"maxFPS", maxFps}}.dump();
}

std::string userRequestedResolutionUpdateMessage(uint32_t height) {
    return json{{"message", "userRequestedResolutionUpdate"}, {"resolutionAlias", std::to_string(height)}}.dump();
}

// Greenlight xcloudapi.ts startStream deviceInfo (display size from PR #1752).
std::string deviceInfoJson(uint32_t width, uint32_t height) {
    json env = {{"clientAppId", "Microsoft.GamingApp"}, {"clientAppType", "native"}, {"clientAppVersion", "2203.1001.4.0"},
                {"clientSdkVersion", "8.5.2"}, {"httpEnvironment", "prod"}, {"sdkInstallId", ""}};
    json hw = {{"make", "Microsoft"}, {"model", "Surface Pro"}, {"sdktype", "native"}};
    json os = {{"name", "Windows 11"}, {"ver", "22631.2715"}, {"platform", "desktop"}};
    json display = {{"dimensions", {{"widthInPixels", width}, {"heightInPixels", height}}},
                    {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}}};
    json root;
    root["appInfo"]["env"] = env;
    root["dev"]["hw"] = hw;
    root["dev"]["os"] = os;
    root["dev"]["displayInfo"] = display;
    return root.dump();
}

} // namespace fluvi::xhome
