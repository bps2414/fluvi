// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline tests: a self-contained D3D12 device (hardware adapter, debug layer + info queue), a
// shader-visible descriptor heap and texture upload/readback helpers. Independent of fluvi_gpu.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "fluvi/gpu/Texture.h"

namespace fluvi::test {

struct Tex {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    gpu::GpuTexture gt;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;
};

struct GpuOptions {
    bool debugLayer = true;
    bool gpuValidation = false;
};

class TestGpu {
public:
    explicit TestGpu(const GpuOptions& options);
    ~TestGpu();
    // One shared instance for the doctest binary (debug layer on; GPU-based validation unless FLUVI_TEST_GBV=0).
    static TestGpu& Shared();

    bool Valid() const { return device_ != nullptr; }
    bool DebugLayerOn() const { return debugOn_; }
    const std::string& AdapterName() const { return adapterName_; }
    ID3D12Device* Device() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }

    // Per-test bookkeeping: frees descriptors, clears stored debug messages.
    void BeginTest();
    // Returns the number of debug-layer errors since BeginTest (+1 if the device was removed); text goes to `report`.
    int EndTest(std::string* report);

    // Textures live in COMMON; descriptors (srv always, uav when asked) go in the shader-visible heap.
    Tex Create(uint32_t w, uint32_t h, DXGI_FORMAT format, bool uav);
    Tex CreateNv12(uint32_t w, uint32_t h);
    // Single plane upload (row-major, `rowBytes` per row); leaves the texture in NON_PIXEL_SHADER_RESOURCE.
    void Upload(Tex& t, const void* data, size_t rowBytes);
    void UploadNv12(Tex& t, const uint8_t* luma, const uint8_t* chromaInterleaved);   // tight pitches
    // Returns rows tightly packed (w * bytes per texel); restores the previous state.
    std::vector<uint8_t> Readback(Tex& t);
    // A small default-heap buffer with UAV flag for results (zeroed), state UNORDERED_ACCESS.
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateResultBuffer(uint64_t bytes);
    std::vector<uint8_t> ReadbackBuffer(ID3D12Resource* buffer, uint64_t bytes, D3D12_RESOURCE_STATES current);

    void Transition(ID3D12GraphicsCommandList* list, Tex& t, D3D12_RESOURCE_STATES to);
    static void UavBarrier(ID3D12GraphicsCommandList* list, const Tex& t);
    // Records with the heap bound, executes and waits.
    void Run(const std::function<void(ID3D12GraphicsCommandList*)>& record);
    void WaitIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::string adapterName_;
    bool debugOn_ = false;
    uint32_t descriptorSize_ = 0, nextDescriptor_ = 0;
    static constexpr uint32_t kHeapSize = 4096;
    D3D12_GPU_DESCRIPTOR_HANDLE NewDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu);
};

} // namespace fluvi::test
