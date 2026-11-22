// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: motion-refinement, shot-change and HUD-lock compute passes ported from Veyra (GPL-3.0-only,
// shaders/Flow{Adapt,Refine,Claim,Fill}, VfgShotChange, HudLock{Update,Compose}.hlsl @ c6633121).
// Owner: agent E. Consumed by the frame-generation module in wave 3.
//
// Same rules as Passes.h: Init once per device; Record() only records (no allocation, no waits, no descriptor
// writes); the caller has bound the shader-visible CBV_SRV_UAV heap holding the GpuTexture descriptors; inputs
// are in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE and outputs in D3D12_RESOURCE_STATE_UNORDERED_ACCESS; the
// pass does not transition, and the caller adds the UAV barrier on an output before it is read. Passes that
// record several dispatches (FlowClaimPass) insert the UAV barriers between their own dispatches.
//
// Veyra #232: no resolution cap anywhere. Every dispatch size and constant derives from the GpuTexture sizes
// passed in (flow extent = the size of the refined/filled textures, the grid extent = the size of the raw
// flow texture), so a 2560x1440 or 4K flow field works as-is. The only fixed limits are algorithmic: FlowFill
// looks up to 64 flow pixels away, FlowClaim packs a pixel index plus a cost rank into 32 bits (the cost rank
// keeps >= 7 bits up to 33M flow pixels, i.e. 8K).
//
// Descriptor formats: the SRV/UAV the caller creates must match the format listed per binding (the shader
// declares its element type). A texture marked "optional" may be left as a default GpuTexture (ptr 0): the pass
// then binds another of its descriptors in that slot and the shader never reads it.
#pragma once
#include <cstdint>
#include <memory>
#include <d3d12.h>
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi::gpu {

// Formats of the flow-chain textures (the SRV/UAV formats the passes expect).
inline constexpr DXGI_FORMAT kFlowRawFormat = DXGI_FORMAT_R16G16_SINT;          // grid vectors, FidelityFX: integer pixels
inline constexpr DXGI_FORMAT kFlowRefinedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // xy vector, z cost, w disagreement
inline constexpr DXGI_FORMAT kFlowVectorFormat = DXGI_FORMAT_R16G16_FLOAT;      // filled / adapted vectors
inline constexpr DXGI_FORMAT kFlowClaimsFormat = DXGI_FORMAT_R32_UINT;
inline constexpr DXGI_FORMAT kFlowAgreementFormat = DXGI_FORMAT_R16_FLOAT;

// FlowAdapt: resamples a flow field to another extent and rescales the vectors (current->previous pixels).
//   in  : srv, R16G16_FLOAT, size = source extent        out : uav, R16G16_FLOAT, size = target extent
// Dispatch ceil(out.w/16) x ceil(out.h/16). Constants: source W/H, target W/H (8 dwords, 4 unused).
class FlowAdaptPass {
public:
    FlowAdaptPass();
    ~FlowAdaptPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

struct FlowRefineParams {
    uint32_t gridSize = 8;          // flow pixels per raw grid cell (FidelityFX optical flow: 8)
    float tieBias = 0.004f;         // added to every neighbour cell's cost so the pixel's own cell wins ties
    bool fidelityFlow = true;       // raw vectors are FidelityFX (integer pixels); false = NVOF S10.5 (1/32 px)
    bool fidelityWarmingUp = false; // FidelityFX optical flow still warming up: output is all zero
    bool hasReverseFlow = false;    // reverseFlow holds NVOF's backward vectors (w = forward-backward disagreement)
};

// FlowRefine: per flow pixel, picks the vector of the 3x3 neighbouring grid cells whose 3x3 luma patch best
// matches the previous frame (motion edges follow the picture, not the grid). Keeps the best cost for FlowFill.
//   rawFlow     : srv, R16G16_SINT, grid extent (size of the raw flow texture)
//   previous    : srv, float-sampled colour (RGBA8/RGBA16F), flow extent      current: same
//   reverseFlow : srv, R16G16_SINT, grid extent; optional (only read when params.hasReverseFlow)
//   sceneChanges: srv, R32_UINT; optional (FidelityFX scene-change history; only read when params.fidelityFlow)
//   refined     : uav, R16G16B16A16_FLOAT, flow extent: xy vector (current->previous, flow pixels), z best cost,
//                 w disagreement
// Dispatch ceil(refined.w/16) x ceil(refined.h/16). Constants: gridW, gridH, flowW, flowH, gridSize, tieBias,
// flags (1 reverse, 2 fidelity, 4 warming up), 0.
class FlowRefinePass {
public:
    FlowRefinePass();
    ~FlowRefinePass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& rawFlow, const GpuTexture& previous,
                const GpuTexture& current, const GpuTexture& reverseFlow, const GpuTexture& sceneChanges,
                const GpuTexture& refined, const FlowRefineParams& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// FlowClaim: disocclusion test for flows without backward vectors (FidelityFX). Every flow pixel claims the
// previous-frame pixel its vector lands on; the best photometric match keeps the claim; the disagreement
// with the winner's vector stands in for a forward-backward check in FlowFill. Three dispatches (clear,
// claim, resolve), UAV barriers between them recorded by the pass.
//   refined   : srv, R16G16B16A16_FLOAT (FlowRefine output), flow extent
//   claims    : uav, R32_UINT, flow extent (scratch; inverted cost << indexBits | pixel index)
//   agreement : uav, R16_FLOAT, flow extent: disagreement with the winner, flow pixels (read as t2 by FlowFill)
// Dispatch 3x ceil(w/16) x ceil(h/16). Constants: flowW, flowH, mode (0 clear, 1 claim, 2 resolve), indexBits.
class FlowClaimPass {
public:
    FlowClaimPass();
    ~FlowClaimPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& refined, const GpuTexture& claims,
                const GpuTexture& agreement);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

struct FlowFillParams {
    float threshold = 0.06f;            // best cost above this = no correspondence (filled)
    float distanceWeight = 0.002f;      // colour distance added per flow pixel of search distance
    float consistencyThreshold = 2.0f;  // disagreement above this (flow pixels) = filled; 0 = unused
    float sourceThreshold = 0.5f;       // disagreement up to this lends its vector
    bool disagreementFromClaims = true; // use the FlowClaim texture (FidelityFX); else refined.w (NVOF)
};

// FlowFill: pixels with no correspondence (background uncovered by a moving object) take the vector of the
// most similar-coloured confident pixel nearby (up to 64 flow pixels away).
//   refined : srv, R16G16B16A16_FLOAT, flow extent     current: srv, float-sampled colour, flow extent
//   claimed : srv, R16_FLOAT (FlowClaim agreement); optional unless params.disagreementFromClaims
//   output  : uav, R16G16_FLOAT, flow extent: final vector, current->previous flow pixels
// Dispatch ceil(output.w/16) x ceil(output.h/16). Constants: flowW, flowH, threshold, distanceWeight,
// consistencyThreshold, sourceThreshold, fromClaims, 0.
class FlowFillPass {
public:
    FlowFillPass();
    ~FlowFillPass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& refined, const GpuTexture& current,
                const GpuTexture& claimed, const GpuTexture& output, const FlowFillParams& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Shot-change thresholds (defaults = SceneCadenceAnalyzer::Config of the CPU path).
struct ShotChangeParams {
    uint32_t parity = 0;            // result slot 0 or 1 (A/B pair parity)
    bool compare = true;            // false when the pair has no valid previous frame (reset): never a cut
    float cutSad = 0.3f, cutHistogram = 0.5f;
    float replacementSad = 0.08f, replacementHistogram = 0.9f;
    float flashSad = 0.5f, flashHistogram = 0.1f;
};
inline constexpr uint32_t kShotChangeResultBytes = 256;
// Result buffer layout (little endian): u64 @ parity*8 = 1 when the pair is a cut; @ 16+parity*16: float sad,
// float histogram distance, u32 compare; u32 @ 48 = running cut count (zero the buffer once at creation).

// ShotChange: 64x36 luma samples of two frames -> sad + 256-bin histogram distance -> cut flag. One 256-thread
// group, no CPU readback (use the buffer as a predication source or read it back in tests).
//   previous, current : srv, float-sampled colour (RGBA8/RGBA16F), same size, any resolution
//   results           : ID3D12Resource buffer >= kShotChangeResultBytes, ALLOW_UNORDERED_ACCESS, in
//                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS (bound as a root UAV, no descriptor)
// Dispatch 1x1x1. Constants (12 dwords): width, height, parity, compare, 6 thresholds, 2 unused.
class ShotChangePass {
public:
    ShotChangePass();
    ~ShotChangePass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& previous, const GpuTexture& current,
                ID3D12Resource* results, const ShotChangeParams& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

enum class HudColorMode : uint32_t { Srgb = 0, ScRgb = 1, Pq = 2 };   // ScRgb: linear, 1 = 80 nits

struct HudLockParams {
    HudColorMode mode = HudColorMode::Srgb;
    bool reset = false;             // update only: clear the state
};

// HudLockUpdate (step 1): per pixel, how long it has stayed the same (x, 0..1) and its held local contrast (y).
// Needs typed UAV load of R16G16_FLOAT (Init fails with a Status otherwise).
//   current, previous : srv, float-sampled colour, same size
//   state             : uav, R16G16_FLOAT, same size, read-modify-write (persists across frames; reset clears it)
// Dispatch ceil(w/16) x ceil(h/16). Constants: width, height, reset, mode, 4 unused.
class HudLockUpdatePass {
public:
    HudLockUpdatePass();
    ~HudLockUpdatePass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& current, const GpuTexture& previous,
                const GpuTexture& state, const HudLockParams& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// HudLockCompose (step 2): a generated frame takes the pixels that have been still (and carry an edge) from the
// held real frame.
//   generated, held : srv, float-sampled colour, same size   state: srv, R16G16_FLOAT (HudLockUpdate)
//   out             : uav, same size and a colour format; a separate texture (copy it back over `generated`, or
//                     present it directly)
// Dispatch ceil(w/16) x ceil(h/16). Constants: width, height, mode, 5 unused.
class HudLockComposePass {
public:
    HudLockComposePass();
    ~HudLockComposePass();
    Status Init(ID3D12Device* device);
    void Record(ID3D12GraphicsCommandList* list, const GpuTexture& generated, const GpuTexture& held,
                const GpuTexture& state, const GpuTexture& out, const HudLockParams& params);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace fluvi::gpu
