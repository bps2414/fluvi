// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: Flow*, ShotChange and HudLock passes (contract: include/fluvi/gpu/FlowPasses.h).
// Dispatch and constant conventions follow Veyra's EnhanceGraph.cpp / HudLock.cpp / VfgBackend.cpp @ c6633121.
#include "fluvi/gpu/FlowPasses.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

#include "ComputeKernel.h"
#include "fluvi/core/Log.h"

#include "FlowAdapt.dxil.h"
#include "FlowClaim.dxil.h"
#include "FlowFill.dxil.h"
#include "FlowRefine.dxil.h"
#include "HudLockCompose.dxil.h"
#include "HudLockUpdate.dxil.h"
#include "VfgShotChange.dxil.h"

namespace fluvi::gpu {
using detail::ComputeKernel;
using detail::Groups;
using detail::KernelSpec;

namespace {
constexpr uint32_t kTile = 16;   // the ported shaders use 16x16 thread groups

template <size_t N>
KernelSpec Spec(const char* name, const unsigned char (&dxil)[N], uint32_t constants, uint32_t srvs, uint32_t uavs, bool rootUav = false) {
    KernelSpec s;
    s.name = name;
    s.dxil = dxil;
    s.dxilSize = N;
    s.constants = constants;
    s.srvs = srvs;
    s.uavs = uavs;
    s.rootUav = rootUav;
    return s;
}

bool Ok(const ComputeKernel& k, const char* what, bool cond) {
    if (!k.Ready()) { FLUVI_ERROR("pipeline", "{}: Record before a successful Init", k.Name()); return false; }
    if (!cond) { FLUVI_ERROR("pipeline", "{}: {}", k.Name(), what); return false; }
    return true;
}

// Optional input left empty: bind a descriptor of the same pass instead (never read by the shader).
D3D12_GPU_DESCRIPTOR_HANDLE OrElse(const GpuTexture& t, D3D12_GPU_DESCRIPTOR_HANDLE fallback) { return t.srv.ptr ? t.srv : fallback; }

uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
} // namespace

// ---- FlowAdapt -------------------------------------------------------------------------------------------
struct FlowAdaptPass::Impl { ComputeKernel k; };
FlowAdaptPass::FlowAdaptPass() : impl_(std::make_unique<Impl>()) {}
FlowAdaptPass::~FlowAdaptPass() = default;
Status FlowAdaptPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FlowAdapt", g_FlowAdapt, 8, 1, 1)); }

void FlowAdaptPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out) {
    if (!Ok(impl_->k, "needs in.srv, out.uav and non-empty sizes", in.srv.ptr && out.uav.ptr && in.width && in.height && out.width && out.height)) return;
    const uint32_t c[8] = {in.width, in.height, out.width, out.height, 0, 0, 0, 0};
    impl_->k.Bind(list, c, &in.srv, &out.uav);
    list->Dispatch(Groups(out.width, kTile), Groups(out.height, kTile), 1);
}

// ---- FlowRefine ------------------------------------------------------------------------------------------
struct FlowRefinePass::Impl { ComputeKernel k; };
FlowRefinePass::FlowRefinePass() : impl_(std::make_unique<Impl>()) {}
FlowRefinePass::~FlowRefinePass() = default;
Status FlowRefinePass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FlowRefine", g_FlowRefine, 8, 5, 1)); }

void FlowRefinePass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& rawFlow, const GpuTexture& previous,
                            const GpuTexture& current, const GpuTexture& reverseFlow, const GpuTexture& sceneChanges,
                            const GpuTexture& refined, const FlowRefineParams& params) {
    if (!Ok(impl_->k, "needs rawFlow, previous, current, refined descriptors and non-empty sizes",
            rawFlow.srv.ptr && previous.srv.ptr && current.srv.ptr && refined.uav.ptr && rawFlow.width && rawFlow.height && refined.width && refined.height))
        return;
    if (!Ok(impl_->k, "hasReverseFlow needs reverseFlow", !params.hasReverseFlow || reverseFlow.srv.ptr)) return;
    const uint32_t flags = (params.hasReverseFlow ? 1u : 0u) | (params.fidelityFlow ? 2u : 0u) | (params.fidelityWarmingUp ? 4u : 0u);
    const uint32_t c[8] = {rawFlow.width, rawFlow.height, refined.width, refined.height, std::max(1u, params.gridSize), Bits(params.tieBias), flags, 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[5] = {rawFlow.srv, previous.srv, current.srv, OrElse(reverseFlow, rawFlow.srv),
                                                 OrElse(sceneChanges, rawFlow.srv)};
    impl_->k.Bind(list, c, srv, &refined.uav);
    list->Dispatch(Groups(refined.width, kTile), Groups(refined.height, kTile), 1);
}

// ---- FlowClaim -------------------------------------------------------------------------------------------
struct FlowClaimPass::Impl { ComputeKernel k; };
FlowClaimPass::FlowClaimPass() : impl_(std::make_unique<Impl>()) {}
FlowClaimPass::~FlowClaimPass() = default;
Status FlowClaimPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FlowClaim", g_FlowClaim, 8, 1, 2)); }

void FlowClaimPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& refined, const GpuTexture& claims, const GpuTexture& agreement) {
    if (!Ok(impl_->k, "needs refined.srv, claims.uav, agreement.uav and non-empty sizes",
            refined.srv.ptr && claims.uav.ptr && agreement.uav.ptr && claims.width && claims.height))
        return;
    const uint32_t w = claims.width, h = claims.height;
    uint32_t indexBits = 1;
    while ((uint64_t{1} << indexBits) < uint64_t{w} * h) ++indexBits;
    const D3D12_GPU_DESCRIPTOR_HANDLE uav[2] = {claims.uav, agreement.uav};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        const uint32_t c[8] = {w, h, mode, indexBits, 0, 0, 0, 0};
        impl_->k.Bind(list, c, &refined.srv, uav);
        list->Dispatch(Groups(w, kTile), Groups(h, kTile), 1);
        if (mode < 2 && claims.resource) {   // clear -> claim -> resolve all go through the claims texture
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = claims.resource;
            list->ResourceBarrier(1, &b);
        }
    }
}

