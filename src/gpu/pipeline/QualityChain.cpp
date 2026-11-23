// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: the quality chain, presets, GPU detection and the new-frames gate (contract:
// include/fluvi/gpu/QualityChain.h). Builds on the passes of ImagePasses.cpp.
#include "fluvi/gpu/QualityChain.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <vector>

#include <windows.h>

#include "fluvi/core/Log.h"

namespace fluvi::gpu {

// ---- path planning ------------------------------------------------------------------------------------------------------

const char* PathName(QualityPath path) {
    switch (path) {
        case QualityPath::Downscale: return "downscale";
        case QualityPath::Native: return "native";
        case QualityPath::UpscaleFsr4: return "upscale-fsr4";
        case QualityPath::UpscaleEasu: return "upscale-easu";
    }
    return "?";
}

ChainLayout PlanLayout(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH, Upscaler wanted, bool fsr4Usable) {
    ChainLayout l;
    l.srcW = srcW;
    l.srcH = srcH;
    l.outW = outW;
    l.outH = outH;
    if (!srcW || !srcH || !outW || !outH) return l;
    // Largest size with the stream's aspect that fits the output (integer maths, rounded to nearest).
    const uint64_t a = uint64_t(outW) * srcH, b = uint64_t(outH) * srcW;
    if (a <= b) {
        l.fitW = outW;
        l.fitH = static_cast<uint32_t>((uint64_t(srcH) * outW + srcW / 2) / srcW);
    } else {
        l.fitH = outH;
        l.fitW = static_cast<uint32_t>((uint64_t(srcW) * outH + srcH / 2) / srcH);
    }
    l.fitW = std::clamp(l.fitW, 1u, outW);
    l.fitH = std::clamp(l.fitH, 1u, outH);
    // An output within 2 px of the stream's aspect (1366x768 is 16:9 to the pixel grid) is filled, not left with a
    // 1 px bar; that is 2 px of stretch at most. An exact 1:1 fit is left alone.
    if (!(l.fitW == srcW && l.fitH == srcH)) {
        if (outW - l.fitW <= 2) l.fitW = outW;
        if (outH - l.fitH <= 2) l.fitH = outH;
    }
    l.offX = (outW - l.fitW) / 2;
    l.offY = (outH - l.fitH) / 2;
    l.scale = double(l.fitW) / srcW;
    if (l.fitW == srcW && l.fitH == srcH) {
        l.path = QualityPath::Native;
    } else if (l.fitW <= srcW && l.fitH <= srcH) {
        l.path = QualityPath::Downscale;
    } else {
        const bool inRange = double(l.fitW) <= kFsr4MaxScale * srcW + 0.5 && double(l.fitH) <= kFsr4MaxScale * srcH + 0.5;
        l.path = (wanted == Upscaler::Fsr4 && fsr4Usable && inRange) ? QualityPath::UpscaleFsr4 : QualityPath::UpscaleEasu;
    }
    return l;
}

// ---- presets and GPU detection ----------------------------------------------------------------------------------------

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The first run of digits after `key` in `name` ("rx 6600" -> 6600), or 0.
uint32_t NumberAfter(const std::string& name, const char* key) {
    const size_t p = name.find(key);
    if (p == std::string::npos) return 0;
    size_t i = p + std::char_traits<char>::length(key);
    while (i < name.size() && name[i] == ' ') ++i;
    uint32_t n = 0;
    bool any = false;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
        n = n * 10 + static_cast<uint32_t>(name[i] - '0');
        any = true;
        ++i;
    }
    return any ? n : 0;
}
constexpr uint64_t kGiB = 1ull << 30;
} // namespace

GpuCaps CapsFromDevice(Device& device) {
    GpuCaps c;
    const AdapterInfo& info = device.Info();
    c.vendor = info.vendor;
    c.dedicatedVram = info.dedicatedVideoMemory;
    if (!info.name.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, info.name.data(), static_cast<int>(info.name.size()), nullptr, 0, nullptr, nullptr);
        c.name.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, info.name.data(), static_cast<int>(info.name.size()), c.name.data(), n, nullptr, nullptr);
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    if (SUCCEEDED(device.D3D()->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof arch))) c.integrated = arch.UMA != FALSE;
    return c;
}

