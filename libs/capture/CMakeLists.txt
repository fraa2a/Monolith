add_library(capture STATIC
    capture.cpp
)

# Expose libs/ as the include root so consumers use: #include <capture/capture.h>
target_include_directories(capture
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(capture
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(capture PRIVATE /W4 /permissive-)
endif()
