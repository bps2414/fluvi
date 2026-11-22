// SPDX-License-Identifier: GPL-3.0-only
// gpu-core on real hardware (label gpu).
#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/SwapChain.h"

using namespace fluvi;
using namespace fluvi::gpu;

namespace {

Device& SharedDevice() {
    static std::unique_ptr<Device> device = [] {
        Status st;
        auto d = Device::Create({}, st);
        REQUIRE_MESSAGE(d, st.message);
        return d;
    }();
    return *device;
}

} // namespace

TEST_CASE("device has an adapter, queues and heaps") {
    Device& d = SharedDevice();
    CHECK(d.D3D());
    CHECK(d.Factory());
    CHECK(d.Adapter());
    CHECK_FALSE(d.Info().name.empty());
    CHECK(d.Info().dedicatedVideoMemory > 0);
    CHECK(d.DirectQueue());
    CHECK(d.ComputeQueue());
    CHECK(d.DirectQueue()->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    CHECK(d.ComputeQueue()->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    CHECK(d.SrvUavHeap().Heap()->GetDesc().Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    CHECK(d.SrvUavHeap().Heap()->GetDesc().NumDescriptors >= 4096);
    CHECK(d.CheckRemoved() == S_OK);
}

TEST_CASE("textures: NV12 planes, RGBA16F UAV, RGBA8 RTV") {
    Device& d = SharedDevice();
    Status st;
    TextureDesc nv12;
    nv12.width = 1920;
    nv12.height = 1080;
    nv12.format = DXGI_FORMAT_NV12;
    nv12.debugName = L"test.nv12";
    auto a = d.CreateTexture(nv12, st);
    REQUIRE_MESSAGE(a, st.message);
    CHECK(a->View().srv.ptr != 0);
    CHECK(a->View().srvChroma.ptr != 0);
    CHECK(a->View().srvChroma.ptr != a->View().srv.ptr);
    CHECK(a->View().uav.ptr == 0);
    CHECK(a->Rtv().ptr == 0);
    CHECK(a->View().resource->GetDesc().Format == DXGI_FORMAT_NV12);

    nv12.unorderedAccess = true;
    CHECK_FALSE(d.CreateTexture(nv12, st));
    CHECK_FALSE(st.ok);

    TextureDesc hdr;
    hdr.width = 1920;
    hdr.height = 1080;
    hdr.unorderedAccess = true;
    hdr.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto b = d.CreateTexture(hdr, st);
    REQUIRE_MESSAGE(b, st.message);
    CHECK(b->View().format == kIntermediateFormat);
    CHECK(b->View().srv.ptr != 0);
    CHECK(b->View().uav.ptr != 0);
    CHECK(b->State() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK((b->View().resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);

    TextureDesc out;
    out.width = 1280;
    out.height = 720;
    out.format = kOutputFormat;
    out.renderTarget = true;
    out.shaderRead = false;
    auto c = d.CreateTexture(out, st);
    REQUIRE_MESSAGE(c, st.message);
    CHECK(c->Rtv().ptr != 0);
    CHECK(c->View().srv.ptr == 0);
}

TEST_CASE("descriptor heap: alloc, free, reuse, full, threads") {
    Status st;
    auto d = Device::Create({}, st);   // own device: an empty heap
    REQUIRE(d);
    DescriptorHeap& heap = d->SrvUavHeap();
    const uint32_t cap = heap.Heap()->GetDesc().NumDescriptors;

    auto a = heap.Allocate(10);
    auto b = heap.Allocate(5);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(b.index == a.index + 10);
    CHECK(a.Cpu(1).ptr == a.cpu.ptr + a.stride);
    CHECK(a.gpu.ptr != 0);
    heap.Free(a);
    auto c = heap.Allocate(4);
    CHECK(c.index == a.index);   // first fit reuses the hole
    heap.Free(c);
    heap.Free(b);
    CHECK_FALSE(heap.Allocate(cap + 1));
    auto all = heap.Allocate(cap);   // everything coalesced back
    CHECK(all);
    heap.Free(all);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&heap, t] {
            for (int i = 0; i < 2000; ++i) {
                auto r = heap.Allocate(1 + uint32_t((i + t) % 7));
                if (r) heap.Free(r);
            }
        });
    for (auto& th : threads) th.join();
    auto again = heap.Allocate(cap);
    CHECK(again);
    heap.Free(again);

    auto rtv = d->RtvHeap().Allocate(3);
    CHECK(rtv);
    CHECK(rtv.gpu.ptr == 0);
    d->RtvHeap().Free(rtv);
}

TEST_CASE("command context: submit and wait across the ring") {
    Device& d = SharedDevice();
    Status st;
    TextureDesc td;
    td.width = 256;
    td.height = 256;
    td.renderTarget = true;
    td.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    auto tex = d.CreateTexture(td, st);
    REQUIRE(tex);

    auto ctx = d.CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, 3);
    REQUIRE(ctx);
    uint64_t last = 0;
    for (int i = 0; i < 12; ++i) {
        auto* list = ctx->Begin();
        REQUIRE(list);
        tex->Transition(list, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[4] = {float(i) / 12, 0, 0, 1};
        list->ClearRenderTargetView(tex->Rtv(), color, 0, nullptr);
        tex->Transition(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tex->Transition(list, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const uint64_t v = ctx->Submit();
        CHECK(v == last + 1);
        last = v;
    }
    ctx->Wait(last);
    CHECK(ctx->IsComplete(last));
    CHECK(ctx->Fence()->GetCompletedValue() >= last);
    CHECK(ctx->Queue() == d.DirectQueue());

    auto compute = d.CreateContext(D3D12_COMMAND_LIST_TYPE_COMPUTE, 2);
    REQUIRE(compute);
    for (int i = 0; i < 4; ++i) {
        compute->Begin();
        last = compute->Submit();
    }
    compute->Wait(last);
    CHECK(compute->IsComplete(last));
    CHECK(compute->Queue() == d.ComputeQueue());
    d.WaitIdle();
    CHECK(d.CheckRemoved() == S_OK);
}

TEST_CASE("swapchain presents with tearing on a real window") {
    Device& d = SharedDevice();
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FluviGpuTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"fluvi gpu test", WS_OVERLAPPEDWINDOW, 100, 100, 640, 360,
                                nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    Status st;
    SwapChainDesc sd;
    sd.hwnd = hwnd;
    auto swap = SwapChain::Create(d, sd, st);
    REQUIRE_MESSAGE(swap, st.message);
    CHECK(swap->Width() > 0);
    const DisplayInfo info = swap->Display();
    CHECK_FALSE(info.deviceName.empty());
    CHECK(info.refreshHz > 20);
    CHECK(info.refreshDenominator > 0);
    MESSAGE("display ", info.refreshNumerator, "/",
            info.refreshDenominator, " Hz, VRR ", info.vrrEnabled ? (*info.vrrEnabled ? "on" : "off") : "unknown",
            ", tearing ", info.tearingSupported);

    auto ctx = d.CreateContext(D3D12_COMMAND_LIST_TYPE_DIRECT, 3);
    auto frame = [&](int i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        REQUIRE(swap->WaitForNextFrame(1000));
        const uint32_t bi = swap->CurrentIndex();
        auto* list = ctx->Begin();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = swap->BackBuffer(bi);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        const float c[4] = {float(i % 60) / 60.f, 0.2f, 0.4f, 1};
        list->ClearRenderTargetView(swap->Rtv(bi), c, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        ctx->Submit();
        REQUIRE(swap->Present({0, true}).ok);
    };
    for (int i = 0; i < 120; ++i) frame(i);

    const PresentStats s = swap->Stats();
    CHECK(s.presents == 120);
    CHECK(s.intervalAvgMs > 0);
    CHECK(s.intervalP99Ms >= s.intervalAvgMs * 0.5);
    CHECK(s.intervalMaxMs >= s.intervalP99Ms);
    CHECK(s.lastPresentQpc > 0);
    MESSAGE("intervals avg ", s.intervalAvgMs, " p99 ", s.intervalP99Ms, " max ", s.intervalMaxMs, " ms, late ",
            s.lateFrames, ", mode ", int(s.mode), ", shown ", s.lastShownQpc != 0);

    REQUIRE(swap->Resize(800, 450).ok);
    CHECK(swap->Width() == 800);
    for (int i = 0; i < 10; ++i) frame(i);
    CHECK(swap->Stats().presents == 130);

    swap.reset();
    DestroyWindow(hwnd);
    CHECK(d.CheckRemoved() == S_OK);
}
