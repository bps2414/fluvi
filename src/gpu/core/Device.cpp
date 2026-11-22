// SPDX-License-Identifier: GPL-3.0-only
// D3D12 device: adapter pick, DRED, queues, descriptor heaps, textures, command contexts.
#include "fluvi/gpu/Device.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "fluvi/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace fluvi::gpu {
namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* VendorName(Vendor v) {
    switch (v) {
    case Vendor::Amd: return "AMD";
    case Vendor::Nvidia: return "NVIDIA";
    case Vendor::Intel: return "Intel";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------------------------
// Descriptor heap: first-fit free list of ranges, coalesced on free.
class HeapImpl final : public DescriptorHeap {
public:
    bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible,
              const wchar_t* name) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = type;
        hd.NumDescriptors = capacity;
        hd.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
        heap_->SetName(name);
        stride_ = device->GetDescriptorHandleIncrementSize(type);
        cpuBase_ = heap_->GetCPUDescriptorHandleForHeapStart();
        if (shaderVisible) gpuBase_ = heap_->GetGPUDescriptorHandleForHeapStart();
        capacity_ = capacity;
        free_[0] = capacity;
        name_ = Narrow(name);
        return true;
    }

    ID3D12DescriptorHeap* Heap() const override { return heap_.Get(); }

    DescriptorRange Allocate(uint32_t count) override {
        if (count == 0) return {};
        std::lock_guard lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second < count) continue;
            const uint32_t start = it->first, left = it->second - count;
            free_.erase(it);
            if (left) free_[start + count] = left;
            DescriptorRange r;
            r.index = start;
            r.count = count;
            r.stride = stride_;
            r.cpu.ptr = cpuBase_.ptr + size_t(start) * stride_;
            if (gpuBase_.ptr) r.gpu.ptr = gpuBase_.ptr + uint64_t(start) * stride_;
            return r;
        }
        FLUVI_ERROR("gpu", "descriptor heap {} full ({} requested, capacity {})", name_, count, capacity_);
        return {};
    }

    void Free(const DescriptorRange& range) override {
        if (!range.count) return;
        std::lock_guard lock(mutex_);
        uint32_t start = range.index, count = range.count;
        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + count) {   // merge with the following block
            count += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin()) {                                 // merge with the preceding block
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                prev->second += count;
                return;
            }
        }
        free_[start] = count;
    }

    uint32_t FreeCount() const {
        std::lock_guard lock(mutex_);
        uint32_t n = 0;
        for (auto& [s, c] : free_) n += c;
        return n;
    }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase_{};
    uint32_t stride_ = 0, capacity_ = 0;
    std::string name_;
    mutable std::mutex mutex_;
    std::map<uint32_t, uint32_t> free_;   // start -> count
};

