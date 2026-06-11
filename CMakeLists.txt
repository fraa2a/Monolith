cmake_minimum_required(VERSION 3.27)
project(windows_recorder_monolith LANGUAGES CXX)

add_subdirectory(libs/capture)
add_subdirectory(libs/audio)
add_subdirectory(app/recorder)
