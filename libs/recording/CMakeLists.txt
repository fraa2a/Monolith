add_library(recording STATIC
    recording.cpp
)

target_include_directories(recording
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(recording
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

find_package(FFMPEG REQUIRED)
target_include_directories(recording PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(recording
    PRIVATE
        encoding
        ${FFMPEG_LIBRARIES}
)

if(MSVC)
    target_compile_options(recording PRIVATE /W4 /permissive-)
endif()
