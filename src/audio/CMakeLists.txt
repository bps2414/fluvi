# Target: fluvi_audio (static). Pure parts (JitterBuffer, DriftController, DriftResampler,
# PlayoutEngine) are separate from WasapiRenderer so tests run with simulated clocks and no device.
find_package(Opus CONFIG REQUIRED)

add_library(fluvi_audio STATIC
  OpusPlayer.cpp
  PlayoutEngine.cpp
  JitterBuffer.cpp
  DriftController.cpp
  DriftResampler.cpp
  WasapiRenderer.cpp)
fluvi_defaults(fluvi_audio)
target_include_directories(fluvi_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fluvi_audio PUBLIC fluvi_core PRIVATE Opus::opus avrt ole32 mmdevapi)

# Pure tests: no audio device needed.
fluvi_add_test(fluvi_audio_tests SOURCES "${FLUVI_ROOT}/tests/audio/AudioTests.cpp" LIBS fluvi_audio Opus::opus)
target_include_directories(fluvi_audio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Opens the default render endpoint and plays ~0.5 s at volume 0.
fluvi_add_test(fluvi_audio_device_tests SOURCES "${FLUVI_ROOT}/tests/audio/AudioDeviceTests.cpp" LIBS fluvi_audio Opus::opus
  LABELS device)
