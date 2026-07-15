cmake_minimum_required(VERSION 3.27)

project(monolith LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generated version header consumed by monolith.rc and the updater.
configure_file(
    src/version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

# Generated default-config header: embeds config/default-config.json (the
# single source of truth for default settings) as a raw string literal, so
# there is no hand-maintained fallback copy to drift out of sync.
file(READ "${CMAKE_SOURCE_DIR}/config/default-config.json" DEFAULT_CONFIG_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/config/default_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/default_config.h"
    @ONLY
)

add_executable(monolith WIN32
    src/main.cpp
    src/settings_config.cpp
    src/settings_window.cpp
    src/updater.cpp
    monolith.rc
)

set_target_properties(monolith PROPERTIES
    OUTPUT_NAME "Monolith"
)

target_include_directories(monolith
    PRIVATE
        include
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(monolith
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# WinSparkle ships as a prebuilt DLL via vcpkg (no CMake config package).
find_path(WINSPARKLE_INCLUDE_DIR winsparkle/winsparkle.h REQUIRED)
find_library(WINSPARKLE_LIBRARY WinSparkle REQUIRED)

target_include_directories(monolith PRIVATE "${WINSPARKLE_INCLUDE_DIR}")

target_link_libraries(monolith
    PRIVATE
        capture
        platform_win
        logging
        gamelist
        audio
        encoding
        storage
        ipc
        nlohmann_json::nlohmann_json
        "${WINSPARKLE_LIBRARY}"
        # System libs
        d3d11
        dxgi
        windowsapp
        ole32
        shell32
        user32
        winmm
)

if(MSVC)
    target_compile_options(monolith PRIVATE /W4 /permissive-)
    target_link_options(monolith PRIVATE
        /MANIFEST:EMBED
        "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
    )
endif()
