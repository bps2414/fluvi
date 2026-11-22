# fluvi.exe (agent H, wave 2): window thread, render loop, sign-in / consoles / connecting, saved settings, pad navigation.
#   fluvi_app_core   the logic without a window or a GPU (settings JSON, pad navigation, model mapping, stream
#                    controller, app controller): what the tests link
#   fluvi            the exe (window + render loop + fluvi.rc with the icon)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(fluvi_app_core STATIC
  SettingsStore.cpp
  PadNav.cpp
  Models.cpp
  StreamController.cpp
  App.cpp)
fluvi_defaults(fluvi_app_core)
target_include_directories(fluvi_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fluvi_app_core
  PUBLIC fluvi_core fluvi_xhome fluvi_input fluvi_ui
  PRIVATE nlohmann_json::nlohmann_json shell32 ole32)

add_executable(fluvi WIN32 main.cpp Window.cpp fluvi.rc)
fluvi_defaults(fluvi)
target_link_libraries(fluvi PRIVATE fluvi_app_core fluvi_gpu fluvi_pipeline user32)

fluvi_add_test(fluvi_app_tests
  SOURCES "${FLUVI_ROOT}/tests/app/AppTests.cpp"
  LIBS fluvi_app_core
  LABELS app)
target_compile_definitions(fluvi_app_tests PRIVATE FLUVI_APP_TEST_TMP="${CMAKE_BINARY_DIR}/app-test-tmp")
