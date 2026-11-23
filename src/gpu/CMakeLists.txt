# gpu-core: device, descriptors, textures, command contexts, swapchain, present clock.
# src/gpu/pipeline is a separate module (added by the root CMakeLists).
add_library(fluvi_gpu STATIC
  core/Device.cpp
  present/SwapChain.cpp
  present/Display.cpp
  present/PresentModeEtw.cpp
  present/RefreshOverride.cpp
  present/PresentClock.cpp)
fluvi_defaults(fluvi_gpu)
target_link_libraries(fluvi_gpu PUBLIC fluvi_core d3d12 dxgi dxguid PRIVATE tdh advapi32 user32)

# Manual demo (not a test): tearing/judder check, present mode in the title bar.
add_executable(fluvi_gpu_demo WIN32 present/GpuDemo.cpp)
fluvi_defaults(fluvi_gpu_demo)
target_link_libraries(fluvi_gpu_demo PRIVATE fluvi_gpu)

fluvi_add_test(fluvi_gpu_tests
  SOURCES "${FLUVI_ROOT}/tests/gpu/GpuTests.cpp"
  LIBS fluvi_gpu
  LABELS gpu)
fluvi_add_test(fluvi_present_clock_tests
  SOURCES "${FLUVI_ROOT}/tests/gpu/PresentClockTests.cpp"
  LIBS fluvi_gpu)
target_include_directories(fluvi_present_clock_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/present")
