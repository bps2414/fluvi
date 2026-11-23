@echo off
rem Runs a command inside the MSVC x64 environment, with the CMake and Ninja bundled with Visual Studio on PATH.
rem   scripts\dev.cmd cmake --preset debug
rem   scripts\dev.cmd cmake --build build\debug
rem Machine-specific settings (FLUVI_VS, VCPKG_ROOT, vcpkg cache variables) go in scripts\dev.local.cmd (not tracked).
if exist "%~dp0dev.local.cmd" call "%~dp0dev.local.cmd"
if not defined FLUVI_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FLUVI_VS=%%i"
if not defined FLUVI_VS (echo Visual Studio with the C++ tools was not found. Set FLUVI_VS in scripts\dev.local.cmd. & exit /b 1)
call "%FLUVI_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined VCPKG_ROOT set "VCPKG_ROOT=%FLUVI_VS%\VC\vcpkg"
set "PATH=%FLUVI_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%FLUVI_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
%*
