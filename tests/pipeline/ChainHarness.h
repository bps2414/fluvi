// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline tests/bench: the QualityChain's TextureAllocator on top of TestGpu (no dependency on fluvi_gpu).
#pragma once
#include <memory>
#include "TestGpu.h"
#include "fluvi/gpu/QualityChain.h"

namespace fluvi::test {
using namespace fluvi::gpu;

// The chain's textures on top of TestGpu.
class TestTexture final : public Texture {
public:
    TestTexture(TestGpu& gpu, Tex t) : gpu_(gpu), tex_(std::move(t)) {}
    const GpuTexture& View() const override { return tex_.gt; }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const override { return {}; }
    void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to) override {
        if (tex_.state == to && to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) TestGpu::UavBarrier(list, tex_);
        else gpu_.Transition(list, tex_, to);
    }
    D3D12_RESOURCE_STATES State() const override { return tex_.state; }
private:
    TestGpu& gpu_;
    Tex tex_;
};

class TestAllocator final : public TextureAllocator {
public:
    explicit TestAllocator(TestGpu& gpu) : gpu_(gpu) {}
    std::unique_ptr<Texture> Create(const TextureDesc& desc, Status& status) override {
        Tex t = gpu_.Create(desc.width, desc.height, desc.format, desc.unorderedAccess);
        if (!t.res) {
            status = Status::Fail("TestGpu::Create failed");
            return nullptr;
        }
        status = Status::Ok();
        return std::make_unique<TestTexture>(gpu_, std::move(t));
    }
    ID3D12DescriptorHeap* ShaderHeap() const override { return gpu_.Heap(); }
private:
    TestGpu& gpu_;
};


} // namespace fluvi::test
