cmake_minimum_required(VERSION 3.27)
project(ipc LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(ipc STATIC ipc_server.cpp)

target_include_directories(ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ipc
    PRIVATE
        nlohmann_json::nlohmann_json
        ws2_32
        advapi32
)

target_compile_features(ipc PUBLIC cxx_std_23)

target_compile_definitions(ipc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
