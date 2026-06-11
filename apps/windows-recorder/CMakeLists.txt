cmake_minimum_required(VERSION 3.27)

project(windows_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(windows_recorder
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
)

if(MSVC)
    target_compile_options(windows_recorder PRIVATE /W4 /permissive-)
endif()

