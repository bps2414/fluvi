// SPDX-License-Identifier: GPL-3.0-only
// fluvi_pipeline_bench (manual tool, not a test): loads a PNG, builds an NV12 frame on the CPU (BT.709 limited), runs
// NV12 -> RGB -> deblock -> downscale -> RCAS on the GPU, writes a PNG per stage next to the input and prints GPU
// timestamp timings per pass.
//   fluvi_pipeline_bench --generate <out.png>            writes a procedural 2560x1440 test image
//   fluvi_pipeline_bench <in.png> [--out W H] [--iters N] [--deblock S] [--sharp S] [--debug]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "../Images.h"
#include "../TestGpu.h"
#include "fluvi/gpu/Passes.h"

using Microsoft::WRL::ComPtr;
using namespace fluvi;
using namespace fluvi::gpu;
using namespace fluvi::test;

namespace {

ComPtr<IWICImagingFactory> g_wic;

bool LoadPng(const std::wstring& path, Image* out) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) return false;
    *out = Image(w, h);
    for (size_t i = 0; i < px.size(); ++i) out->px[i] = px[i] / 255.f;
    return true;
}

bool SavePng(const std::wstring& path, const Image& img) {
    std::vector<uint8_t> px(img.px.size());
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(std::lround(std::clamp(img.px[i], 0.f, 1.f) * 255.f));
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(g_wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    frame->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(px.size()), px.data())) || FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
    return true;
}

Image GenerateTestImage() {
    // Left third: smooth noise (photo-like), middle: text-like strokes, right: zone plate. Hard compression-style
    // block seams are added later by the NV12 quantisation.
    const uint32_t w = 2560, h = 1440;
    Image noise = Noise(w, h, 4, 40), text = TextEdges(w, h), zone = ZonePlate(w, h);
    Image img(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const Image& s = x < w / 3 ? noise : x < 2 * w / 3 ? text : zone;
            for (int c = 0; c < 4; ++c) img.at(x, y, c) = s.at(x, y, c);
        }
    return img;
}

