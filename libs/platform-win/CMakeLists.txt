add_library(platform_win STATIC
    platform_win.cpp
)

target_include_directories(platform_win
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(platform_win
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(platform_win PRIVATE /W4 /permissive-)
endif()