Fsr4Tier ClassifyFsr4(const GpuCaps& caps) {
    if (caps.vendor != Vendor::Amd) return Fsr4Tier::None;
    const std::string name = Lower(caps.name);
    uint32_t n = NumberAfter(name, "rx ");
    if (!n) n = NumberAfter(name, " w");   // Radeon PRO W7800 / W6800
    if (n >= 1000) {
        if (n >= 9000) return Fsr4Tier::Fast;                                   // RDNA 4 (FP8)
        if (n >= 7000 && n < 8000) return caps.integrated ? Fsr4Tier::Slow : Fsr4Tier::Fast;   // RDNA 3 (INT8)
        if (n >= 6000 && n < 7000) return Fsr4Tier::Slow;                       // RDNA 2 (INT8)
        return Fsr4Tier::None;
    }
    if (n) return Fsr4Tier::None;   // RX 400/500 and older
    if (caps.integrated) {
        // "Radeon 780M", "890M", "8060S": RDNA 3 / 3.5 APUs. Older Vega APUs ("Radeon(TM) Graphics", "Vega 8") are out.
        const uint32_t apu = NumberAfter(name, "radeon ");
        if (apu >= 740) return Fsr4Tier::Slow;
    }
    return Fsr4Tier::None;
}

const char* TierName(Fsr4Tier tier) {
    switch (tier) {
        case Fsr4Tier::None: return "none";
        case Fsr4Tier::Slow: return "slow";
        case Fsr4Tier::Fast: return "fast";
    }
    return "?";
}

QualityPreset DetectPreset(const GpuCaps& caps) {
    if (caps.integrated || caps.dedicatedVram < 4 * kGiB) return QualityPreset::Light;
    const std::string name = Lower(caps.name);
    switch (caps.vendor) {
        case Vendor::Amd:
            return ClassifyFsr4(caps) == Fsr4Tier::Fast && caps.dedicatedVram >= 12 * kGiB ? QualityPreset::Max : QualityPreset::Standard;
        case Vendor::Nvidia:
            if (name.find("gtx") != std::string::npos || name.find(" mx") != std::string::npos || name.find("gt ") != std::string::npos)
                return QualityPreset::Light;
            return caps.dedicatedVram >= 12 * kGiB ? QualityPreset::Max : QualityPreset::Standard;
        case Vendor::Intel:
            return caps.dedicatedVram >= 8 * kGiB ? QualityPreset::Standard : QualityPreset::Light;
        default:
            return QualityPreset::Light;
    }
}

ChainConfig ResolveChainConfig(const VideoSettings& s, Fsr4Tier tier) {
    ChainConfig c;
    c.deblock = std::clamp(s.deblock, 0.f, 1.f);
    c.sharpness = std::clamp(s.sharpness, 0.f, 1.f);
    switch (s.preset) {
        case QualityPreset::Light:
            c.upscaler = Upscaler::Easu;
            c.downscale = DownscaleFilter::Bicubic;
            c.deblock = 0.f;
            break;
        case QualityPreset::Standard:
            c.upscaler = tier == Fsr4Tier::Fast ? Upscaler::Fsr4 : Upscaler::Easu;
            c.downscale = DownscaleFilter::Lanczos3;
            break;
        case QualityPreset::Max:
            c.upscaler = tier != Fsr4Tier::None ? Upscaler::Fsr4 : Upscaler::Easu;
            c.downscale = DownscaleFilter::Lanczos3;
            break;
    }
    if (s.upscaler == UpscalerChoice::Easu) c.upscaler = Upscaler::Easu;
    else if (s.upscaler == UpscalerChoice::Fsr4) c.upscaler = Upscaler::Fsr4;
    return c;
}

