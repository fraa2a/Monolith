cmake_minimum_required(VERSION 3.27)

# vcpkg toolchain auto-detection (set VCPKG_ROOT env var or pass
# -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake).
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(monolith VERSION 1.5.0 LANGUAGES CXX)

# Engine version — the engine component of the component updater (the ui and
# updater components carry their own versions in their tauri.conf.json).
# Bump it when the engine changes; local builds fall back to project(VERSION).
set(MONOLITH_VERSION "${PROJECT_VERSION}" CACHE STRING "Monolith engine version (X.Y.Z)")

# Split into numeric components for the Win32 VERSIONINFO resource.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _monolith_ver_match "${MONOLITH_VERSION}")
if(NOT _monolith_ver_match)
    message(FATAL_ERROR "MONOLITH_VERSION must look like X.Y.Z, got: ${MONOLITH_VERSION}")
endif()
set(MONOLITH_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(MONOLITH_VERSION_MINOR "${CMAKE_MATCH_2}")
set(MONOLITH_VERSION_PATCH "${CMAKE_MATCH_3}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(libs/capture)
add_subdirectory(libs/platform-win)
add_subdirectory(libs/logging)
add_subdirectory(libs/gamelist)
add_subdirectory(libs/audio)
add_subdirectory(libs/encoding)
add_subdirectory(libs/storage)
add_subdirectory(libs/replay-buffer)
add_subdirectory(libs/disk-segments)
add_subdirectory(libs/recording)
add_subdirectory(libs/ipc)
add_subdirectory(app/recorder)
# Tauri v2 / WebView2 UI (Home = clip grid, Settings = popup). npm/Vite bundles
# the frontend and `cargo build --release` compiles Monolith.UI.exe when both
# toolchains are present. See ADR-0012.
add_subdirectory(app/desktop-ui)
# Component self-updater (Updater.exe): fetches update-manifest.json and
# swaps only the components whose version changed. Sits next to Monolith.exe.
add_subdirectory(app/updater)
