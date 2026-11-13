@echo off
setlocal
cd /d "%~dp0"

rem Run from a "x64 Native Tools Command Prompt for VS" (or any shell where cmake and cl are on PATH).
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\dev\vcpkg"

if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo [Highrim] vcpkg not found in: %VCPKG_ROOT%
    echo Set VCPKG_ROOT or install vcpkg to C:\dev\vcpkg.
    exit /b 1
)

git submodule update --init --recursive
if errorlevel 1 exit /b %errorlevel%

cmake -S . -B out\build-release -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
if errorlevel 1 exit /b %errorlevel%

cmake --build out\build-release --config Release --target Highrim
if errorlevel 1 exit /b %errorlevel%

echo [Highrim] Built package\Data\SKSE\Plugins\Highrim.dll
