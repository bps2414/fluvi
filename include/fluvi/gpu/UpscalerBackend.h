// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: the small interface every vendor upscaler backend sits behind (backends are loaded by DLL
// at run time, nothing in the chain requires a specific GPU). Today: FSR 4.1.2 (AMD FidelityFX SDK 2.4, signed DLLs
// loaded through amd_fidelityfx_loader_dx12.dll). The chain falls back to the EASU compute pass when a backend cannot
// be created or its dispatch fails.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <d3d12.h>
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi::gpu {

struct UpscaleSizes {
    uint32_t renderW = 0, renderH = 0;     // what is upscaled (the cleaned stream picture)
    uint32_t outputW = 0, outputH = 0;     // what comes out (the fitted image, never stretched)
};

struct UpscaleFrame {
    ID3D12GraphicsCommandList* list = nullptr;
    // Colour in (render size) and out (output size, needs a UAV). Both are D3D12 resources the backend wraps itself;
    // the caller has them in NON_PIXEL_SHADER_RESOURCE (input) and UNORDERED_ACCESS (output) and gets them back in
    // the same states. Video gamma (non-linear sRGB-ish): backends must treat the colour as non-linear.
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    float frameTimeMs = 16.6f;
    bool reset = true;                      // drop temporal history (video has no motion vectors: see Fsr4Backend.cpp)
    float sharpness = 0.f;                  // backend-internal sharpening; the chain keeps this 0 and uses RCAS
};

// One instance per (render size, output size); the chain recreates it on a resize.
class UpscalerBackend {
public:
    virtual ~UpscalerBackend() = default;
    virtual const char* Name() const = 0;                // e.g. "FSR 4.1.2"
    // Records the upscale into frame.list. FidelityFX binds its own descriptor heaps and root state on the list:
    // the caller rebinds its heap afterwards. False = the backend failed (logged); the chain falls back to EASU.
    virtual bool Record(const UpscaleFrame& frame) = 0;
};

// Creates a backend for the sizes, or explains why not (missing DLLs, GPU not supported, ratio out of range...).
using UpscalerFactory = std::function<std::unique_ptr<UpscalerBackend>(ID3D12Device*, const UpscaleSizes&, Status& why)>;

// FSR 4.1.2 through the FidelityFX loader DLL. Searches: next to the executable, $FLUVI_FFX_DIR, then the SDK's
// signedbin folder baked in at build time (dev). Context creation compiles the ML shaders: slow (hundreds of ms),
// call it off the render thread.
std::unique_ptr<UpscalerBackend> CreateFsr4Backend(ID3D12Device* device, const UpscaleSizes& sizes, Status& why);

// Largest per-dimension scale FSR 4 accepts (its "ultra performance" ratio).
inline constexpr double kFsr4MaxScale = 3.0;

} // namespace fluvi::gpu
