# Monolith.Settings — WinUI 3 (C# / .NET 8) settings sidecar.
#
# The recorder's tray "Settings…" entry launches Monolith.Settings.exe from
# its own directory, so the published output is dropped next to Monolith.exe.
# Self-contained publish: bundles the .NET runtime and the Windows App SDK
# runtime, so target machines need no preinstalled prerequisites.

find_program(DOTNET_EXECUTABLE
    NAMES dotnet dotnet.exe
    PATHS "$ENV{ProgramFiles}/dotnet")

if(DOTNET_EXECUTABLE)
    set(SETTINGS_PUBLISH_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/publish")
    set(SETTINGS_INSTALL_DIR "$<TARGET_FILE_DIR:monolith>/settings")

    add_custom_target(settings_app ALL
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SETTINGS_PUBLISH_DIR}"
        COMMAND "${DOTNET_EXECUTABLE}" publish
            "${CMAKE_CURRENT_SOURCE_DIR}/Monolith.Settings.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release>
            -r win-x64
            --self-contained true
            -p:Version=${MONOLITH_VERSION}
            -o "${SETTINGS_PUBLISH_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SETTINGS_INSTALL_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SETTINGS_PUBLISH_DIR}"
            "${SETTINGS_INSTALL_DIR}"
        COMMENT "Publishing Monolith.Settings (WinUI 3, self-contained) under the recorder output directory"
        VERBATIM)
    add_dependencies(settings_app monolith)
else()
    message(WARNING
        "dotnet SDK not found: Monolith.Settings.exe will NOT be built. "
        "Install the .NET 8 SDK (see global.json) and re-run CMake configure.")
endif()
