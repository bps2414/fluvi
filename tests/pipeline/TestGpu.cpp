// SPDX-License-Identifier: GPL-3.0-only
#include "TestGpu.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include <windows.h>

namespace fluvi::test {
using Microsoft::WRL::ComPtr;

struct TestGpu::Impl {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12InfoQueue> infoQueue;
};

namespace {
D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = t;
    return h;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}
} // namespace

TestGpu::TestGpu(const GpuOptions& options) : impl_(std::make_unique<Impl>()) {
    if (options.debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debugOn_ = true;
            ComPtr<ID3D12Debug1> debug1;
            if (options.gpuValidation && SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
        }
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(debugOn_ ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            adapterName_ = name;
            break;
        }
    }
    if (!device_) return;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))) ||
        FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl_->allocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->allocator.Get(), nullptr, IID_PPV_ARGS(&impl_->list))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence)))) {
        device_.Reset();
        return;
    }
    impl_->list->Close();
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (debugOn_ && SUCCEEDED(device_.As(&impl_->infoQueue))) {
        D3D12_MESSAGE_SEVERITY allow[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.AllowList.NumSeverities = 2;
        filter.AllowList.pSeverityList = allow;
        impl_->infoQueue->PushStorageFilter(&filter);
        impl_->infoQueue->SetMessageCountLimit(2000);
    }
}

TestGpu::~TestGpu() {
    if (device_ && impl_) WaitIdle();
    if (impl_ && impl_->event) CloseHandle(impl_->event);
}

TestGpu& TestGpu::Shared() {
    static TestGpu gpu([] {
        GpuOptions o;
        char gbv[8] = {}; size_t gbvLen = 0;
        getenv_s(&gbvLen, gbv, sizeof gbv, "FLUVI_TEST_GBV");
        o.gpuValidation = !(gbvLen > 0 && gbv[0] == '0');
        return o;
    }());
    return gpu;
}

void TestGpu::BeginTest() {
    WaitIdle();
    nextDescriptor_ = 0;
    if (impl_->infoQueue) impl_->infoQueue->ClearStoredMessages();
}

int TestGpu::EndTest(std::string* report) {
    WaitIdle();
    int errors = 0;
    if (FAILED(device_->GetDeviceRemovedReason())) {
        ++errors;
        if (report) *report += std::format("device removed: 0x{:08X}\n", static_cast<uint32_t>(device_->GetDeviceRemovedReason()));
    }
    if (impl_->infoQueue) {
        const UINT64 n = impl_->infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            impl_->infoQueue->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (FAILED(impl_->infoQueue->GetMessage(i, msg, &len))) continue;
            ++errors;
            if (report && i < 10) *report += std::format("[D3D12 id {}] {}\n", static_cast<int>(msg->ID), msg->pDescription);
        }
    }
    return errors;
}

D3D12_GPU_DESCRIPTOR_HANDLE TestGpu::NewDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu) {
    const uint32_t i = nextDescriptor_++;
    auto c = heap_->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(i) * descriptorSize_;
    auto g = heap_->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(i) * descriptorSize_;
    *cpu = c;
    return g;
}

Tex TestGpu::Create(uint32_t w, uint32_t h, DXGI_FORMAT format, bool uav) {
    Tex t;
    t.w = w;
    t.h = h;
    t.format = format;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.res)))) return t;
    t.gt.resource = t.res.Get();
    t.gt.format = format;
    t.gt.width = w;
    t.gt.height = h;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    t.gt.srv = NewDescriptor(&cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t.res.Get(), &sv, cpu);
    if (uav) {
        t.gt.uav = NewDescriptor(&cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = format;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(t.res.Get(), nullptr, &uv, cpu);
    }
    return t;
}

Tex TestGpu::CreateNv12(uint32_t w, uint32_t h) {
    Tex t;
    t.w = w;
    t.h = h;
    t.format = DXGI_FORMAT_NV12;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.res)))) return t;
    t.gt.resource = t.res.Get();
    t.gt.format = DXGI_FORMAT_NV12;
    t.gt.width = w;
    t.gt.height = h;
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        const auto gpu = NewDescriptor(&cpu);
        (plane == 0 ? t.gt.srv : t.gt.srvChroma) = gpu;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = plane == 0 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        sv.Texture2D.PlaneSlice = plane;
        device_->CreateShaderResourceView(t.res.Get(), &sv, cpu);
    }
    return t;
}

