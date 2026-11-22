@echo off
rem Runs a command inside the MSVC x64 environment, with the bundled CMake/Ninja on PATH.
rem   scripts\dev.cmd cmake --preset debug
rem   scripts\dev.cmd cmake --build build\debug
rem   scripts\dev.cmd cmake -S . -B build\agent-a -G Ninja ... (see CMakePresets.json for the cache vars)
call "D:\Bryan\Dev\Tools\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=D:\Bryan\Dev\Tools\VSBuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;D:\Bryan\Dev\Tools\VSBuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "VCPKG_DOWNLOADS=D:\Bryan\Dev\Caches\vcpkg\downloads"
set "VCPKG_DEFAULT_BINARY_CACHE=D:\Bryan\Dev\Caches\vcpkg\archives"
set "X_VCPKG_REGISTRIES_CACHE=D:\Bryan\Dev\Caches\vcpkg\registries"
%*
