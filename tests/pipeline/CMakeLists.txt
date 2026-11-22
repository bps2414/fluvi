# Owned by agent E. Included from src/gpu/pipeline/CMakeLists.txt.
# Test utilities: a self-contained D3D12 device + procedural images (no dependency on fluvi_gpu).
add_library(fluvi_pipeline_testutil STATIC TestGpu.cpp Images.cpp)
fluvi_defaults(fluvi_pipeline_testutil)
target_include_directories(fluvi_pipeline_testutil PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fluvi_pipeline_testutil PUBLIC fluvi_pipeline d3d12 dxgi dxguid)

fluvi_add_test(fluvi_pipeline_tests SOURCES PipelineTests.cpp ChainTests.cpp LIBS fluvi_pipeline_testutil LABELS gpu)

# Manual tool (not a test): PNG in, per-stage PNGs and GPU timings out.
add_executable(fluvi_pipeline_bench bench/PipelineBench.cpp)
fluvi_defaults(fluvi_pipeline_bench)
target_link_libraries(fluvi_pipeline_bench PRIVATE fluvi_pipeline_testutil windowscodecs ole32 uuid)
