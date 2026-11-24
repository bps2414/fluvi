// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: the RmlUi system interface (clock, log, "[key]" translation, `qr:` sources) and the renderer subclass
// that generates QR textures.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

#include <RmlUi/Core/SystemInterface.h>

#include "UiHelpers.h"

#pragma warning(push, 0)
#include "RmlUi_Renderer_DX12.h"
#pragma warning(pop)

namespace fluvi::ui {

using QrMatrix = std::vector<std::vector<bool>>;

class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void JoinPath(Rml::String& translated, const Rml::String& document_path, const Rml::String& path) override;
    int TranslateString(Rml::String& translated, const Rml::String& input) override;

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// The vendored DX12 backend plus `qr:<n>` textures built from the matrices the UI keeps.
class Renderer final : public RenderInterface_DX12 {
public:
    Renderer(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, uint32_t frames, const std::map<int, QrMatrix>& qrs)
        : RenderInterface_DX12(device, queue, format, frames), qrs_(qrs) {}

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;

private:
    const std::map<int, QrMatrix>& qrs_;
};

} // namespace fluvi::ui
