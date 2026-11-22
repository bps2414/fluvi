// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: NV12->RGB, deblock, downscale, EASU, RCAS (contract: include/fluvi/gpu/Passes.h).
#include "fluvi/gpu/Passes.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "ComputeKernel.h"
#include "fluvi/core/Log.h"

#include "Deblock.dxil.h"
#include "Downscale.dxil.h"
#include "FsrEasu.dxil.h"
#include "FsrRcas.dxil.h"
#include "Nv12ToRgb.dxil.h"

namespace fluvi::gpu {
using detail::ComputeKernel;
using detail::Groups;
using detail::KernelSpec;

namespace {
constexpr uint32_t kTile = 8;   // all image passes use 8x8 thread groups

template <size_t N>
KernelSpec Spec(const char* name, const unsigned char (&dxil)[N], uint32_t constants, uint32_t srvs, uint32_t uavs) {
    KernelSpec s;
    s.name = name;
    s.dxil = dxil;
    s.dxilSize = N;
    s.constants = constants;
    s.srvs = srvs;
    s.uavs = uavs;
    return s;
}

bool Ok(const ComputeKernel& k, const char* what, bool cond) {
    if (!k.Ready()) { FLUVI_ERROR("pipeline", "{}: Record before a successful Init", k.Name()); return false; }
    if (!cond) { FLUVI_ERROR("pipeline", "{}: {}", k.Name(), what); return false; }
    return true;
}
} // namespace

// ---- NV12 -> RGB -----------------------------------------------------------------------------------------
struct Nv12ToRgbPass::Impl { ComputeKernel k; };
Nv12ToRgbPass::Nv12ToRgbPass() : impl_(std::make_unique<Impl>()) {}
Nv12ToRgbPass::~Nv12ToRgbPass() = default;

Status Nv12ToRgbPass::Init(ID3D12Device* device) {
    return impl_->k.Create(device, Spec("Nv12ToRgb", g_Nv12ToRgb, 12, 2, 1));
}

void Nv12ToRgbPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& nv12, const GpuTexture& out, const Nv12Params& params) {
    if (!Ok(impl_->k, "needs nv12.srv, nv12.srvChroma and out.uav", nv12.srv.ptr && nv12.srvChroma.ptr && out.uav.ptr)) return;
    uint32_t w = params.visibleWidth ? params.visibleWidth : nv12.width;
    uint32_t h = params.visibleHeight ? params.visibleHeight : nv12.height;
    w = std::min({w, nv12.width, out.width});
    h = std::min({h, nv12.height, out.height});
    if (!Ok(impl_->k, "empty visible area", w && h)) return;

    const bool bt709 = params.matrix == YuvMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    struct C { uint32_t w, h, p0, p1; float yOff, yScale, cOff, cScale, crR, cbB, crG, cbG; } c{};
    c.w = w;
    c.h = h;
    c.yOff = params.fullRange ? 0.f : 16.f / 255.f;
    c.yScale = params.fullRange ? 1.f : 255.f / 219.f;
    c.cOff = 128.f / 255.f;
    c.cScale = params.fullRange ? 1.f : 255.f / 224.f;
    c.crR = static_cast<float>(2.0 * (1.0 - kr));
    c.cbB = static_cast<float>(2.0 * (1.0 - kb));
    c.crG = static_cast<float>(2.0 * kr * (1.0 - kr) / kg);
    c.cbG = static_cast<float>(2.0 * kb * (1.0 - kb) / kg);
    static_assert(sizeof(C) == 12 * 4);
    const D3D12_GPU_DESCRIPTOR_HANDLE srv[2] = {nv12.srv, nv12.srvChroma};
    impl_->k.Bind(list, &c, srv, &out.uav);
    list->Dispatch(Groups(w, kTile), Groups(h, kTile), 1);
}

// ---- Deblock ---------------------------------------------------------------------------------------------
struct DeblockPass::Impl { ComputeKernel k; };
DeblockPass::DeblockPass() : impl_(std::make_unique<Impl>()) {}
DeblockPass::~DeblockPass() = default;

