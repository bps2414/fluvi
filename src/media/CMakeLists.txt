# Agent F (media): H.264 Annex-B -> FFmpeg d3d12va -> NV12 gpu-core textures, repeat marking, keyframe recovery.
#   fluvi_media             static library (links FFmpeg avcodec/avutil, shared)
#   fluvi_media_probe       decodes an Annex-B file, prints per-frame timing / isNew (no window)
#   fluvi_media_tests       CPU: H.264 parsing on the committed clips
#   fluvi_media_gpu_tests   GPU (label gpu): decode, NV12 textures, repeats, recovery, decode ms
if(NOT FLUVI_DXC)
  message(FATAL_ERROR "dxc not found (Windows SDK bin\\x64). Set FLUVI_DXC.")
endif()

# ---- FFmpeg (dev: the shared build in Tools; delivery: a trimmed h264/hevc build, same layout) -------------
set(FLUVI_FFMPEG_DIR "D:/Bryan/Dev/Tools/ffmpeg-shared" CACHE PATH
    "FFmpeg shared build (include/ lib/ bin/), or a folder holding one such build")
set(_ffmpeg_roots "${FLUVI_FFMPEG_DIR}")
file(GLOB _ffmpeg_sub LIST_DIRECTORIES true "${FLUVI_FFMPEG_DIR}/*")
list(APPEND _ffmpeg_roots ${_ffmpeg_sub})
set(FLUVI_FFMPEG_ROOT "")
foreach(root IN LISTS _ffmpeg_roots)
  if(EXISTS "${root}/include/libavutil/hwcontext_d3d12va.h" AND EXISTS "${root}/lib/avcodec.lib")
    set(FLUVI_FFMPEG_ROOT "${root}")
    break()
  endif()
endforeach()
if(NOT FLUVI_FFMPEG_ROOT)
  message(FATAL_ERROR "FFmpeg with d3d12va not found under FLUVI_FFMPEG_DIR=${FLUVI_FFMPEG_DIR}")
endif()
message(STATUS "media: FFmpeg at ${FLUVI_FFMPEG_ROOT}")

set(FLUVI_FFMPEG_DLLS "")
foreach(lib avcodec avutil swresample)
  file(GLOB _dll "${FLUVI_FFMPEG_ROOT}/bin/${lib}-*.dll")
  if(NOT _dll)
    message(FATAL_ERROR "FFmpeg: ${lib}-*.dll not found in ${FLUVI_FFMPEG_ROOT}/bin")
  endif()
  list(GET _dll 0 _dll)
  list(APPEND FLUVI_FFMPEG_DLLS "${_dll}")
  if(NOT lib STREQUAL "swresample")
    add_library(FFmpeg::${lib} SHARED IMPORTED)
    set_target_properties(FFmpeg::${lib} PROPERTIES
      IMPORTED_IMPLIB "${FLUVI_FFMPEG_ROOT}/lib/${lib}.lib"
      IMPORTED_LOCATION "${_dll}"
      INTERFACE_INCLUDE_DIRECTORIES "${FLUVI_FFMPEG_ROOT}/include")
  endif()
endforeach()

# Every executable lands in ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}: one copy of the DLLs serves them all.
add_custom_target(fluvi_ffmpeg_dlls
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${FLUVI_FFMPEG_DLLS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  COMMENT "copy FFmpeg DLLs next to the executables")

# ---- shader -------------------------------------------------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_gen}")
add_custom_command(
  OUTPUT "${_gen}/PictureCompare.dxil.h"
  COMMAND "${FLUVI_DXC}" -nologo -T cs_6_0 -E main -O3 -WX
          -Fh "${_gen}/PictureCompare.dxil.h" -Vn g_PictureCompare "${CMAKE_CURRENT_SOURCE_DIR}/PictureCompare.hlsl"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/PictureCompare.hlsl"
  COMMENT "dxc PictureCompare.hlsl"
  VERBATIM)

# ---- library ------------------------------------------------------------------------------------------------
add_library(fluvi_media STATIC
  VideoDecoder.cpp
  H264Parse.cpp
  "${_gen}/PictureCompare.dxil.h")
fluvi_defaults(fluvi_media)
target_include_directories(fluvi_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_gen}")
target_include_directories(fluvi_media SYSTEM PRIVATE "${FLUVI_FFMPEG_ROOT}/include")
target_link_libraries(fluvi_media PUBLIC fluvi_core fluvi_gpu PRIVATE FFmpeg::avcodec FFmpeg::avutil d3d12 dxguid)
add_dependencies(fluvi_media fluvi_ffmpeg_dlls)

# ---- probe tool (console, no window) ------------------------------------------------------------------------
add_executable(fluvi_media_probe probe/main.cpp)
fluvi_defaults(fluvi_media_probe)
target_include_directories(fluvi_media_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fluvi_media_probe PRIVATE fluvi_media)

# ---- tests --------------------------------------------------------------------------------------------------
fluvi_add_test(fluvi_media_tests
  SOURCES "${FLUVI_ROOT}/tests/media/ParseTests.cpp" "${FLUVI_ROOT}/tests/media/Clip.cpp"
  LIBS fluvi_media)
target_include_directories(fluvi_media_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUVI_ROOT}/tests/media")

fluvi_add_test(fluvi_media_gpu_tests
  SOURCES "${FLUVI_ROOT}/tests/media/DecoderTests.cpp" "${FLUVI_ROOT}/tests/media/Clip.cpp"
  LIBS fluvi_media
  LABELS gpu)
target_include_directories(fluvi_media_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUVI_ROOT}/tests/media")
