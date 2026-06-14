cmake_minimum_required(VERSION 3.27)

# vcpkg toolchain auto-detection (set VCPKG_ROOT env var or pass
# -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake).
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(monolith VERSION 0.5.3 LANGUAGES CXX)

# Single source of truth for the app version. CI overrides this from the git
# tag with -DMONOLITH_VERSION=X.Y.Z; local builds fall back to project(VERSION).
set(MONOLITH_VERSION "${PROJECT_VERSION}" CACHE STRING "Monolith app version (X.Y.Z)")

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
add_subdirectory(libs/audio)
add_subdirectory(libs/encoding)
add_subdirectory(libs/replay-buffer)
add_subdirectory(libs/recording)
add_subdirectory(libs/ipc)
add_subdirectory(app/recorder)
add_subdirectory(app/desktop-ui)
