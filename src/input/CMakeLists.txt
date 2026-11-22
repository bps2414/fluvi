# Owned by agent D (input). Targets: fluvi_input (static) and fluvi_pad_probe (manual tool).
# GameInput.dll and xinput1_4.dll are loaded at run time (no import libraries): GameInput is inbox on Windows 11;
# on older systems Start() fails and the factory falls back to XInput. Only avrt (MMCSS) is linked.
add_library(fluvi_input STATIC
  GamepadReader.cpp
  GamepadMapping.cpp
  GameInputReader.cpp
  XInputReader.cpp
  RumbleEngine.cpp)
fluvi_defaults(fluvi_input)
target_include_directories(fluvi_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fluvi_input PUBLIC fluvi_core PRIVATE avrt)

add_executable(fluvi_pad_probe pad_probe.cpp)
fluvi_defaults(fluvi_pad_probe)
target_link_libraries(fluvi_pad_probe PRIVATE fluvi_input fluvi_core)

fluvi_add_test(fluvi_input_tests SOURCES "${FLUVI_ROOT}/tests/input/InputTests.cpp" LIBS fluvi_input)
target_include_directories(fluvi_input_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
