// SPDX-License-Identifier: GPL-3.0-only
// Fluvi gpu: FSR 3.1 frame interpolation into app-owned textures. Contract: include/fluvi/gpu/FrameGen.h.
// Ported from Veyra (GPL-3.0-only), src/gfx/FsrFgPresenter.cpp @ c6633121. Changes:
// Fluvi logging/Status, DLL search like Fsr4Backend (exe dir, FLUVI_FFX_DIR, SDK signedbin), zero motion + constant
// depth owned here, prefers the 3.1 provider and falls back to the default one.
#include "fluvi/gpu/FrameGen.h"

#include <cmath>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

#include <windows.h>
#include <wrl/client.h>

#include "fluvi/core/Log.h"

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

namespace fluvi::gpu {
namespace {

using Microsoft::WRL::ComPtr;

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf).parent_path() : std::filesystem::path();
}

HMODULE LoadLoader(std::string& error) {
    std::vector<std::filesystem::path> dirs{ExeDir()};
    wchar_t env[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"FLUVI_FFX_DIR", env, static_cast<DWORD>(std::size(env))) > 0) dirs.emplace_back(env);
#ifdef FLUVI_FFX_SIGNEDBIN_DIR
    dirs.emplace_back(FLUVI_FFX_SIGNEDBIN_DIR);
#endif
    for (const auto& dir : dirs) {
        std::error_code ec;
        const auto loader = dir / L"amd_fidelityfx_loader_dx12.dll";
        if (!std::filesystem::exists(loader, ec) || !std::filesystem::exists(dir / L"amd_fidelityfx_framegeneration_dx12.dll", ec))
            continue;
        if (HMODULE m = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
        error = std::format("LoadLibrary({}) falhou ({})", loader.string(), GetLastError());
    }
    if (error.empty()) error = "amd_fidelityfx_framegeneration_dx12.dll não encontrada (exe dir, FLUVI_FFX_DIR, signedbin)";
    return nullptr;
}

uint32_t SurfaceFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource Res(ID3D12Resource* r) {
    FfxApiResource api{};
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    api.resource = r;
    api.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    api.description.format = SurfaceFormat(d.Format);
    api.description.width = static_cast<uint32_t>(d.Width);
    api.description.height = d.Height;
    api.description.depth = d.DepthOrArraySize;
    api.description.mipCount = d.MipLevels;
    api.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        api.description.usage = static_cast<uint32_t>(api.description.usage | FFX_API_RESOURCE_USAGE_UAV);
    api.state = FFX_API_RESOURCE_STATE_COMMON;
    return api;
}

// Committed textures start zeroed: zero motion vectors, depth 0 everywhere.
ComPtr<ID3D12Resource> ZeroTexture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT f) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

class FsrFg final : public FrameGenerator {
public:
    ~FsrFg() override {
        if (ctx_ && fn_.DestroyContext) fn_.DestroyContext(&ctx_, nullptr);   // the caller drained the GPU
    }

    bool Init(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, Status& status) {
        std::string error;
        HMODULE loader = LoadLoader(error);
        if (!loader) { status = Status::Fail(error); return false; }
        ffxLoadFunctions(&fn_, loader);
        if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Query || !fn_.Configure || !fn_.Dispatch) {
            status = Status::Fail("amd_fidelityfx_loader_dx12.dll sem as funções ffx*");
            return false;
        }

        // Providers: prefer FSR 3.1 interpolation (4.x FG needs RDNA4); otherwise the loader's default.
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        versions.device = device;
        versions.outputCount = &count;
        uint64_t selected = 0;
        if (fn_.Query(nullptr, &versions.header) == FFX_API_RETURN_OK && count > 0 && count <= 64) {
            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            if (fn_.Query(nullptr, &versions.header) == FFX_API_RETURN_OK) {
                for (uint64_t i = 0; i < count; ++i) {
                    const std::string_view n = names[i] ? names[i] : "?";
                    FLUVI_INFO("fg", "provider 0x{:X} {}", ids[i], n);
                    if (!selected && n.find("3.1.") != std::string_view::npos) selected = ids[i];
                }
            }
        }

