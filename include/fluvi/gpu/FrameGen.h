// SPDX-License-Identifier: GPL-3.0-only
// Fluvi gpu: frame generation for the "Liso" mode (30 -> 60). AMD FSR 3.1 frame interpolation (FidelityFX SDK 2.4,
// MIT) loaded at run time through amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll, into a
// texture the app owns (no FidelityFX swap chain). Ported from Veyra's FsrFgPresenter (GPL-3.0-only, c6633121).
//
// Video has no engine motion vectors or depth: the backend feeds all-zero display-resolution motion vectors and a
// constant depth, so the interpolation comes from FSR's own optical flow (still pixels, like the HUD, stay put).
//
// Generate(color = real frame N) writes the frame halfway between N-1 and N into `output`. The caller presents
// `output` first and N about half a content interval later. Owner: wave 3, agent I.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <d3d12.h>
#include "fluvi/core/Status.h"

namespace fluvi::gpu {

class FrameGenerator {
public:
    virtual ~FrameGenerator() = default;
    // Seconds-long the first time (loads the DLL, compiles shaders): call it outside the hot path if possible.
    static std::unique_ptr<FrameGenerator> Create(ID3D12Device* device, uint32_t width, uint32_t height,
                                                  DXGI_FORMAT format, Status& status);
    // color and output: width x height, the Create format, output with ALLOW_UNORDERED_ACCESS, color != output.
    // Both in D3D12_RESOURCE_STATE_COMMON before and after (FidelityFX transitions inside the list).
    // reset: first frame or a discontinuity (the output is then not a valid in-between frame).
    // frameMs: time between the previous real frame and this one. False on failure (then the generator is dead).
    virtual bool Generate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output,
                          bool reset, float frameMs) = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;
    virtual const std::string& Provider() const = 0;   // e.g. "3.1.6"
};

} // namespace fluvi::gpu
