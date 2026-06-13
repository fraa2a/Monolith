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
    add_custom_target(settings_app ALL
        COMMAND "${DOTNET_EXECUTABLE}" publish
            "${CMAKE_CURRENT_SOURCE_DIR}/Monolith.Settings.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release>
            -r win-x64
            --self-contained true
            -p:Version=${MONOLITH_VERSION}
            -o "$<TARGET_FILE_DIR:monolith>"
        COMMENT "Publishing Monolith.Settings (WinUI 3, self-contained) next to Monolith.exe"
        VERBATIM)
    add_dependencies(settings_app monolith)
else()
    message(WARNING
        "dotnet SDK not found: Monolith.Settings.exe will NOT be built. "
        "Install the .NET 8 SDK (see global.json) and re-run CMake configure.")
endif()
