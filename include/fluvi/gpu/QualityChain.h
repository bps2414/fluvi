// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: the quality chain: any monitor, any GPU.
//
//   decoded NV12 (+ visible crop)  ->  RGB  ->  deblock  ->  [ downscale | nothing | FSR 4.1.2 / EASU ]  ->  RCAS
//                                                                                    -> RGBA8 output, any size
// The path is chosen from the stream size and the output size (window or monitor, any resolution and aspect):
//   output smaller than the stream  -> Downscale (supersampling) + RCAS
//   output equal to the stream      -> Native: cleanup (deblock) + RCAS
//   output bigger                   -> FSR 4.1.2 + RCAS; EASU + RCAS when FSR 4 is unavailable, disabled by the
//                                      preset/override, fails at run time, or the ratio is over 3x per dimension
//   aspect different from the stream's: the picture is fitted and centred with black bars, never stretched.
//
// Using it (the app / session):
//   1. once: auto chain = QualityChain::Create(device, DeviceTextureAllocator(gpuDevice), options, status);
//      options.config = ResolveChainConfig(settings.video, ClassifyFsr4(CapsFromDevice(gpuDevice)));
//   2. when the stream size or the output size is known or changes: chain->Configure(srcW, srcH, outW, outH). Slow
//      (allocates; creating the FSR 4 context compiles ML shaders): call it off the render thread when you can.
//      Record() also configures by itself if the sizes differ, with a warning in the log (a hitch).
//   3. per frame, with the shader-visible CBV_SRV_UAV heap bound on `list` (SetDescriptorHeaps) and the states
//      nv12 = NON_PIXEL_SHADER_RESOURCE, out = UNORDERED_ACCESS (an RGBA8 texture with a UAV):
//      chain->Record(list, nv12, nv12Params, out);   // leaves out in UNORDERED_ACCESS, UAV barrier added
//      (FSR 4 sets its own descriptor heaps on the list; Record rebinds the heap given by the allocator.)
//   4. settings changes: chain->SetConfig(ResolveChainConfig(...)); the upscaler choice is applied at the next
//      Configure, deblock/sharpness/downscale filter at the next Record.
// Direct queue only: a direct-queue list. (Async compute is not used: the chain is a straight dependency line.)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <d3d12.h>
#include "fluvi/common/Settings.h"
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/Passes.h"
#include "fluvi/gpu/Texture.h"
#include "fluvi/gpu/UpscalerBackend.h"

namespace fluvi::gpu {

// ---- path planning (pure functions) ----------------------------------------------------------------------------

enum class QualityPath : uint8_t { Downscale, Native, UpscaleFsr4, UpscaleEasu };
const char* PathName(QualityPath path);

enum class Upscaler : uint8_t { Easu, Fsr4 };   // what is asked for; the chain still falls back to EASU

struct ChainLayout {
    uint32_t srcW = 0, srcH = 0;     // visible stream picture
    uint32_t outW = 0, outH = 0;     // output texture
    uint32_t fitW = 0, fitH = 0;     // the picture inside the output (aspect kept, as large as fits)
    uint32_t offX = 0, offY = 0;     // its top-left corner: (outW-fitW)/2, (outH-fitH)/2
    double scale = 1.0;              // fitW / srcW
    QualityPath path = QualityPath::Native;
    bool HasBars() const { return fitW != outW || fitH != outH; }
};

// wanted == Fsr4 only yields UpscaleFsr4 when fsr4Usable (backend alive) and scale <= kFsr4MaxScale.
// Zero sizes give a layout with everything 0 (path Native); callers check srcW/outW.
ChainLayout PlanLayout(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH, Upscaler wanted, bool fsr4Usable);

// ---- presets and GPU detection ------------------------------------------------------------------------------------

struct ChainConfig {
    Upscaler upscaler = Upscaler::Easu;
    DownscaleFilter downscale = DownscaleFilter::Lanczos3;
    float deblock = 0.5f;      // 0 = pass skipped
    float sharpness = 0.6f;    // RCAS, 0..1
};

// What the preset logic needs to know about the GPU (plain data so it can be tested).
struct GpuCaps {
    Vendor vendor = Vendor::Unknown;
    std::string name;                // UTF-8 adapter name
    uint64_t dedicatedVram = 0;      // bytes
    bool integrated = false;         // UMA: shares system memory
};
GpuCaps CapsFromDevice(Device& device);

// How FSR 4.1.2 runs on a GPU, by hardware generation (a heuristic for presets and logging; whether it really runs
// is decided by creating the context): None = not AMD, or older than RDNA 2; Slow = RDNA 2 and integrated RDNA 3
// (INT8 ML model, about 2 ms for a 1080p output on an RX 6600, scaling with the output pixels); Fast = RDNA 3
// discrete (INT8) and RDNA 4 (FP8).
enum class Fsr4Tier : uint8_t { None, Slow, Fast };
Fsr4Tier ClassifyFsr4(const GpuCaps& caps);
const char* TierName(Fsr4Tier tier);

// First-run default (VideoSettings::preset when presetDetected is false). Integrated GPUs and less than 4 GiB of
// VRAM -> Light; AMD with a Fast FSR 4 tier and >= 12 GiB, or NVIDIA with >= 12 GiB -> Max; the rest -> Standard.
QualityPreset DetectPreset(const GpuCaps& caps);

// Preset + overrides -> what the chain does:
//   Light:    EASU, Bicubic downscale, deblock off, RCAS
//   Standard: FSR 4 only when the tier is Fast (EASU otherwise), Lanczos3, deblock and RCAS from the settings
//   Max:      FSR 4 whenever the tier is not None, Lanczos3, deblock and RCAS from the settings
// settings.upscaler (Easu / Fsr4) overrides the preset's upscaler; Fsr4 is attempted on any GPU (it falls back to
// EASU, with the reason in the log, when the backend cannot be created).
ChainConfig ResolveChainConfig(const VideoSettings& settings, Fsr4Tier tier);

// ---- the chain ---------------------------------------------------------------------------------------------------------

// What the chain needs to allocate its intermediates. DeviceTextureAllocator wraps gpu-core's Device.
class TextureAllocator {
public:
    virtual ~TextureAllocator() = default;
    virtual std::unique_ptr<Texture> Create(const TextureDesc& desc, Status& status) = 0;
    // The shader-visible CBV_SRV_UAV heap that holds the descriptors of everything Create() returns (rebound after FSR 4).
    virtual ID3D12DescriptorHeap* ShaderHeap() const = 0;
};

class DeviceTextureAllocator final : public TextureAllocator {
public:
    explicit DeviceTextureAllocator(Device& device) : device_(device) {}
    std::unique_ptr<Texture> Create(const TextureDesc& desc, Status& status) override { return device_.CreateTexture(desc, status); }
    ID3D12DescriptorHeap* ShaderHeap() const override { return device_.SrvUavHeap().Heap(); }
private:
    Device& device_;
};

struct ChainOptions {
    ChainConfig config;
    // Empty = CreateFsr4Backend. Tests inject a failing factory to force the EASU fallback.
    UpscalerFactory fsr4Factory;
    // Video has no motion vectors: FSR 4 gets zero motion and zero jitter, and with temporal history on it ghosts
    // on anything that moves. True (default) = reset every frame: a clean single-frame spatial upscale.
    bool fsr4ResetEveryFrame = true;
};

class QualityChain {
public:
    // Builds the passes (Nv12, Deblock, Downscale, EASU, RCAS). Not configured yet.
    static std::unique_ptr<QualityChain> Create(ID3D12Device* device, TextureAllocator& allocator, const ChainOptions& options, Status& status);
    virtual ~QualityChain() = default;