// ---- new-frames gate ----------------------------------------------------------------------------------------------------

FrameDecision NewFrameGate::OnFrame(bool isNew, double nowMs, bool forceRedraw) {
    if (isNew) {
        if (lastNewMs_ >= 0) {
            const double dt = nowMs - lastNewMs_;
            if (dt > 0) contentIntervalMs_ = contentIntervalMs_ > 0 ? contentIntervalMs_ * 0.9 + dt * 0.1 : dt;
        }
        lastNewMs_ = nowMs;
    }
    FrameDecision d;
    if (newFramesOnly_ && !isNew && !forceRedraw) d = {false, false};
    if (d.process) ++processed_;
    else ++skipped_;
    return d;
}

// ---- the chain ------------------------------------------------------------------------------------------------------------

namespace {

constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

class ChainImpl final : public QualityChain {
public:
    ChainImpl(ID3D12Device* device, TextureAllocator& allocator, const ChainOptions& options)
        : device_(device), alloc_(allocator), opt_(options), config_(options.config) {
        if (!opt_.fsr4Factory) opt_.fsr4Factory = CreateFsr4Backend;
    }

    Status Init() {
        for (const Status& st : {nv_.Init(device_), deblock_.Init(device_), down_.Init(device_), easu_.Init(device_), rcas_.Init(device_)})
            if (!st.ok) return st;
        return Status::Ok();
    }

    void SetConfig(const ChainConfig& c) override { config_ = c; }
    const ChainConfig& Config() const override { return config_; }
    const ChainLayout& Layout() const override { return layout_; }
    void ResetHistory() override { resetNext_ = true; }
    std::string Fsr4Fallback() const override { return fsr4Why_; }

    std::string UpscalerName() const override {
        if (layout_.path == QualityPath::UpscaleFsr4 && fsr4_) return fsr4_->Name();
        if (layout_.path == QualityPath::UpscaleEasu) return "EASU";
        return {};
    }

    Status Configure(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH) override {
        if (!srcW || !srcH || !outW || !outH) return Status::Fail("QualityChain: tamanho zero");
        if (configured_ && srcW == layout_.srcW && srcH == layout_.srcH && outW == layout_.outW && outH == layout_.outH &&
            configuredUpscaler_ == config_.upscaler)
            return Status::Ok();

        ChainLayout layout = PlanLayout(srcW, srcH, outW, outH, config_.upscaler, true);
        std::unique_ptr<UpscalerBackend> backend;
        std::string why;
        if (layout.path == QualityPath::UpscaleFsr4) {
            const bool same = fsr4_ && fsr4Sizes_.renderW == srcW && fsr4Sizes_.renderH == srcH && fsr4Sizes_.outputW == layout.fitW &&
                              fsr4Sizes_.outputH == layout.fitH;
            if (same) {
                backend = std::move(fsr4_);
            } else {
                const UpscaleSizes sizes{srcW, srcH, layout.fitW, layout.fitH};
                Status st;
                backend = opt_.fsr4Factory(device_, sizes, st);
                if (!backend) {
                    why = st.message.empty() ? "FSR 4 indisponível" : st.message;
                    FLUVI_WARN("pipeline", "FSR 4 indisponível, usando EASU: {}", why);
                    layout = PlanLayout(srcW, srcH, outW, outH, Upscaler::Easu, false);
                } else {
                    fsr4Sizes_ = sizes;
                }
            }
        } else if (config_.upscaler == Upscaler::Fsr4 && layout.path == QualityPath::UpscaleEasu) {
            why = std::format("ampliação {:.2f}x fora do alcance do FSR 4 (máximo {}x)", layout.scale, kFsr4MaxScale);
        }
        if (fsr4_) Retire(nullptr, std::move(fsr4_));
        fsr4_ = std::move(backend);
        fsr4Why_ = why;

        // Intermediates: reuse what already has the right size, retire the rest.
        Status st = Ensure(rgb_, srcW, srcH, L"chain.rgb");
        if (st.ok) st = Ensure(deb_, srcW, srcH, L"chain.deblock");
        if (st.ok) {
            if (layout.path == QualityPath::Native) {
                if (scaled_) Retire(std::move(scaled_), nullptr);
            } else {
                st = Ensure(scaled_, layout.fitW, layout.fitH, L"chain.scaled");
            }
        }
        if (!st.ok) return st;

        layout_ = layout;
        configured_ = true;
        configuredUpscaler_ = config_.upscaler;
        resetNext_ = true;
        FLUVI_INFO("pipeline", "cadeia: {}x{} -> {}x{} saída (imagem {}x{} em {},{}), caminho {}{}{}", srcW, srcH, outW, outH, layout.fitW,
                   layout.fitH, layout.offX, layout.offY, PathName(layout.path),
                   layout.path == QualityPath::UpscaleFsr4 && fsr4_ ? std::format(" ({})", fsr4_->Name()) : std::string(),
                   why.empty() ? std::string() : std::format(" [{}]", why));
        return Status::Ok();
    }

