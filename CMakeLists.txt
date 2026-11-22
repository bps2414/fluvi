cmake_minimum_required(VERSION 3.28)
project(fluvi VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Fluvi is Windows-only (D3D12, WASAPI, GameInput).")
endif()

set(FLUVI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${FLUVI_ROOT}/cmake")
include(Fluvi)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Which modules to configure. "all", or a ;-list of: gpu pipeline xhome input audio ui app.
# Wave agents build only their own module so a half-done neighbour never breaks their configure:
#   -DFLUVI_MODULES=gpu
set(FLUVI_MODULES "all" CACHE STRING "Modules to configure")

function(fluvi_module name dir)
  if(FLUVI_MODULES STREQUAL "all" OR name IN_LIST FLUVI_MODULES)
    add_subdirectory(${dir})
  endif()
endfunction()

enable_testing()
find_package(doctest CONFIG REQUIRED)

# Core: logging, clock. Owned by the main session.
add_library(fluvi_core STATIC src/core/Log.cpp src/core/Time.cpp)
fluvi_defaults(fluvi_core)
target_include_directories(fluvi_core PUBLIC "${FLUVI_ROOT}/include")

fluvi_module(gpu      src/gpu)           # agent A: gpu-core + present  -> fluvi_gpu
fluvi_module(pipeline src/gpu/pipeline)  # agent E: passes + shaders    -> fluvi_pipeline
fluvi_module(xhome    src/net/xhome)     # agent B: sign-in, session    -> fluvi_xhome
fluvi_module(input    src/input)         # agent D                      -> fluvi_input
fluvi_module(audio    src/audio)         # agent D                      -> fluvi_audio
fluvi_module(ui       src/ui)            # agent C: RmlUi               -> fluvi_ui
fluvi_module(app      src/app)           # main session: fluvi.exe

fluvi_add_test(fluvi_core_tests SOURCES tests/core/CoreTests.cpp)
