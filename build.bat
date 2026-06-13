@echo off
setlocal
cd /d "%~dp0"

:: Find and set mt.exe (manifest tool) so CMake generates correct manifests.
set "CMAKE_MT="
for /r "C:\Program Files (x86)\Windows Kits\" %%f in (mt.exe) do set "CMAKE_MT=%%f" & goto :found_mt
:found_mt
if defined CMAKE_MT (
    echo Found mt.exe: %CMAKE_MT%
    set "CMAKE_MT_ARG=-DCMAKE_MT:FILEPATH=%CMAKE_MT%"
) else (
    echo WARNING: mt.exe not found -- SxS manifest will be broken!
    set "CMAKE_MT_ARG="
)

:: Find vcpkg root.
if not defined VCPKG_ROOT set "VCPKG_ROOT=%CD%\vcpkg"
if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    set "TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
) else (
    set "TOOLCHAIN="
)

:: Configure with Visual Studio generator (Ninja also works locally).
echo === Configuring ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 %TOOLCHAIN% %CMAKE_MT_ARG%
if %ERRORLEVEL% neq 0 (
    echo CMake configure failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo === Building Release ===
cmake --build build --config Release --parallel
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo === Build finished with exit code %ERRORLEVEL% ===
pause
