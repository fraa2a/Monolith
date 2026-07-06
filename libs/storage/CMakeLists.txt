add_library(storage STATIC
    storage.cpp
)

target_include_directories(storage
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(storage
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# SQLite from vcpkg (target name is unofficial::sqlite3::sqlite3).
find_package(unofficial-sqlite3 CONFIG REQUIRED)

target_link_libraries(storage
    PRIVATE
        encoding
        unofficial::sqlite3::sqlite3
)

if(MSVC)
    target_compile_options(storage PRIVATE /W4 /permissive-)
endif()
