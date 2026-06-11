add_library(replay_buffer STATIC
    replay_buffer.cpp
)

target_include_directories(replay_buffer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(replay_buffer
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

find_package(FFMPEG REQUIRED)
target_include_directories(replay_buffer PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(replay_buffer
    PRIVATE
        encoding
        ${FFMPEG_LIBRARIES}
)

if(MSVC)
    target_compile_options(replay_buffer PRIVATE /W4 /permissive-)
endif()