    bool Record(ID3D12GraphicsCommandList* list, const GpuTexture& nv12, const Nv12Params& params, const GpuTexture& out) override {
        if (!list || !nv12.srv.ptr || !nv12.srvChroma.ptr || !out.uav.ptr || !out.resource) {
            FLUVI_ERROR("pipeline", "QualityChain::Record: nv12.srv/srvChroma e out.uav/resource são obrigatórios");
            return false;
        }
        // Retire bookkeeping: one tick per recorded frame.
        for (auto& r : retired_) if (r.framesLeft) --r.framesLeft;
        std::erase_if(retired_, [](const Retired& r) { return r.framesLeft == 0; });

        const uint32_t srcW = std::min(params.visibleWidth ? params.visibleWidth : nv12.width, nv12.width);
        const uint32_t srcH = std::min(params.visibleHeight ? params.visibleHeight : nv12.height, nv12.height);
        if (!configured_ || srcW != layout_.srcW || srcH != layout_.srcH || out.width != layout_.outW || out.height != layout_.outH ||
            configuredUpscaler_ != config_.upscaler) {
            if (configured_) FLUVI_WARN("pipeline", "Record reconfigurou a cadeia (tamanho ou escalador mudou): chame Configure antes, fora do frame");
            const Status st = Configure(srcW, srcH, out.width, out.height);
            if (!st.ok) {
                FLUVI_ERROR("pipeline", "Configure falhou: {}", st.message);
                return false;
            }
        }
        Nv12Params p = params;
        p.visibleWidth = srcW;
        p.visibleHeight = srcH;

        const auto now = std::chrono::steady_clock::now();
        const float frameMs = lastFrame_.time_since_epoch().count() == 0
                                  ? 16.6f
                                  : std::clamp(std::chrono::duration<float, std::milli>(now - lastFrame_).count(), 1.f, 100.f);
        lastFrame_ = now;

        // 1. NV12 -> RGB
        rgb_->Transition(list, kWrite);
        nv_.Record(list, nv12, rgb_->View(), p);
        rgb_->Transition(list, kRead);
        Texture* cur = rgb_.get();

        // 2. deblock (before any scaling: the block grid is the stream's)
        if (config_.deblock > 0.f) {
            deb_->Transition(list, kWrite);
            deblock_.Record(list, cur->View(), deb_->View(), config_.deblock);
            deb_->Transition(list, kRead);
            cur = deb_.get();
        }

        // 3. scale
        bool useEasu = layout_.path == QualityPath::UpscaleEasu;
        if (layout_.path == QualityPath::Downscale) {
            scaled_->Transition(list, kWrite);
            down_.Record(list, cur->View(), scaled_->View(), config_.downscale);
            scaled_->Transition(list, kRead);
            cur = scaled_.get();
        } else if (layout_.path == QualityPath::UpscaleFsr4) {
            scaled_->Transition(list, kWrite);
            UpscaleFrame f;
            f.list = list;
            f.color = cur->View().resource;
            f.output = scaled_->View().resource;
            f.frameTimeMs = frameMs;
            f.reset = opt_.fsr4ResetEveryFrame || resetNext_;
            const bool ok = fsr4_->Record(f);
            ID3D12DescriptorHeap* heaps[] = {alloc_.ShaderHeap()};   // FidelityFX bound its own heaps
            list->SetDescriptorHeaps(1, heaps);
            if (ok) {
                scaled_->Transition(list, kRead);
                cur = scaled_.get();
            } else {
                // Run-time failure: EASU for good (until the next Configure), and still produce this frame.
                fsr4Why_ = "o dispatch do FSR 4 falhou";
                FLUVI_WARN("pipeline", "FSR 4 falhou no dispatch, usando EASU");
                Retire(nullptr, std::move(fsr4_));
                layout_.path = QualityPath::UpscaleEasu;
                useEasu = true;
            }
        }
        if (useEasu) {
            scaled_->Transition(list, kWrite);
            easu_.Record(list, cur->View(), scaled_->View());
            scaled_->Transition(list, kRead);
            cur = scaled_.get();
        }
        resetNext_ = false;

        // 4. RCAS into the output, centred (bars when the aspect differs)
        const RcasPlacement place{layout_.offX, layout_.offY};
        rcas_.Record(list, cur->View(), out, config_.sharpness, &place);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = out.resource;
        list->ResourceBarrier(1, &b);
        return true;
    }

private:
    struct Retired {
        std::unique_ptr<Texture> tex;
        std::unique_ptr<UpscalerBackend> backend;
        uint32_t framesLeft = kRetireFrames;
    };