// ---------------------------------------------------------------------------------------------
class TextureImpl final : public Texture {
public:
    TextureImpl(HeapImpl& srvHeap, HeapImpl& rtvHeap) : srvHeap_(srvHeap), rtvHeap_(rtvHeap) {}
    ~TextureImpl() override {
        srvHeap_.Free(srvRange_);
        rtvHeap_.Free(rtvRange_);
    }
    const GpuTexture& View() const override { return view_; }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const override { return rtvRange_ ? rtvRange_.Cpu(0) : D3D12_CPU_DESCRIPTOR_HANDLE{}; }
    void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to) override {
        if (to == state_) {
            if (to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {   // back-to-back UAV work needs a UAV barrier
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = resource_.Get();
                list->ResourceBarrier(1, &b);
            }
            return;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource_.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state_;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
        state_ = to;
    }
    D3D12_RESOURCE_STATES State() const override { return state_; }

    ComPtr<ID3D12Resource> resource_;
    DescriptorRange srvRange_, rtvRange_;
    GpuTexture view_;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;

private:
    HeapImpl& srvHeap_;
    HeapImpl& rtvHeap_;
};

// ---------------------------------------------------------------------------------------------
class ContextImpl final : public CommandContext {
public:
    ~ContextImpl() override {
        if (fence_ && last_) Wait(last_);
        if (event_) CloseHandle(event_);
    }

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12_COMMAND_LIST_TYPE type, uint32_t count) {
        queue_ = queue;
        allocators_.resize(count ? count : 1);
        fenceValues_.assign(allocators_.size(), 0);
        for (auto& a : allocators_)
            if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&a)))) return false;
        ComPtr<ID3D12Device4> d4;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d4)))) return false;
        if (FAILED(d4->CreateCommandList1(0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list_)))) return false;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        list_->SetName(L"fluvi.context.list");
        return event_ != nullptr;
    }

    ID3D12GraphicsCommandList* Begin() override {
        const size_t i = slot_;
        if (fenceValues_[i]) Wait(fenceValues_[i]);
        allocators_[i]->Reset();
        list_->Reset(allocators_[i].Get(), nullptr);
        return list_.Get();
    }

    uint64_t Submit() override {
        if (FAILED(list_->Close())) {
            FLUVI_ERROR("gpu", "command list Close failed");
            return last_;
        }
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t value = ++last_;
        queue_->Signal(fence_.Get(), value);
        fenceValues_[slot_] = value;
        slot_ = (slot_ + 1) % allocators_.size();
        return value;
    }

    bool IsComplete(uint64_t v) const override { return fence_->GetCompletedValue() >= v; }

    void Wait(uint64_t v) override {
        if (IsComplete(v)) return;
        fence_->SetEventOnCompletion(v, event_);
        // A removed device never signals; don't hang forever.
        while (WaitForSingleObject(event_, 2000) == WAIT_TIMEOUT) {
            if (fence_->GetCompletedValue() == UINT64_MAX || IsComplete(v)) return;
            FLUVI_WARN("gpu", "fence wait > 2 s (value {}, completed {})", v, fence_->GetCompletedValue());
        }
    }

    ID3D12Fence* Fence() const override { return fence_.Get(); }
    ID3D12CommandQueue* Queue() const override { return queue_; }

private:
    ID3D12CommandQueue* queue_ = nullptr;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators_;
    std::vector<uint64_t> fenceValues_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t last_ = 0;
    size_t slot_ = 0;
};

// ---------------------------------------------------------------------------------------------
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* names[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
        "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
        "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
        "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame",
        "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
        "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
        "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
        "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata"};
    return size_t(op) < std::size(names) ? names[op] : "?";
}

class DeviceImpl final : public Device {
public:
    ~DeviceImpl() override {
        WaitIdle();
        if (idleEvent_) CloseHandle(idleEvent_);
    }

    bool Init(const DeviceDesc& desc, Status& status) {
        // DRED must be configured before the device exists.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        } else {
            FLUVI_WARN("gpu", "DRED settings unavailable");
        }
        if (desc.debugLayer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
            else FLUVI_WARN("gpu", "D3D12 debug layer requested but not installed");
        }

