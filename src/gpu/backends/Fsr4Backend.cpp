// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline: FSR 4.1.2 upscaler backend. AMD FidelityFX SDK 2.4 (MIT), the signed binaries are loaded at run
// time through amd_fidelityfx_loader_dx12.dll (which loads amd_fidelityfx_upscaler_dx12.dll); nothing links to AMD
// code. Contract: include/fluvi/gpu/UpscalerBackend.h. Owner: agent G.
//
// Video is not a game: there is no camera jitter and no motion vectors. The backend feeds FSR 4 a constant depth, an
// all-zero motion vector texture and zero jitter. Temporal accumulation with zero motion would smear anything that
// moves, so the chain resets the history every frame by default (UpscaleFrame::reset): the result is a clean
// single-frame ML upscale, with none of the temporal gain FSR 4 has in games. Real motion (from the optical flow of
// wave 3) could be fed in later.
#include "fluvi/gpu/UpscalerBackend.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

#include <windows.h>
#include <wrl/client.h>

#include "fluvi/core/Log.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

namespace fluvi::gpu {
namespace {

using Microsoft::WRL::ComPtr;

// ---- the FidelityFX entry points (loaded once, kept for the life of the process) --------------------------------------
struct FfxApi {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxConfigure configure = nullptr;
    std::string loadedFrom;
    std::string error;   // why loading failed
};

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf).parent_path() : std::filesystem::path();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const FfxApi& Api() {
    static std::once_flag once;
    static FfxApi api;
    std::call_once(once, [] {
        std::vector<std::filesystem::path> dirs;
        dirs.push_back(ExeDir());
        wchar_t env[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"FLUVI_FFX_DIR", env, static_cast<DWORD>(std::size(env))) > 0) dirs.emplace_back(env);
#ifdef FLUVI_FFX_SIGNEDBIN_DIR
        dirs.emplace_back(FLUVI_FFX_SIGNEDBIN_DIR);
#endif
        for (const auto& dir : dirs) {
            const auto loader = dir / L"amd_fidelityfx_loader_dx12.dll";
            std::error_code ec;
            if (!std::filesystem::exists(loader, ec)) continue;
            // The loader resolves the effect DLL next to the executable or next to itself.
            api.module = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!api.module) {
                api.error = std::format("LoadLibrary({}) falhou ({})", loader.string(), GetLastError());
                continue;
            }
            api.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(api.module, "ffxCreateContext"));
            api.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(api.module, "ffxDestroyContext"));
            api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(api.module, "ffxQuery"));
            api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(api.module, "ffxDispatch"));
            api.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(api.module, "ffxConfigure"));
            if (api.create && api.destroy && api.query && api.dispatch) {
                api.loadedFrom = loader.string();
                api.error.clear();
                return;
            }
            api.error = std::format("{}: faltam funções ffx*", loader.string());
            FreeLibrary(api.module);
            api.module = nullptr;
        }
        if (api.error.empty()) api.error = "amd_fidelityfx_loader_dx12.dll não encontrada (exe dir, FLUVI_FFX_DIR, signedbin)";
    });
    return api;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
    FLUVI_LOG(type == FFX_API_MESSAGE_TYPE_ERROR ? log::Level::Error : log::Level::Warn, "fsr4", "{}", Narrow(message ? message : L""));
}

struct VersionInfo {
    uint64_t id = 0;
    std::string name;
};

// Every upscaler provider the DLL (or the driver) offers for this device. Names are copied: the API may overwrite them.
std::vector<VersionInfo> QueryVersions(const FfxApi& api, ID3D12Device* device) {
    std::vector<VersionInfo> out;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    uint64_t count = 0;
    q.outputCount = &count;
    if (api.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return out;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count, nullptr);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (api.query(nullptr, &q.header) != FFX_API_RETURN_OK) return out;
    for (uint64_t i = 0; i < count; ++i) out.push_back({ids[i], names[i] ? names[i] : ""});
    return out;
}