        ffxOverrideVersion over{};
        over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        over.versionId = selected;
        ffxCreateContextDescFrameGenerationVersion apiVersion{};
        apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        apiVersion.version = FFX_FRAMEGENERATION_VERSION;
        if (selected) apiVersion.header.pNext = &over.header;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        backend.header.pNext = &apiVersion.header;
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        desc.displaySize = desc.maxRenderSize = {w, h};
        desc.backBufferFormat = SurfaceFormat(format);
        const ffxReturnCode_t rc = fn_.CreateContext(&ctx_, &desc.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !ctx_) {
            ctx_ = nullptr;
            status = Status::Fail(std::format("FSR FG: CreateContext falhou ({})", unsigned(rc)));
            return false;
        }
        ffxQueryGetProviderVersion actual{};
        actual.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fn_.Query(&ctx_, &actual.header) == FFX_API_RETURN_OK && actual.versionName) provider_ = actual.versionName;
        else provider_ = "?";

        motion_ = ZeroTexture(device, w, h, DXGI_FORMAT_R16G16_FLOAT);
        depth_ = ZeroTexture(device, w, h, DXGI_FORMAT_R32_FLOAT);
        if (!motion_ || !depth_) { status = Status::Fail("FSR FG: texturas de movimento/profundidade"); return false; }
        width_ = w;
        height_ = h;
        format_ = format;
        FLUVI_INFO("fg", "FSR frame generation {} at {}x{}", provider_, w, h);
        return true;
    }

    bool Generate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output, bool reset,
                  float frameMs) override {
        if (dead_ || !ctx_ || !list || !color || !output || color == output) return false;
        const auto fail = [&](ffxReturnCode_t rc, const char* stage) {
            FLUVI_ERROR("fg", "{} falhou ({}) no quadro {}", stage, unsigned(rc), frameId_);
            dead_ = true;
            return false;
        };
        const FfxApiRect2D rect{0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)};

        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cfg.frameGenerationEnabled = true;
        cfg.frameID = frameId_;
        cfg.generationRect = rect;
        if (ffxReturnCode_t rc = fn_.Configure(&ctx_, &cfg.header); rc != FFX_API_RETURN_OK) return fail(rc, "Configure");

        ffxDispatchDescFrameGenerationPrepareV2 prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.commandList = list;
        prep.frameID = frameId_;
        prep.renderSize = {width_, height_};
        prep.motionVectorScale = {1, 1};
        prep.frameTimeDelta = std::isfinite(frameMs) && frameMs > 0 ? frameMs : 33.333f;
        prep.reset = reset;
        prep.cameraNear = 0.1f;
        prep.cameraFar = 1000.0f;
        prep.cameraFovAngleVertical = 1.0f;
        prep.viewSpaceToMetersFactor = 1.0f;
        prep.cameraUp[1] = prep.cameraRight[0] = prep.cameraForward[2] = 1.0f;   // a still virtual camera
        prep.depth = Res(depth_.Get());
        prep.motionVectors = Res(motion_.Get());
        if (ffxReturnCode_t rc = fn_.Dispatch(&ctx_, &prep.header); rc != FFX_API_RETURN_OK) return fail(rc, "Prepare");

        ffxDispatchDescFrameGeneration d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        d.commandList = list;
        d.frameID = frameId_;
        d.reset = reset;
        d.presentColor = Res(color);
        d.outputs[0] = Res(output);
        d.numGeneratedFrames = 1;
        d.generationRect = rect;
        d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        d.minMaxLuminance[0] = 0;
        d.minMaxLuminance[1] = 1000;
        if (ffxReturnCode_t rc = fn_.Dispatch(&ctx_, &d.header); rc != FFX_API_RETURN_OK) return fail(rc, "Generate");
        ++frameId_;
        return true;
    }

    uint32_t Width() const override { return width_; }
    uint32_t Height() const override { return height_; }
    const std::string& Provider() const override { return provider_; }

private:
    ffxFunctions fn_{};
    ffxContext ctx_ = nullptr;
    ComPtr<ID3D12Resource> motion_, depth_;
    uint32_t width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint64_t frameId_ = 0;
    bool dead_ = false;
    std::string provider_;
};

} // namespace

std::unique_ptr<FrameGenerator> FrameGenerator::Create(ID3D12Device* device, uint32_t width, uint32_t height,
                                                       DXGI_FORMAT format, Status& status) {
    if (!device || !width || !height || SurfaceFormat(format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        status = Status::Fail("FSR FG: parâmetros inválidos");
        return nullptr;
    }
    auto fg = std::make_unique<FsrFg>();
    if (!fg->Init(device, width, height, format, status)) return nullptr;
    status = Status::Ok();
    return fg;
}

} // namespace fluvi::gpu