        if (FAILED(CreateDXGIFactory2(desc.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)))) {
            status = Status::Fail("CreateDXGIFactory2 falhou");
            return false;
        }
        const auto pref = desc.preferHighPerformance ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter4> adapter;
            if (factory_->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC3 ad{};
            adapter->GetDesc3(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
            ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))) &&
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                continue;
            adapter_ = adapter;
            device_ = device;
            info_.name = ad.Description;
            info_.vendor = Vendor(ad.VendorId);
            if (info_.vendor != Vendor::Amd && info_.vendor != Vendor::Nvidia && info_.vendor != Vendor::Intel)
                info_.vendor = Vendor::Unknown;
            info_.deviceId = ad.DeviceId;
            info_.dedicatedVideoMemory = ad.DedicatedVideoMemory;
            info_.luid = ad.AdapterLuid;
            break;
        }
        if (!device_) {
            status = Status::Fail("nenhuma GPU com D3D12 encontrada");
            return false;
        }
        FLUVI_INFO("gpu", "adapter: {} ({} 0x{:04X}), {} MB VRAM, debug layer {}", Narrow(info_.name),
                   VendorName(info_.vendor), info_.deviceId, info_.dedicatedVideoMemory >> 20, desc.debugLayer);
        device_->SetName(L"fluvi.device");

        if (desc.debugLayer) {
            ComPtr<ID3D12InfoQueue> iq;
            if (SUCCEEDED(device_.As(&iq))) {
                iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            }
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct_)))) {
            status = Status::Fail("CreateCommandQueue(direct) falhou");
            return false;
        }
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&compute_)))) {
            status = Status::Fail("CreateCommandQueue(compute) falhou");
            return false;
        }
        direct_->SetName(L"fluvi.queue.direct");
        compute_->SetName(L"fluvi.queue.compute");
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idleFence_[0]))) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idleFence_[1])))) {
            status = Status::Fail("CreateFence falhou");
            return false;
        }
        idleEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!srvHeap_.Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true, L"fluvi.heap.srv_uav") ||
            !rtvHeap_.Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false, L"fluvi.heap.rtv")) {
            status = Status::Fail("CreateDescriptorHeap falhou");
            return false;
        }
        status = Status::Ok();
        return true;
    }

    ID3D12Device* D3D() const override { return device_.Get(); }
    IDXGIFactory6* Factory() const override { return factory_.Get(); }
    IDXGIAdapter4* Adapter() const override { return adapter_.Get(); }
    const AdapterInfo& Info() const override { return info_; }
    ID3D12CommandQueue* DirectQueue() const override { return direct_.Get(); }
    ID3D12CommandQueue* ComputeQueue() const override { return compute_.Get(); }

    std::unique_ptr<CommandContext> CreateContext(D3D12_COMMAND_LIST_TYPE type, uint32_t allocatorCount) override {
        ID3D12CommandQueue* queue = type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? compute_.Get()
                                  : type == D3D12_COMMAND_LIST_TYPE_DIRECT  ? direct_.Get()
                                                                            : nullptr;
        if (!queue) {
            FLUVI_ERROR("gpu", "CreateContext: unsupported list type {}", int(type));
            return nullptr;
        }
        auto ctx = std::make_unique<ContextImpl>();
        if (!ctx->Init(device_.Get(), queue, type, allocatorCount)) {
            FLUVI_ERROR("gpu", "CreateContext failed");
            return nullptr;
        }
        return ctx;
    }

    DescriptorHeap& SrvUavHeap() override { return srvHeap_; }
    DescriptorHeap& RtvHeap() override { return rtvHeap_; }

    std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc, Status& status) override {
        const bool nv12 = desc.format == DXGI_FORMAT_NV12;
        if (!desc.width || !desc.height) {
            status = Status::Fail("textura com tamanho zero");
            return nullptr;
        }
        if (nv12 && (desc.unorderedAccess || desc.renderTarget || (desc.width & 1) || (desc.height & 1))) {
            status = Status::Fail("NV12: só leitura e tamanho par");
            return nullptr;
        }
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = desc.width;
        rd.Height = desc.height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = desc.format;
        rd.SampleDesc.Count = 1;
        if (desc.unorderedAccess) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (desc.renderTarget) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = desc.format;
        auto tex = std::make_unique<TextureImpl>(srvHeap_, rtvHeap_);
        const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, desc.initialState,
                                                            desc.renderTarget ? &clear : nullptr,
                                                            IID_PPV_ARGS(&tex->resource_));
        if (FAILED(hr)) {
            status = Status::Fail(std::format("CreateCommittedResource falhou (0x{:08X})", unsigned(hr)));
            CheckRemoved();
            return nullptr;
        }
        if (desc.debugName) tex->resource_->SetName(desc.debugName);
        tex->state_ = desc.initialState;
        GpuTexture& v = tex->view_;
        v.resource = tex->resource_.Get();
        v.format = desc.format;
        v.width = desc.width;
        v.height = desc.height;

        const uint32_t srvCount = desc.shaderRead ? (nv12 ? 2u : 1u) : 0u;
        const uint32_t total = srvCount + (desc.unorderedAccess ? 1u : 0u);
        if (total) {
            tex->srvRange_ = srvHeap_.Allocate(total);
            if (!tex->srvRange_) {
                status = Status::Fail("sem descritores SRV/UAV livres");
                return nullptr;
            }
        }
        uint32_t slot = 0;
        if (desc.shaderRead) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            if (nv12) {
                sd.Format = DXGI_FORMAT_R8_UNORM;
                sd.Texture2D.PlaneSlice = 0;
                device_->CreateShaderResourceView(v.resource, &sd, tex->srvRange_.Cpu(slot));
                v.srv = tex->srvRange_.Gpu(slot++);
                sd.Format = DXGI_FORMAT_R8G8_UNORM;
                sd.Texture2D.PlaneSlice = 1;
                device_->CreateShaderResourceView(v.resource, &sd, tex->srvRange_.Cpu(slot));
                v.srvChroma = tex->srvRange_.Gpu(slot++);
            } else {
                sd.Format = desc.format;
                device_->CreateShaderResourceView(v.resource, &sd, tex->srvRange_.Cpu(slot));
                v.srv = tex->srvRange_.Gpu(slot++);
            }
        }
        if (desc.unorderedAccess) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = desc.format;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(v.resource, nullptr, &ud, tex->srvRange_.Cpu(slot));
            v.uav = tex->srvRange_.Gpu(slot++);
        }
        if (desc.renderTarget) {
            tex->rtvRange_ = rtvHeap_.Allocate(1);
            if (!tex->rtvRange_) {
                status = Status::Fail("sem descritores RTV livres");
                return nullptr;
            }
            device_->CreateRenderTargetView(v.resource, nullptr, tex->rtvRange_.Cpu(0));
        }
        status = Status::Ok();
        return tex;
    }

    HRESULT CheckRemoved() override {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (SUCCEEDED(reason)) return reason;
        if (!dredLogged_.exchange(true)) LogDred(reason);
        return reason;
    }

    void WaitIdle() override {
        std::lock_guard lock(idleMutex_);
        ID3D12CommandQueue* queues[] = {direct_.Get(), compute_.Get()};
        for (int i = 0; i < 2; ++i) {
            ID3D12Fence* fence = idleFence_[i].Get();
            if (!queues[i] || !fence) continue;
            const uint64_t v = ++idleValue_[i];
            if (FAILED(queues[i]->Signal(fence, v))) continue;
            if (fence->GetCompletedValue() >= v) continue;
            fence->SetEventOnCompletion(v, idleEvent_);
            if (WaitForSingleObject(idleEvent_, 5000) == WAIT_TIMEOUT) {
                FLUVI_WARN("gpu", "WaitIdle timed out");
                CheckRemoved();
            }
        }
    }