// FSR 4.1.2 if it is there; else the highest "FSR 4.x". Never an FSR 3/2 provider (those are not ML upscalers and
// are what the default selection falls back to on a GPU FSR 4 does not support).
const VersionInfo* PickFsr4(const std::vector<VersionInfo>& versions) {
    const VersionInfo* best = nullptr;
    for (const auto& v : versions)
        if (v.name.find("4.1.2") != std::string::npos && (!best || v.name.find('*') == std::string::npos)) best = &v;
    if (best) return best;
    for (const auto& v : versions)
        if (v.name.find("4.") != std::string::npos && v.name.find("FSR") != std::string::npos && (!best || v.name > best->name)) best = &v;
    return best;
}

D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = t;
    return h;
}

// ---- the backend ---------------------------------------------------------------------------------------------------------
class Fsr4Backend final : public UpscalerBackend {
public:
    ~Fsr4Backend() override {
        if (context_) Api().destroy(&context_, nullptr);
    }
    const char* Name() const override { return name_.c_str(); }

    Status Init(ID3D12Device* device, const UpscaleSizes& sizes) {
        const FfxApi& api = Api();
        if (!api.module) return Status::Fail(api.error);
        if (sizes.renderW == 0 || sizes.renderH == 0 || sizes.outputW < sizes.renderW || sizes.outputH < sizes.renderH)
            return Status::Fail("FSR 4: tamanhos inválidos (a saída precisa ser maior que a entrada)");
        if (double(sizes.outputW) > kFsr4MaxScale * sizes.renderW + 0.5 || double(sizes.outputH) > kFsr4MaxScale * sizes.renderH + 0.5)
            return Status::Fail(std::format("FSR 4: ampliação {}x{} -> {}x{} passa de {}x por dimensão", sizes.renderW, sizes.renderH,
                                            sizes.outputW, sizes.outputH, kFsr4MaxScale));
        sizes_ = sizes;

        const auto versions = QueryVersions(api, device);
        std::string list;
        for (const auto& v : versions) list += std::format("{}[0x{:X}]", list.empty() ? "" : ", ", v.id) + " " + v.name;
        FLUVI_INFO("fsr4", "provedores de upscale: {}", list.empty() ? "(nenhum)" : list);
        const VersionInfo* pick = PickFsr4(versions);
        if (!pick) return Status::Fail("FSR 4: nenhum provedor FSR 4.x para esta GPU (não suportada pelo DLL)");

        // Pointers in the descriptor chain must outlive the context: keep them as members.
        backend_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend_.device = device;
        override_.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override_.versionId = pick->id;
        version_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version_.version = FFX_UPSCALER_VERSION;
        create_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create_.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        create_.maxRenderSize = {sizes.renderW, sizes.renderH};
        create_.maxUpscaleSize = {sizes.outputW, sizes.outputH};
        create_.fpMessage = FfxMessage;
        create_.header.pNext = &version_.header;
        version_.header.pNext = &override_.header;
        override_.header.pNext = &backend_.header;
        backend_.header.pNext = nullptr;

        const ffxReturnCode_t rc = api.create(&context_, &create_.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !context_) {
            context_ = nullptr;
            return Status::Fail(std::format("FSR 4: ffxCreateContext falhou ({}) com {}", rc, pick->name));
        }
        name_ = pick->name;
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (api.query(&context_, &pv.header) == FFX_API_RETURN_OK && pv.versionName) name_ = pv.versionName;
        FLUVI_INFO("fsr4", "contexto criado: {} ({}x{} -> {}x{}), DLL {}", name_, sizes.renderW, sizes.renderH, sizes.outputW,
                   sizes.outputH, api.loadedFrom);

        return CreateConstants(device);
    }

