# Owned by agent E (shaders: src/gpu/pipeline + shaders/ + tests/pipeline). Target fluvi_pipeline: compute passes with
# HLSL compiled at build time (dxc, cs_6_0) into generated headers embedded in the library. Depends on
# fluvi_core only (raw D3D12), not on fluvi_gpu.
if(NOT FLUVI_DXC)
  message(FATAL_ERROR "dxc not found (Windows SDK 10.0.26100 bin\\x64). Set FLUVI_DXC.")
endif()

set(FLUVI_SHADER_DIR "${FLUVI_ROOT}/shaders")
set(FLUVI_FFX_GPU_DIR "${FLUVI_ROOT}/third_party/fidelityfx/sdk/Kits/FidelityFX")
set(FLUVI_FSR1_DIR "${FLUVI_FFX_GPU_DIR}/upscalers/fsr3/include/gpu/fsr1")
set(FLUVI_FFX_CORE_DIR "${FLUVI_FFX_GPU_DIR}/api/internal/gpu")
set(FLUVI_SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FLUVI_SHADER_GEN_DIR}")

file(GLOB FLUVI_FFX_HEADERS "${FLUVI_FSR1_DIR}/*.h" "${FLUVI_FFX_CORE_DIR}/*.h")

set(FLUVI_PIPELINE_SHADERS
  Nv12ToRgb Deblock Downscale FsrEasu FsrRcas
  FlowAdapt FlowRefine FlowClaim FlowFill VfgShotChange HudLockUpdate HudLockCompose)

set(FLUVI_PIPELINE_GENERATED "")
foreach(shader IN LISTS FLUVI_PIPELINE_SHADERS)
  set(out "${FLUVI_SHADER_GEN_DIR}/${shader}.dxil.h")
  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${FLUVI_DXC}" -nologo -T cs_6_0 -E main -O3 -WX
            -I "${FLUVI_SHADER_DIR}" -I "${FLUVI_FFX_CORE_DIR}" -I "${FLUVI_FSR1_DIR}"
            -Fh "${out}" -Vn g_${shader} "${FLUVI_SHADER_DIR}/${shader}.hlsl"
    DEPENDS "${FLUVI_SHADER_DIR}/${shader}.hlsl" ${FLUVI_FFX_HEADERS}
    COMMENT "dxc ${shader}.hlsl"
    VERBATIM)
  list(APPEND FLUVI_PIPELINE_GENERATED "${out}")
endforeach()

# Vendor upscaler backends (agent G): FSR 4.1.2 through the FidelityFX loader DLL, loaded at run time.
set(FLUVI_FFX_SIGNEDBIN "${FLUVI_ROOT}/third_party/fidelityfx/sdk/Kits/FidelityFX/signedbin")
set(FLUVI_FFX_API_INCLUDE "${FLUVI_ROOT}/third_party/fidelityfx/sdk/Kits/FidelityFX/api/include"
                          "${FLUVI_ROOT}/third_party/fidelityfx/sdk/Kits/FidelityFX/upscalers/include"
                          "${FLUVI_ROOT}/third_party/fidelityfx/sdk/Kits/FidelityFX/framegeneration/include")

add_library(fluvi_pipeline STATIC
  ComputeKernel.cpp ImagePasses.cpp FlowPasses.cpp QualityChain.cpp
  "${FLUVI_ROOT}/src/gpu/backends/Fsr4Backend.cpp" "${FLUVI_ROOT}/src/gpu/backends/FsrFgBackend.cpp"
  ${FLUVI_PIPELINE_GENERATED})
fluvi_defaults(fluvi_pipeline)
target_include_directories(fluvi_pipeline PRIVATE "${FLUVI_SHADER_GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(fluvi_pipeline SYSTEM PRIVATE ${FLUVI_FFX_API_INCLUDE})
# Dev fallback for the loader DLL search (the app ships the DLLs next to fluvi.exe).
target_compile_definitions(fluvi_pipeline PRIVATE "FLUVI_FFX_SIGNEDBIN_DIR=\"${FLUVI_FFX_SIGNEDBIN}\"")
target_link_libraries(fluvi_pipeline PUBLIC fluvi_core d3d12 dxgi)

add_subdirectory("${FLUVI_ROOT}/tests/pipeline" "${CMAKE_BINARY_DIR}/tests/pipeline")