// ---- FlowFill --------------------------------------------------------------------------------------------
struct FlowFillPass::Impl { ComputeKernel k; };
FlowFillPass::FlowFillPass() : impl_(std::make_unique<Impl>()) {}
FlowFillPass::~FlowFillPass() = default;
Status FlowFillPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FlowFill", g_FlowFill, 8, 3, 1)); }

void FlowFillPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& refined, const GpuTexture& current, const GpuTexture& claimed,
                          const GpuTexture& output, const FlowFillParams& params) {
    if (!Ok(impl_->k, "needs refined.srv, current.srv, output.uav and non-empty sizes", refined.srv.ptr && current.srv.ptr && output.uav.ptr && output.width && output.height))
        return;
    if (!Ok(impl_->k, "disagreementFromClaims needs claimed.srv", !params.disagreementFromClaims || claimed.srv.ptr)) return;
    const uint32_t c[8] = {output.width, output.height, Bits(params.threshold), Bits(params.distanceWeight), Bits(params.consistencyThreshold),
                           Bits(params.sourceThreshold), params.disagreementFromClaims ? 1u : 0u, 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[3] = {refined.srv, current.srv, OrElse(claimed, current.srv)};
    impl_->k.Bind(list, c, srv, &output.uav);
    list->Dispatch(Groups(output.width, kTile), Groups(output.height, kTile), 1);
}

// ---- ShotChange ------------------------------------------------------------------------------------------
struct ShotChangePass::Impl { ComputeKernel k; };
ShotChangePass::ShotChangePass() : impl_(std::make_unique<Impl>()) {}
ShotChangePass::~ShotChangePass() = default;
Status ShotChangePass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("VfgShotChange", g_VfgShotChange, 12, 2, 0, true)); }

void ShotChangePass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& previous, const GpuTexture& current,
                            ID3D12Resource* results, const ShotChangeParams& params) {
    if (!Ok(impl_->k, "needs previous.srv, current.srv, a results buffer and parity 0/1",
            previous.srv.ptr && current.srv.ptr && results && params.parity < 2 && current.width && current.height))
        return;
    if (!Ok(impl_->k, "previous and current must have the same size", previous.width == current.width && previous.height == current.height)) return;
    const uint32_t c[12] = {current.width, current.height, params.parity, params.compare ? 1u : 0u, Bits(params.cutSad), Bits(params.cutHistogram),
                            Bits(params.replacementSad), Bits(params.replacementHistogram), Bits(params.flashSad), Bits(params.flashHistogram), 0, 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[2] = {previous.srv, current.srv};
    impl_->k.Bind(list, c, srv, nullptr, results->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
}

// ---- HudLockUpdate ---------------------------------------------------------------------------------------
struct HudLockUpdatePass::Impl { ComputeKernel k; };
HudLockUpdatePass::HudLockUpdatePass() : impl_(std::make_unique<Impl>()) {}
HudLockUpdatePass::~HudLockUpdatePass() = default;

Status HudLockUpdatePass::Init(ID3D12Device* device) {
    if (!device) return Status::Fail("HudLockUpdate: no device");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{DXGI_FORMAT_R16G16_FLOAT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
        !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return Status::Fail("HudLockUpdate: device lacks typed UAV load/store for R16G16_FLOAT");
    return impl_->k.Create(device, Spec("HudLockUpdate", g_HudLockUpdate, 8, 2, 1));
}

void HudLockUpdatePass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& current, const GpuTexture& previous,
                               const GpuTexture& state, const HudLockParams& params) {
    if (!Ok(impl_->k, "needs current.srv, previous.srv, state.uav and non-empty sizes", current.srv.ptr && previous.srv.ptr && state.uav.ptr && current.width && current.height))
        return;
    const uint32_t c[8] = {current.width, current.height, params.reset ? 1u : 0u, static_cast<uint32_t>(params.mode), 0, 0, 0, 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[2] = {current.srv, previous.srv};
    impl_->k.Bind(list, c, srv, &state.uav);
    list->Dispatch(Groups(current.width, kTile), Groups(current.height, kTile), 1);
}

// ---- HudLockCompose --------------------------------------------------------------------------------------
struct HudLockComposePass::Impl { ComputeKernel k; };
HudLockComposePass::HudLockComposePass() : impl_(std::make_unique<Impl>()) {}
HudLockComposePass::~HudLockComposePass() = default;
Status HudLockComposePass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("HudLockCompose", g_HudLockCompose, 8, 3, 1)); }

void HudLockComposePass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& generated, const GpuTexture& held,
                                const GpuTexture& state, const GpuTexture& out, const HudLockParams& params) {
    if (!Ok(impl_->k, "needs generated.srv, held.srv, state.srv, out.uav and non-empty sizes",
            generated.srv.ptr && held.srv.ptr && state.srv.ptr && out.uav.ptr && generated.width && generated.height))
        return;
    const uint32_t c[8] = {generated.width, generated.height, static_cast<uint32_t>(params.mode), 0, 0, 0, 0, 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[3] = {generated.srv, held.srv, state.srv};
    impl_->k.Bind(list, c, srv, &out.uav);
    list->Dispatch(Groups(generated.width, kTile), Groups(generated.height, kTile), 1);
}

} // namespace fluvi::gpu
