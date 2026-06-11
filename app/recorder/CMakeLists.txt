cmake_minimum_required(VERSION 3.27)

project(monolith LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(monolith WIN32
    src/main.cpp
)

set_target_properties(monolith PROPERTIES
    OUTPUT_NAME "Monolith"
)

target_include_directories(monolith
    PRIVATE
        include
)

target_compile_definitions(monolith
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(monolith
    PRIVATE
        capture
        audio
        encoding
        replay_buffer
        recording
        # System libs
        d3d11
        dxgi
        windowsapp
        ole32
        shell32
        user32
)

if(MSVC)
    target_compile_options(monolith PRIVATE /W4 /permissive-)
    target_link_options(monolith PRIVATE
        /MANIFEST:EMBED
        /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
    )
endif()
