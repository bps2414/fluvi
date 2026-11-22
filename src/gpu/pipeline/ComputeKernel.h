// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline (internal): one compute kernel = root signature + PSO built from an embedded DXIL blob.
// Root layout: [0] 32-bit constants (b0), then one 1-descriptor table per SRV (t0..), one per UAV (u0..),
// then an optional root UAV (raw buffer). Static samplers: s0 linear clamp, s1 point clamp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi::gpu::detail {

struct KernelSpec {
    const char* name = "";
    const unsigned char* dxil = nullptr;
    size_t dxilSize = 0;
    uint32_t constants = 0;   // 32-bit values in b0
    uint32_t srvs = 0;        // descriptor tables t0..
    uint32_t uavs = 0;        // descriptor tables u0..
    bool rootUav = false;     // one extra root UAV (GPU virtual address) at u<uavs>
};

class ComputeKernel {
public:
    Status Create(ID3D12Device* device, const KernelSpec& spec);
    bool Ready() const { return pso_ != nullptr; }
    // Sets root signature, PSO, constants and descriptors. `srv`/`uav` hold spec.srvs / spec.uavs handles.
    void Bind(ID3D12GraphicsCommandList* list, const void* constants, const D3D12_GPU_DESCRIPTOR_HANDLE* srv,
              const D3D12_GPU_DESCRIPTOR_HANDLE* uav, D3D12_GPU_VIRTUAL_ADDRESS rootUav = 0) const;
    const char* Name() const { return name_; }
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    const char* name_ = "";
    uint32_t constants_ = 0, srvs_ = 0, uavs_ = 0;
    bool rootUav_ = false;
};

inline uint32_t Groups(uint32_t n, uint32_t groupSize) { return (n + groupSize - 1) / groupSize; }
inline bool Has(const GpuTexture& t) { return t.resource != nullptr || t.srv.ptr != 0 || t.uav.ptr != 0; }

} // namespace fluvi::gpu::detail
