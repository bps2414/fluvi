include_guard(GLOBAL)

# Compiler settings every Fluvi target uses.
function(fluvi_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP)
endfunction()

# fluvi_add_test(<name> SOURCES a.cpp b.cpp LIBS lib1 lib2 [LABELS gpu])
# doctest executable registered with ctest. Tests that need real hardware get the label "gpu" / "device".
function(fluvi_add_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS;LABELS" ${ARGN})
  add_executable(${name} ${T_SOURCES} "${FLUVI_ROOT}/tests/doctest_main.cpp")
  fluvi_defaults(${name})
  target_link_libraries(${name} PRIVATE doctest::doctest fluvi_core ${T_LIBS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${FLUVI_ROOT}")
  if(T_LABELS)
    set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
  endif()
endfunction()

# DXC from the Windows SDK, for build-time shader compilation.
find_program(FLUVI_DXC dxc
  HINTS "$ENV{WindowsSdkVerBinPath}/x64" "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_SYSTEM_VERSION}/x64"
  DOC "DirectX Shader Compiler")
