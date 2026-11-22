// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: a non-owning view of a GPU texture, the currency between gpu-core, the pipeline passes, media and
// the UI. Raw D3D12 types on purpose: a pass only needs a device and a command list, not gpu-core.
#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace fluvi::gpu {

// Formats used across modules.
inline constexpr DXGI_FORMAT kIntermediateFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // between passes
inline constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;              // last pass + swapchain

struct GpuTexture {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    // Shader-visible CBV_SRV_UAV descriptors in the heap the caller binds. Zero (ptr == 0) when absent.
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};          // RGB textures; for NV12: the luma plane (R8_UNORM view, plane 0)
    D3D12_GPU_DESCRIPTOR_HANDLE srvChroma{};    // NV12 only: the chroma plane (R8G8_UNORM view, plane 1)
    D3D12_GPU_DESCRIPTOR_HANDLE uav{};          // writable textures
};

} // namespace fluvi::gpu
