// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: the compute passes of the image chain. Owner: agent E (src/gpu/pipeline, shaders/).
//
// Chain (wave 2 wires it): NV12 -> RGB -> deblock -> downscale 1440->1080 (or EASU/FSR4 for 720p) -> RCAS.
//
// Rules shared by every pass:
// - Init once per device (root signature + PSO, shaders embedded in the binary, compiled at build time).
// - Record() only records: no allocation, no waits, no descriptor writes. The caller has bound the
//   shader-visible CBV_SRV_UAV heap that holds the GpuTexture descriptors (SetDescriptorHeaps) on the list.
// - States: inputs in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, outputs in
//   D3D12_RESOURCE_STATE_UNORDERED_ACCESS. The pass does not transition; the caller does (and adds the UAV
//   barrier between passes that chain on the same resource).
// - Intermediates are kIntermediateFormat (RGBA16F, linear-ish: values stay in the video's gamma, no
//   conversion); the last pass may write kOutputFormat. Any output size is accepted unless stated.
// - Works on direct and compute command lists.
#pragma once
#include <cstdint>
#include <memory>
#include <d3d12.h>
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi::gpu {

enum class YuvMatrix : uint8_t { Bt709, Bt601 };

struct Nv12Params {
    YuvMatrix matrix = YuvMatrix::Bt709;   // Xbox streams are BT.709
    bool fullRange = false;                // limited (16-235) unless the stream says otherwise
    uint32_t visibleWidth = 0, visibleHeight = 0;   // crop of the decoded surface (decoders align to 16); 0 = whole
};

// NV12 (two SRVs: srv = luma, srvChroma = chroma) -> RGB at the same size. Chroma upsampled with a
// proper (not nearest) filter, MPEG-2/H.264 chroma siting (left, vertically centred).
class Nv12ToRgbPass {
public:
    Nv12ToRgbPass();
    ~Nv12ToRgbPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& nv12, const GpuTexture& out, const Nv12Params& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Light deblocking of H.264 8x8/4x4 block edges, edge-aware so real detail survives. strength 0..1 (0 = copy).
class DeblockPass {
public:
    DeblockPass();
    ~DeblockPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, float strength);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

enum class DownscaleFilter : uint8_t { Lanczos3, Bicubic, Box };

// High-quality reduction (2560x1440 -> 1920x1080 and any other ratio <= 1). Separable, gamma-aware
// option off by default (video gamma kept).
class DownscalePass {
public:
    DownscalePass();
    ~DownscalePass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, DownscaleFilter filter = DownscaleFilter::Lanczos3);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// FSR 1 EASU (AMD, MIT): upscale for the 720p case and the fallback when FSR 4 is unavailable.
class EasuPass {
public:
    EasuPass();
    ~EasuPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Where the RCAS image lands in a bigger output (agent G, wave 2).
struct RcasPlacement { uint32_t x = 0, y = 0; };

// FSR 1 RCAS (AMD, MIT): sharpening. sharpness 0..1 (0 = pass-through, 1 = max), mapped to RCAS stops.
// Without `placement`: out gets min(in, out) pixels from the origin (as before). With it: the whole of `out` is
// written, `in` sits at placement->x/y and everything around it is opaque black (bars, picture never stretched).
class RcasPass {
public:
    RcasPass();
    ~RcasPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, float sharpness,
                const RcasPlacement* placement = nullptr);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace fluvi::gpu
