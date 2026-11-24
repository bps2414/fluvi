# ui: RmlUi 6.3 drawn inside our D3D12 frame -> fluvi_ui, fluvi_ui_demo, tests.
find_package(RmlUi CONFIG REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)

# Vendored RmlUi DX12 backend (MIT) + D3D12MA + offsetAllocator, compiled through wrapper TUs that silence warnings,
# so /W4 stays clean for our own code.
add_library(fluvi_ui STATIC
  UiImpl.cpp
  UiModel.cpp
  UiInput.cpp
  UiSettings.cpp
  UiStats.cpp
  UiSystem.cpp
  Qr.cpp
  rml_dx12/RmlUi_Platform_Win32_Fluvi.cpp
  vendor/RendererDx12.cpp
  vendor/D3D12MemAlloc.cpp
  vendor/OffsetAllocator.cpp)
fluvi_defaults(fluvi_ui)
target_include_directories(fluvi_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/rml_dx12")
target_compile_definitions(fluvi_ui PRIVATE
  RMLUI_DISABLE_INCLUDE_WINDOWS
  RMLUI_RENDER_BACKEND_FIELD_SWAPCHAIN_BACKBUFFER_COUNT=4
  RMLUI_RENDER_BACKEND_FIELD_DESCRIPTORAMOUNT_FOR_SRV_CBV_UAV=1024
  RMLUI_RENDER_BACKEND_FIELD_DESCRIPTOR_HEAP_RTV=32
  FLUVI_VERSION="${PROJECT_VERSION}")
target_link_libraries(fluvi_ui
  PUBLIC fluvi_core d3d12
  PRIVATE RmlUi::RmlUi unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator dxgi dxguid d3dcompiler user32 shell32)

# Demo: the 6 screens with fake data. Keys 1-6 switch screens; `--shots <dir>` renders every screen and state
# offscreen to PNG (no window) and exits.
if(TARGET fluvi_gpu)
  add_executable(fluvi_ui_demo demo/main.cpp)
  fluvi_defaults(fluvi_ui_demo)
  target_link_libraries(fluvi_ui_demo PRIVATE fluvi_ui fluvi_gpu windowscodecs ole32)
endif()

fluvi_add_test(fluvi_ui_tests
  SOURCES "${FLUVI_ROOT}/tests/ui/UiTests.cpp" "${FLUVI_ROOT}/tests/ui/UiLogicTests.cpp" "${FLUVI_ROOT}/tests/ui/UiTextTests.cpp"
  LIBS fluvi_ui)
target_include_directories(fluvi_ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET fluvi_gpu)
  fluvi_add_test(fluvi_ui_gpu_tests
    SOURCES "${FLUVI_ROOT}/tests/ui/UiGpuTests.cpp"
    LIBS fluvi_ui fluvi_gpu
    LABELS gpu)
endif()
