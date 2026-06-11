add_library(audio STATIC
    audio.cpp
)

target_include_directories(audio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(audio
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(audio PRIVATE /W4 /permissive-)
endif()
