cmake_minimum_required(VERSION 3.27)

# vcpkg toolchain auto-detection (set VCPKG_ROOT env var or pass
# -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake).
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(windows_recorder_monolith LANGUAGES CXX)

add_subdirectory(libs/capture)
add_subdirectory(libs/audio)
add_subdirectory(libs/encoding)
add_subdirectory(libs/replay-buffer)
add_subdirectory(app/recorder)
