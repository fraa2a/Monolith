add_library(encoding STATIC
    encoding.cpp
    ffmpeg_process.cpp
    segment_replay.cpp
    mux_common.cpp
    thumbnail.cpp
)

target_include_directories(encoding
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(encoding
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

find_package(FFMPEG REQUIRED)
target_include_directories(encoding PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(encoding PRIVATE ${FFMPEG_LIBRARIES} platform_win)

if(MSVC)
    target_compile_options(encoding PRIVATE /W4 /permissive-)
    # Suppress C99 designated-init warnings from FFmpeg C headers.
    target_compile_options(encoding PRIVATE /wd4200)
endif()
