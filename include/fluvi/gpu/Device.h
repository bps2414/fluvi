// SPDX-License-Identifier: GPL-3.0-only
// Fluvi gpu-core: the D3D12 device and its queues, descriptors, textures and command recording.
// Owner: agent A (src/gpu/core). Interface first; the implementation is free behind it.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "fluvi/core/Status.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi::gpu {

enum class Vendor : uint32_t { Unknown = 0, Amd = 0x1002, Nvidia = 0x10DE, Intel = 0x8086 };

struct DeviceDesc {
    bool debugLayer = false;       // D3D12 debug layer (no GPU-based validation); DRED is always on
    bool preferHighPerformance = true;
};

struct AdapterInfo {
    std::wstring name;
    Vendor vendor = Vendor::Unknown;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;
    LUID luid{};
};

// A range of descriptors in a heap. Free it with the heap it came from.
struct DescriptorRange {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};    // zero for non-shader-visible heaps
    uint32_t index = 0, count = 0;
    uint32_t stride = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t i) const { return {cpu.ptr + size_t(i) * stride}; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t i) const { return {gpu.ptr + uint64_t(i) * stride}; }
    explicit operator bool() const { return count != 0; }
};

class DescriptorHeap {
public:
    virtual ~DescriptorHeap() = default;
    virtual ID3D12DescriptorHeap* Heap() const = 0;
    virtual DescriptorRange Allocate(uint32_t count) = 0;   // thread-safe; empty range when full (logged)
    virtual void Free(const DescriptorRange& range) = 0;     // thread-safe; caller guarantees the GPU is done with it
};

struct TextureDesc {
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = kIntermediateFormat;   // DXGI_FORMAT_NV12 allowed (SRVs for both planes)
    bool shaderRead = true;                     // create SRV(s)
    bool unorderedAccess = false;               // create a UAV (and ALLOW_UNORDERED_ACCESS)
    bool renderTarget = false;                  // create an RTV in the RTV heap
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    const wchar_t* debugName = nullptr;
};

// An owned texture with its descriptors; frees everything on destruction (caller ensures the GPU is done).
class Texture {
public:
    virtual ~Texture() = default;
    virtual const GpuTexture& View() const = 0;
    virtual D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const = 0;              // zero if !renderTarget
    // Tracked state for convenience: records a barrier only if the state differs.
    virtual void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to) = 0;
    virtual D3D12_RESOURCE_STATES State() const = 0;
};

// Records on a ring of allocators for one queue. Begin -> record -> Submit; fence values are monotonic.
class CommandContext {
public:
    virtual ~CommandContext() = default;
    virtual ID3D12GraphicsCommandList* Begin() = 0;   // waits only if the allocator it reuses is still in flight
    virtual uint64_t Submit() = 0;                     // closes, executes, signals; returns the fence value
    virtual bool IsComplete(uint64_t fenceValue) const = 0;
    virtual void Wait(uint64_t fenceValue) = 0;        // CPU wait
    virtual ID3D12Fence* Fence() const = 0;
    virtual ID3D12CommandQueue* Queue() const = 0;
};

class Device {
public:
    static std::unique_ptr<Device> Create(const DeviceDesc& desc, Status& status);
    virtual ~Device() = default;

    virtual ID3D12Device* D3D() const = 0;
    virtual IDXGIFactory6* Factory() const = 0;
    virtual IDXGIAdapter4* Adapter() const = 0;
    virtual const AdapterInfo& Info() const = 0;

    virtual ID3D12CommandQueue* DirectQueue() const = 0;
    virtual ID3D12CommandQueue* ComputeQueue() const = 0;    // async compute
    // allocatorCount = frames in flight. One context per recording thread.
    virtual std::unique_ptr<CommandContext> CreateContext(D3D12_COMMAND_LIST_TYPE type, uint32_t allocatorCount) = 0;

    virtual DescriptorHeap& SrvUavHeap() = 0;   // shader-visible CBV_SRV_UAV, big enough for the whole app
    virtual DescriptorHeap& RtvHeap() = 0;

    virtual std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc, Status& status) = 0;

    // S_OK while alive; the removed reason otherwise. On removal the implementation logs the DRED
    // breadcrumbs and page-fault data once.
    virtual HRESULT CheckRemoved() = 0;
    virtual void WaitIdle() = 0;   // all queues
};

} // namespace fluvi::gpu