private:
    void LogDred(HRESULT reason) {
        FLUVI_ERROR("gpu", "device removed: 0x{:08X}", unsigned(reason));
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(device_.As(&dred))) {
            FLUVI_ERROR("gpu", "DRED data unavailable");
            return;
        }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
            int nodes = 0;
            for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n && nodes < 32; n = n->pNext, ++nodes) {
                const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;   // list fully executed: not the culprit
                FLUVI_ERROR("gpu", "DRED list '{}' on queue '{}': {}/{} ops done",
                            n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?",
                            n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?", done, n->BreadcrumbCount);
                const uint32_t from = done > 4 ? done - 4 : 0, to = std::min(n->BreadcrumbCount, done + 4);
                for (uint32_t i = from; i < to; ++i)
                    FLUVI_ERROR("gpu", "  {} [{}] {}", i == done ? ">>" : "  ", i, BreadcrumbOpName(n->pCommandHistory[i]));
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
            FLUVI_ERROR("gpu", "DRED page fault at VA 0x{:016X}", fault.PageFaultVA);
            for (auto* a = fault.pHeadExistingAllocationNode; a; a = a->pNext)
                FLUVI_ERROR("gpu", "  existing: {}", a->ObjectNameA ? a->ObjectNameA : "?");
            for (auto* a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
                FLUVI_ERROR("gpu", "  recently freed: {}", a->ObjectNameA ? a->ObjectNameA : "?");
        }
    }

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> direct_, compute_;
    ComPtr<ID3D12Fence> idleFence_[2];   // direct, compute
    HANDLE idleEvent_ = nullptr;
    uint64_t idleValue_[2] = {};
    std::mutex idleMutex_;
    std::atomic<bool> dredLogged_{false};
    AdapterInfo info_;
    HeapImpl srvHeap_, rtvHeap_;
};

} // namespace

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, Status& status) {
    auto device = std::make_unique<DeviceImpl>();
    if (!device->Init(desc, status)) return nullptr;
    return device;
}

} // namespace fluvi::gpu
