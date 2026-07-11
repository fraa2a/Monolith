add_library(gamelist STATIC
    gamelist.cpp
)

target_include_directories(gamelist
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(gamelist
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# SQLite from vcpkg (target name is unofficial::sqlite3::sqlite3).
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(gamelist
    PRIVATE
        platform_win
        unofficial::sqlite3::sqlite3
        nlohmann_json::nlohmann_json
        winhttp
)

if(MSVC)
    target_compile_options(gamelist PRIVATE /W4 /permissive-)
endif()
