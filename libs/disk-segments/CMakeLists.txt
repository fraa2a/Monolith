add_library(disk_segments STATIC
    disk_segments.cpp
)

target_include_directories(disk_segments
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(disk_segments
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

find_package(FFMPEG REQUIRED)
target_include_directories(disk_segments PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(disk_segments
    PRIVATE
        encoding
        ${FFMPEG_LIBRARIES}
)

if(MSVC)
    target_compile_options(disk_segments PRIVATE /W4 /permissive-)
endif()
