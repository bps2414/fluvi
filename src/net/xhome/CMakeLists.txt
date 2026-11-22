# Agent B (xhome): sign-in, session API, WebRTC session, input protocol. No GPU code.
#   fluvi_xhome      static library
#   fluvi_xhome_cli  sign-in / consoles / wake / probe tool for hand testing
#   fluvi_xhome_tests (doctest)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(fluvi_xhome STATIC
  Https.cpp
  Account.cpp
  StreamApi.cpp
  Protocol.cpp
  Sdp.cpp
  WebRtcSession.cpp)
fluvi_defaults(fluvi_xhome)
target_link_libraries(fluvi_xhome
  PUBLIC fluvi_core
  PRIVATE LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json winhttp crypt32 shell32 ole32 ws2_32)

add_executable(fluvi_xhome_cli cli/main.cpp)
fluvi_defaults(fluvi_xhome_cli)
target_link_libraries(fluvi_xhome_cli PRIVATE fluvi_xhome nlohmann_json::nlohmann_json)

# Offline tests; the WebRTC one runs two local peers on loopback. Nothing here needs the internet or an account.
fluvi_add_test(fluvi_xhome_tests
  SOURCES "${FLUVI_ROOT}/tests/xhome/XhomeTests.cpp"
  LIBS fluvi_xhome LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json)