Status DeblockPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("Deblock", g_Deblock, 4, 1, 1)); }

void DeblockPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, float strength) {
    if (!Ok(impl_->k, "needs in.srv and out.uav", in.srv.ptr && out.uav.ptr)) return;
    const uint32_t w = std::min(in.width, out.width), h = std::min(in.height, out.height);
    if (!Ok(impl_->k, "empty image", w && h)) return;
    struct C { uint32_t w, h; float strength; uint32_t pad; } c{w, h, std::clamp(strength, 0.f, 1.f), 0};
    impl_->k.Bind(list, &c, &in.srv, &out.uav);
    list->Dispatch(Groups(w, kTile), Groups(h, kTile), 1);
}

// ---- Downscale -------------------------------------------------------------------------------------------
struct DownscalePass::Impl { ComputeKernel k; };
DownscalePass::DownscalePass() : impl_(std::make_unique<Impl>()) {}
DownscalePass::~DownscalePass() = default;

Status DownscalePass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("Downscale", g_Downscale, 8, 1, 1)); }

void DownscalePass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, DownscaleFilter filter) {
    if (!Ok(impl_->k, "needs in.srv, out.uav and non-empty sizes", in.srv.ptr && out.uav.ptr && in.width && in.height && out.width && out.height)) return;
    // Anti-ringing: Lanczos overshoots most, Catmull-Rom a little, the box not at all.
    const float antiring = filter == DownscaleFilter::Lanczos3 ? 0.85f : filter == DownscaleFilter::Bicubic ? 0.5f : 0.f;
    struct C { uint32_t inW, inH, outW, outH, filter; float antiring; uint32_t p0, p1; } c{
        in.width, in.height, out.width, out.height, static_cast<uint32_t>(filter), antiring, 0, 0};
    impl_->k.Bind(list, &c, &in.srv, &out.uav);
    list->Dispatch(Groups(out.width, kTile), Groups(out.height, kTile), 1);
}

// ---- EASU ------------------------------------------------------------------------------------------------
struct EasuPass::Impl { ComputeKernel k; };
EasuPass::EasuPass() : impl_(std::make_unique<Impl>()) {}
EasuPass::~EasuPass() = default;

Status EasuPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FsrEasu", g_FsrEasu, 4, 1, 1)); }

void EasuPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out) {
    if (!Ok(impl_->k, "needs in.srv, out.uav and non-empty sizes", in.srv.ptr && out.uav.ptr && in.width && in.height && out.width && out.height)) return;
    const uint32_t c[4] = {in.width, in.height, out.width, out.height};
    impl_->k.Bind(list, c, &in.srv, &out.uav);
    list->Dispatch(Groups(out.width, kTile), Groups(out.height, kTile), 1);
}

// ---- RCAS ------------------------------------------------------------------------------------------------
struct RcasPass::Impl { ComputeKernel k; };
RcasPass::RcasPass() : impl_(std::make_unique<Impl>()) {}
RcasPass::~RcasPass() = default;

Status RcasPass::Init(ID3D12Device* device) { return impl_->k.Create(device, Spec("FsrRcas", g_FsrRcas, 4, 1, 1)); }

void RcasPass::Record(ID3D12GraphicsCommandList* list, const GpuTexture& in, const GpuTexture& out, float sharpness) {
    if (!Ok(impl_->k, "needs in.srv and out.uav", in.srv.ptr && out.uav.ptr)) return;
    const uint32_t w = std::min(in.width, out.width), h = std::min(in.height, out.height);
    if (!Ok(impl_->k, "empty image", w && h)) return;
    // RCAS' own scale is exp2(-stops); sharpness is that scale directly (stops = -log2(sharpness)), so 0 is a
    // true pass-through and 1 is the maximum (0 stops).
    struct C { uint32_t w, h; float lobeScale; uint32_t pad; } c{w, h, std::clamp(sharpness, 0.f, 1.f), 0};
    impl_->k.Bind(list, &c, &in.srv, &out.uav);
    list->Dispatch(Groups(w, kTile), Groups(h, kTile), 1);
}

} // namespace fluvi::gpu