    bool Record(const UpscaleFrame& f) override {
        if (!context_ || !f.list || !f.color || !f.output) return false;
        if (!constantsReady_) FillConstants(f.list);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = f.list;
        d.color = ffxApiGetResourceDX12(f.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(depth_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {0.f, 0.f};
        d.motionVectorScale = {1.f, 1.f};
        d.renderSize = {sizes_.renderW, sizes_.renderH};
        d.upscaleSize = {sizes_.outputW, sizes_.outputH};
        d.enableSharpening = f.sharpness > 0.f;
        d.sharpness = f.sharpness;
        d.frameTimeDelta = f.frameTimeMs;
        d.preExposure = 1.f;
        d.reset = f.reset;
        d.cameraNear = 0.1f;
        d.cameraFar = 1000.f;
        d.cameraFovAngleVertical = 1.0f;
        d.viewSpaceToMetersFactor = 1.f;
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const ffxReturnCode_t rc = Api().dispatch(&context_, &d.header);
        if (rc != FFX_API_RETURN_OK) {
            FLUVI_ERROR("fsr4", "ffxDispatch falhou ({})", rc);
            return false;
        }
        return true;
    }

private:
    // Constant depth (1.0) and zero motion at render size, uploaded once. Plain committed textures: FidelityFX makes
    // its own descriptors from the resources.
    Status CreateConstants(ID3D12Device* device) {
        struct Spec { DXGI_FORMAT format; ComPtr<ID3D12Resource>* tex; float fill; };
        const Spec specs[2] = {{DXGI_FORMAT_R32_FLOAT, &depth_, 1.f}, {DXGI_FORMAT_R16G16_FLOAT, &motion_, 0.f}};
        const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
        for (int i = 0; i < 2; ++i) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = sizes_.renderW;
            rd.Height = sizes_.renderH;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = specs[i].format;
            rd.SampleDesc.Count = 1;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(specs[i].tex->GetAddressOf()))))
                return Status::Fail("FSR 4: sem memória para a textura de profundidade/movimento");
            device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint_[i], &rows_[i], &rowSize_[i], &total_[i]);
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total_[i];
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const auto up = Heap(D3D12_HEAP_TYPE_UPLOAD);
            if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(upload_[i].GetAddressOf()))))
                return Status::Fail("FSR 4: sem memória para o buffer de upload");
            uint8_t* p = nullptr;
            upload_[i]->Map(0, nullptr, reinterpret_cast<void**>(&p));
            std::memset(p, 0, static_cast<size_t>(total_[i]));
            if (specs[i].fill != 0.f) {
                for (UINT y = 0; y < rows_[i]; ++y) {
                    auto* row = reinterpret_cast<float*>(p + footprint_[i].Offset + size_t(y) * footprint_[i].Footprint.RowPitch);
                    std::fill(row, row + sizes_.renderW, specs[i].fill);
                }
            }
            upload_[i]->Unmap(0, nullptr);
        }
        return Status::Ok();
    }

    void FillConstants(ID3D12GraphicsCommandList* list) {
        ID3D12Resource* tex[2] = {depth_.Get(), motion_.Get()};
        for (int i = 0; i < 2; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = tex[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload_[i].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprint_[i];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = tex[i];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &b);
        }
        constantsReady_ = true;   // the upload buffers stay alive with the backend (the GPU may still be reading them)
    }

    UpscaleSizes sizes_;
    std::string name_ = "FSR 4";
    ffxContext context_ = nullptr;
    ffxCreateBackendDX12Desc backend_{};
    ffxOverrideVersion override_{};
    ffxCreateContextDescUpscaleVersion version_{};
    ffxCreateContextDescUpscale create_{};
    ComPtr<ID3D12Resource> depth_, motion_, upload_[2];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_[2]{};
    UINT rows_[2]{};
    UINT64 rowSize_[2]{}, total_[2]{};
    bool constantsReady_ = false;
};

} // namespace

std::unique_ptr<UpscalerBackend> CreateFsr4Backend(ID3D12Device* device, const UpscaleSizes& sizes, Status& why) {
    if (!device) {
        why = Status::Fail("FSR 4: sem dispositivo D3D12");
        return nullptr;
    }
    auto backend = std::make_unique<Fsr4Backend>();
    why = backend->Init(device, sizes);
    if (!why.ok) return nullptr;
    return backend;
}

} // namespace fluvi::gpu
