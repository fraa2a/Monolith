cmake_minimum_required(VERSION 3.27)

project(windows_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(windows_recorder WIN32
    src/main.cpp
)

target_include_directories(windows_recorder
    PRIVATE
        include
)

target_compile_definitions(windows_recorder
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(windows_recorder
    PRIVATE
        capture
        audio
        encoding
        replay_buffer
        # System libs
        d3d11
        dxgi
        windowsapp
        ole32
        shell32
        user32
)

if(MSVC)
    target_compile_options(windows_recorder PRIVATE /W4 /permissive-)
    target_link_options(windows_recorder PRIVATE
        /MANIFEST:EMBED
        /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
    )
endif()
