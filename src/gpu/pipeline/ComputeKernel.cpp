// SPDX-License-Identifier: GPL-3.0-only
#include "ComputeKernel.h"

#include <format>
#include <vector>

namespace fluvi::gpu::detail {

using Microsoft::WRL::ComPtr;

Status ComputeKernel::Create(ID3D12Device* device, const KernelSpec& spec) {
    if (!device || !spec.dxil || !spec.dxilSize) return Status::Fail("ComputeKernel: no device or shader");
    name_ = spec.name;
    constants_ = spec.constants;
    srvs_ = spec.srvs;
    uavs_ = spec.uavs;
    rootUav_ = spec.rootUav;

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(spec.srvs + spec.uavs);
    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(1 + ranges.size() + 1);

    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.ShaderRegister = 0;
    p.Constants.Num32BitValues = spec.constants;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    if (spec.constants) params.push_back(p);

    for (uint32_t i = 0; i < ranges.size(); ++i) {
        const bool uav = i >= spec.srvs;
        ranges[i].RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = uav ? i - spec.srvs : i;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER t{};
        t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        t.DescriptorTable.NumDescriptorRanges = 1;
        t.DescriptorTable.pDescriptorRanges = &ranges[i];
        t.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(t);
    }
    if (spec.rootUav) {
        D3D12_ROOT_PARAMETER u{};
        u.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        u.Descriptor.ShaderRegister = spec.uavs;
        u.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(u);
    }

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < 2; ++i) {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = static_cast<UINT>(params.size());
    rs.pParameters = params.data();
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
    if (FAILED(hr))
        return Status::Fail(std::format("{}: serialize root signature failed 0x{:08X} {}", spec.name, static_cast<uint32_t>(hr),
                                        error ? static_cast<const char*>(error->GetBufferPointer()) : ""));
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (FAILED(hr)) return Status::Fail(std::format("{}: CreateRootSignature failed 0x{:08X}", spec.name, static_cast<uint32_t>(hr)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_.Get();
    pso.CS = {spec.dxil, spec.dxilSize};
    hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) {
        root_.Reset();
        return Status::Fail(std::format("{}: CreateComputePipelineState failed 0x{:08X}", spec.name, static_cast<uint32_t>(hr)));
    }
    return Status::Ok();
}

void ComputeKernel::Bind(ID3D12GraphicsCommandList* list, const void* constants, const D3D12_GPU_DESCRIPTOR_HANDLE* srv,
                         const D3D12_GPU_DESCRIPTOR_HANDLE* uav, D3D12_GPU_VIRTUAL_ADDRESS rootUav) const {
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    UINT index = 0;
    if (constants_) list->SetComputeRoot32BitConstants(index++, constants_, constants, 0);
    for (uint32_t i = 0; i < srvs_; ++i) list->SetComputeRootDescriptorTable(index++, srv[i]);
    for (uint32_t i = 0; i < uavs_; ++i) list->SetComputeRootDescriptorTable(index++, uav[i]);
    if (rootUav_) list->SetComputeRootUnorderedAccessView(index++, rootUav);
}

} // namespace fluvi::gpu::detail