void TestGpu::Transition(ID3D12GraphicsCommandList* list, Tex& t, D3D12_RESOURCE_STATES to) {
    if (t.state == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
    t.state = to;
}

void TestGpu::UavBarrier(ID3D12GraphicsCommandList* list, const Tex& t) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = t.res.Get();
    list->ResourceBarrier(1, &b);
}

void TestGpu::Upload(Tex& t, const void* data, size_t rowBytes) {
    const auto desc = t.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> up;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto bd = BufferDesc(total);
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + y * rowBytes, std::min<size_t>(rowBytes, rowSize));
    up->Unmap(0, nullptr);
    Run([&](ID3D12GraphicsCommandList* l) {
        Transition(l, t, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = t.res.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(l, t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    });
}

void TestGpu::UploadNv12(Tex& t, const uint8_t* luma, const uint8_t* chroma) {
    const auto desc = t.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    UINT rows[2]{};
    UINT64 rowSize[2]{}, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 2, 0, fp, rows, rowSize, &total);
    ComPtr<ID3D12Resource> up;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto bd = BufferDesc(total);
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    const uint8_t* src[2] = {luma, chroma};
    for (int plane = 0; plane < 2; ++plane)
        for (UINT y = 0; y < rows[plane]; ++y)
            std::memcpy(p + fp[plane].Offset + static_cast<size_t>(y) * fp[plane].Footprint.RowPitch, src[plane] + static_cast<size_t>(y) * rowSize[plane], rowSize[plane]);
    up->Unmap(0, nullptr);
    Run([&](ID3D12GraphicsCommandList* l) {
        Transition(l, t, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, s{};
            dst.pResource = t.res.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = plane;
            s.pResource = up.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp[plane];
            l->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        }
        Transition(l, t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    });
}

std::vector<uint8_t> TestGpu::Readback(Tex& t) {
    const auto desc = t.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> rb;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
    const auto bd = BufferDesc(total);
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    const auto before = t.state;
    Run([&](ID3D12GraphicsCommandList* l) {
        Transition(l, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = t.res.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(l, t, before);
    });
    std::vector<uint8_t> out(static_cast<size_t>(rowSize) * rows);
    uint8_t* p = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    rb->Map(0, &range, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < rows; ++y) std::memcpy(out.data() + static_cast<size_t>(y) * rowSize, p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, rowSize);
    rb->Unmap(0, nullptr);
    return out;
}

ComPtr<ID3D12Resource> TestGpu::CreateResultBuffer(uint64_t bytes) {
    ComPtr<ID3D12Resource> buf;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const auto bd = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf));
    // Zero it with a copy from a zeroed upload buffer.
    ComPtr<ID3D12Resource> up;
    const auto uh = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto ud = BufferDesc(bytes);
    device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    std::memset(p, 0, static_cast<size_t>(bytes));
    up->Unmap(0, nullptr);
    Run([&](ID3D12GraphicsCommandList* l) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buf.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1, &b);
        l->CopyBufferRegion(buf.Get(), 0, up.Get(), 0, bytes);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        l->ResourceBarrier(1, &b);
    });
    return buf;
}

std::vector<uint8_t> TestGpu::ReadbackBuffer(ID3D12Resource* buffer, uint64_t bytes, D3D12_RESOURCE_STATES current) {
    ComPtr<ID3D12Resource> rb;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
    const auto bd = BufferDesc(bytes);
    device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    Run([&](ID3D12GraphicsCommandList* l) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = current;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        l->ResourceBarrier(1, &b);
        l->CopyBufferRegion(rb.Get(), 0, buffer, 0, bytes);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        l->ResourceBarrier(1, &b);
    });
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    void* p = nullptr;
    rb->Map(0, nullptr, &p);
    std::memcpy(out.data(), p, out.size());
    rb->Unmap(0, nullptr);
    return out;
}

void TestGpu::Run(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
    impl_->allocator->Reset();
    impl_->list->Reset(impl_->allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    impl_->list->SetDescriptorHeaps(1, heaps);
    record(impl_->list.Get());
    impl_->list->Close();
    ID3D12CommandList* lists[] = {impl_->list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    WaitIdle();
}

void TestGpu::WaitIdle() {
    if (!queue_) return;
    const uint64_t v = ++impl_->fenceValue;
    queue_->Signal(impl_->fence.Get(), v);
    if (impl_->fence->GetCompletedValue() < v) {
        impl_->fence->SetEventOnCompletion(v, impl_->event);
        WaitForSingleObject(impl_->event, 120000);
    }
}

} // namespace fluvi::test
