// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: RmlUi system interface, renderer subclass (QR textures), "[key]" translation and the dp scale.
#include "UiSystem.h"

#include <algorithm>
#include <cstdlib>

#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

namespace fluvi::ui {

namespace {

bool IsKeyChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; }

} // namespace

std::string TranslateBrackets(std::string_view text, int* replaced) {
    std::string out;
    out.reserve(text.size());
    int count = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[' && i + 1 < text.size() && text[i + 1] >= 'a' && text[i + 1] <= 'z') {
            size_t j = i + 1;
            bool dot = false;
            while (j < text.size() && IsKeyChar(text[j])) dot |= text[j++] == '.';
            if (j < text.size() && text[j] == ']' && dot && text[j - 1] != '.') {
                out += i18n::Tr(text.substr(i + 1, j - i - 1));
                ++count;
                i = j + 1;
                continue;
            }
        }
        out += text[i++];
    }
    if (replaced) *replaced = count;
    return out;
}

float DpRatio(uint32_t width, uint32_t height, float dpiScale) {
    const float dpi = dpiScale > 0.1f ? dpiScale : 1.0f;
    const float fit = std::min(static_cast<float>(width) / (1600.0f * dpi), static_cast<float>(height) / (900.0f * dpi));
    return dpi * std::clamp(fit, 1.0f, 1.5f);
}

double SystemInterface::GetElapsedTime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    switch (type) {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT: FLUVI_ERROR("rmlui", "{}", message); break;
    case Rml::Log::LT_WARNING: FLUVI_WARN("rmlui", "{}", message); break;
    default: FLUVI_DEBUG("rmlui", "{}", message); break;
    }
    return true;
}

void SystemInterface::JoinPath(Rml::String& translated, const Rml::String& document_path, const Rml::String& path) {
    if (path.rfind("qr:", 0) == 0) {
        translated = path;
        return;
    }
    Rml::SystemInterface::JoinPath(translated, document_path, path);
}

int SystemInterface::TranslateString(Rml::String& translated, const Rml::String& input) {
    if (input.find('[') == Rml::String::npos) {
        translated = input;
        return 0;
    }
    int count = 0;
    translated = TranslateBrackets(input, &count);
    return count;
}

Rml::TextureHandle Renderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
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
            // The river bed colour (--bed), so the code reads as part of the brand on its white card.
            Rml::byte* p = &rgba[(static_cast<size_t>(y) * static_cast<size_t>(px) + static_cast<size_t>(x)) * 4];
            p[0] = 0x0A;
            p[1] = 0x22;
            p[2] = 0x28;
        }
    }
    dimensions = {px, px};
    return GenerateTexture({rgba.data(), rgba.size()}, dimensions);
}

} // namespace fluvi::ui
