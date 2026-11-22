# fluvi.exe. Owned by the main session; wave 2 wires the modules in.
add_executable(fluvi WIN32 main.cpp)
fluvi_defaults(fluvi)
target_link_libraries(fluvi PRIVATE fluvi_core)