    virtual void SetConfig(const ChainConfig& config) = 0;
    virtual const ChainConfig& Config() const = 0;

    // (Re)creates the intermediates for a stream of srcW x srcH (the visible picture) and an output of outW x outH,
    // and the FSR 4 context when the path needs one. Old intermediates are kept for kRetireFrames Record() calls
    // before they are freed, so a resize does not need an idle GPU. No-op when nothing changed.
    virtual Status Configure(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH) = 0;
    static constexpr uint32_t kRetireFrames = 6;

    // False when not configured and Configure fails, or the textures are unusable (logged); nothing is recorded then.
    virtual bool Record(ID3D12GraphicsCommandList* list, const GpuTexture& nv12, const Nv12Params& params, const GpuTexture& out) = 0;
    // Drops FSR 4's history on the next frame (stream restart, resize...).
    virtual void ResetHistory() = 0;

    virtual const ChainLayout& Layout() const = 0;
    // The upscaler really in use on an upscale path: "FSR 4.1.2", "EASU", or "" on the other paths.
    virtual std::string UpscalerName() const = 0;
    // Why FSR 4 is not in use although the config asked for it (backend creation or dispatch failure); "" otherwise.
    virtual std::string Fsr4Fallback() const = 0;
};

// ---- "Mostrar só frames novos (pra Lossless)" -----------------------------------------------------------------
// Play mode option VideoSettings::newFramesOnly. The decoder marks a frame as new or repeated (the console sends
// 60 Hz of a 30 fps game with every frame doubled); Lossless Scaling captures the window and interpolates between the
// frames it sees, so it needs the window to change only when the content changes. With the option on, the app runs
// the chain AND presents only for new frames (or when `forceRedraw`: UI overlay changed, resize, first frame), so the
// window updates at the content cadence and a duplicate costs no GPU work at all. With it off every frame is
// processed and presented (Play mode as usual). Smooth mode ignores the option (it needs the repeats to pace).
struct FrameDecision {
    bool process = true;   // run QualityChain::Record for this frame
    bool present = true;   // Present() after it
};

class NewFrameGate {
public:
    explicit NewFrameGate(bool newFramesOnly = false) : newFramesOnly_(newFramesOnly) {}
    void SetNewFramesOnly(bool on) { newFramesOnly_ = on; }
    bool NewFramesOnly() const { return newFramesOnly_; }
    // Once per decoded frame. nowMs: any monotonic clock, used to estimate the content rate.
    FrameDecision OnFrame(bool isNew, double nowMs, bool forceRedraw = false);
    uint64_t Processed() const { return processed_; }
    uint64_t Skipped() const { return skipped_; }
    // Smoothed rate of new frames (0 until two have been seen).
    double ContentHz() const { return contentIntervalMs_ > 0 ? 1000.0 / contentIntervalMs_ : 0.0; }
private:
    bool newFramesOnly_ = false;
    uint64_t processed_ = 0, skipped_ = 0;
    double lastNewMs_ = -1, contentIntervalMs_ = 0;
};

} // namespace fluvi::gpu