struct Timer {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    uint32_t count = 0;
    void Init(ID3D12Device* d, uint32_t n) {
        count = n;
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, n, 0};
        d->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap));
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = uint64_t(n) * 8;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    }
    void Mark(ID3D12GraphicsCommandList* l, uint32_t i) const { l->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i); }
    void Resolve(ID3D12GraphicsCommandList* l) const { l->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, readback.Get(), 0); }
    std::vector<uint64_t> Read() const {
        std::vector<uint64_t> v(count);
        void* p = nullptr;
        readback->Map(0, nullptr, &p);
        std::memcpy(v.data(), p, v.size() * 8);
        readback->Unmap(0, nullptr);
        return v;
    }
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) { std::fprintf(stderr, "WIC unavailable\n"); return 1; }
    std::wstring input;
    uint32_t outW = 1920, outH = 1080, iters = 20;
    float deblock = 0.5f, sharp = 0.3f;
    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--generate" && i + 1 < argc) {
            const bool ok = SavePng(argv[i + 1], GenerateTestImage());
            std::wprintf(L"%s %s\n", ok ? L"wrote" : L"FAILED to write", argv[i + 1]);
            return ok ? 0 : 1;
        }
        if (a == L"--out" && i + 2 < argc) { outW = std::wcstoul(argv[++i], nullptr, 10); outH = std::wcstoul(argv[++i], nullptr, 10); }
        else if (a == L"--iters" && i + 1 < argc) iters = std::wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--deblock" && i + 1 < argc) deblock = static_cast<float>(std::wcstod(argv[++i], nullptr));
        else if (a == L"--sharp" && i + 1 < argc) sharp = static_cast<float>(std::wcstod(argv[++i], nullptr));
        else if (a == L"--debug") debug = true;
        else input = a;
    }
    if (input.empty()) { std::fprintf(stderr, "usage: fluvi_pipeline_bench <in.png> [--out W H] [--iters N] [--deblock S] [--sharp S] [--debug] | --generate <out.png>\n"); return 2; }

    Image src;
    if (!LoadPng(input, &src)) { std::fprintf(stderr, "cannot read the input PNG\n"); return 1; }
    const Nv12Frame frame = RgbToNv12(src, YuvMatrix::Bt709, false);
    std::printf("input %ux%u (NV12 %ux%u), output %ux%u, deblock %.2f, sharpness %.2f\n", src.w, src.h, frame.w, frame.h, outW, outH, deblock, sharp);

    GpuOptions options;
    options.debugLayer = debug;
    TestGpu gpu(options);
    if (!gpu.Valid()) { std::fprintf(stderr, "no D3D12 hardware adapter\n"); return 1; }
    std::printf("adapter: %s%s\n", gpu.AdapterName().c_str(), gpu.DebugLayerOn() ? " (debug layer)" : "");

    Nv12ToRgbPass nv;
    DeblockPass db;
    DownscalePass ds;
    RcasPass rc;
    for (const Status& st : {nv.Init(gpu.Device()), db.Init(gpu.Device()), ds.Init(gpu.Device()), rc.Init(gpu.Device())})
        if (!st.ok) { std::fprintf(stderr, "pass init failed: %s\n", st.message.c_str()); return 1; }

    Tex nv12 = gpu.CreateNv12(frame.w, frame.h);
    gpu.UploadNv12(nv12, frame.y.data(), frame.uv.data());
    Tex rgb = gpu.Create(frame.w, frame.h, kIntermediateFormat, true), deb = gpu.Create(frame.w, frame.h, kIntermediateFormat, true);
    Tex scaled = gpu.Create(outW, outH, kIntermediateFormat, true), fin = gpu.Create(outW, outH, kOutputFormat, true);

    Timer timer;
    timer.Init(gpu.Device(), 5);
    auto chain = [&](ID3D12GraphicsCommandList* l) {
        timer.Mark(l, 0);
        gpu.Transition(l, rgb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        nv.Record(l, nv12.gt, rgb.gt, {});
        TestGpu::UavBarrier(l, rgb);
        gpu.Transition(l, rgb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        timer.Mark(l, 1);
        gpu.Transition(l, deb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        db.Record(l, rgb.gt, deb.gt, deblock);
        TestGpu::UavBarrier(l, deb);
        gpu.Transition(l, deb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        timer.Mark(l, 2);
        gpu.Transition(l, scaled, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ds.Record(l, deb.gt, scaled.gt, DownscaleFilter::Lanczos3);
        TestGpu::UavBarrier(l, scaled);
        gpu.Transition(l, scaled, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        timer.Mark(l, 3);
        gpu.Transition(l, fin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rc.Record(l, scaled.gt, fin.gt, sharp);
        TestGpu::UavBarrier(l, fin);
        timer.Mark(l, 4);
        timer.Resolve(l);
    };

    const double freq = [&] { UINT64 f = 0; gpu.Queue()->GetTimestampFrequency(&f); return double(f); }();
    const char* names[4] = {"NV12 -> RGB", "deblock", "downscale (Lanczos3)", "RCAS"};
    double sum[4] = {}, best[4] = {1e9, 1e9, 1e9, 1e9}, totalSum = 0;
    for (uint32_t it = 0; it < iters + 1; ++it) {   // first iteration is a warm-up
        gpu.Run(chain);
        const auto t = timer.Read();
        if (it == 0) continue;
        for (int p = 0; p < 4; ++p) {
            const double ms = double(t[p + 1] - t[p]) * 1000.0 / freq;
            sum[p] += ms;
            best[p] = std::min(best[p], ms);
        }
        totalSum += double(t[4] - t[0]) * 1000.0 / freq;
    }
    std::printf("GPU time per pass over %u runs (avg / best, ms):\n", iters);
    for (int p = 0; p < 4; ++p) std::printf("  %-22s %7.3f / %7.3f\n", names[p], sum[p] / iters, best[p]);
    std::printf("  %-22s %7.3f\n", "whole chain (avg)", totalSum / iters);

    const std::filesystem::path base = std::filesystem::path(input).replace_extension();
    struct Stage { const char* suffix; Tex* tex; };
    for (const Stage& s : {Stage{"_1_nv12_rgb", &rgb}, Stage{"_2_deblock", &deb}, Stage{"_3_downscale", &scaled}, Stage{"_4_rcas", &fin}}) {
        const std::wstring path = base.wstring() + std::wstring(s.suffix, s.suffix + std::strlen(s.suffix)) + L".png";
        std::wprintf(L"%s %s\n", SavePng(path, Download(gpu, *s.tex)) ? L"wrote" : L"FAILED", path.c_str());
    }
    return 0;
}