    void Retire(std::unique_ptr<Texture> tex, std::unique_ptr<UpscalerBackend> backend) {
        if (tex || backend) retired_.push_back({std::move(tex), std::move(backend), kRetireFrames});
    }

    Status Ensure(std::unique_ptr<Texture>& tex, uint32_t w, uint32_t h, const wchar_t* name) {
        if (tex && tex->View().width == w && tex->View().height == h) return Status::Ok();
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.format = kIntermediateFormat;
        d.shaderRead = true;
        d.unorderedAccess = true;
        d.initialState = kRead;
        d.debugName = name;
        Status st;
        auto fresh = alloc_.Create(d, st);
        if (!fresh) return st.ok ? Status::Fail("QualityChain: textura não criada") : st;
        if (tex) Retire(std::move(tex), nullptr);
        tex = std::move(fresh);
        return Status::Ok();
    }

    ID3D12Device* device_;
    TextureAllocator& alloc_;
    ChainOptions opt_;
    ChainConfig config_;
    Nv12ToRgbPass nv_;
    DeblockPass deblock_;
    DownscalePass down_;
    EasuPass easu_;
    RcasPass rcas_;
    ChainLayout layout_;
    bool configured_ = false;
    Upscaler configuredUpscaler_ = Upscaler::Easu;
    std::unique_ptr<Texture> rgb_, deb_, scaled_;
    std::unique_ptr<UpscalerBackend> fsr4_;
    UpscaleSizes fsr4Sizes_;
    std::string fsr4Why_;
    bool resetNext_ = true;
    std::vector<Retired> retired_;
    std::chrono::steady_clock::time_point lastFrame_{};
};

} // namespace

std::unique_ptr<QualityChain> QualityChain::Create(ID3D12Device* device, TextureAllocator& allocator, const ChainOptions& options, Status& status) {
    if (!device) {
        status = Status::Fail("QualityChain: sem dispositivo D3D12");
        return nullptr;
    }
    auto chain = std::make_unique<ChainImpl>(device, allocator, options);
    status = chain->Init();
    if (!status.ok) return nullptr;
    return chain;
}

} // namespace fluvi::gpu
