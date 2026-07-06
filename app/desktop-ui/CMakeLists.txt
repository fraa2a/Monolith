# Monolith Desktop UI — Tauri v2 (WebView2). `cargo build --release` produces a
# single self-contained exe (src-tauri/target/release/monolith_ui.exe) that starts
# its own loopback HTTP server and opens the WebView2 window. It is copied to
# <recorder-output>/ui/Monolith.UI.exe; the recorder's tray "Settings…" entry
# launches it (settings_window.cpp), preferring it over the legacy WinUI sidecar.
#
# Two toolchains are needed: `deno` bundles the Preact frontend into dist/
# (build.ts, embedded into the exe via include_dir!) and `cargo` compiles the
# Tauri host. If either is absent, CMake warns and skips the UI build — the
# recorder and the legacy settings sidecar still build, mirroring the
# .NET-SDK-optional behaviour.

find_program(DENO_EXECUTABLE  NAMES deno deno.exe)
find_program(CARGO_EXECUTABLE NAMES cargo cargo.exe)

if(DENO_EXECUTABLE AND CARGO_EXECUTABLE)
    set(UI_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAURI_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri")
    set(CARGO_EXE_OUT "${TAURI_DIR}/target/release/monolith_ui.exe")
    set(UI_INSTALL_DIR "$<TARGET_FILE_DIR:monolith>/ui")

    add_custom_target(desktop_ui ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UI_INSTALL_DIR}"
        # 1. Bundle the Preact frontend into dist/ (embedded into the exe at
        #    compile time by include_dir!, so dist/ must exist before cargo runs).
        COMMAND "${DENO_EXECUTABLE}" run -A build.ts
        # 2. Compile the Tauri host. Plain `cargo build --release` is used (not
        #    `cargo tauri build`) so the tauri-cli is not required — bundling is
        #    disabled (tauri.conf.json bundle.active=false) and we ship the bare exe.
        COMMAND "${CARGO_EXECUTABLE}" build --release --manifest-path "${TAURI_DIR}/Cargo.toml"
        # 3. Drop the exe next to the recorder as ui/Monolith.UI.exe.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CARGO_EXE_OUT}" "${UI_INSTALL_DIR}/Monolith.UI.exe"
        WORKING_DIRECTORY "${UI_SRC_DIR}"
        COMMENT "Building Monolith.UI (Tauri v2, self-contained exe)"
        VERBATIM)
    add_dependencies(desktop_ui monolith)
else()
    message(WARNING
        "deno and/or cargo not found: the Monolith.UI Tauri app will NOT be built. "
        "Install Deno 2.9+ (https://deno.com) and the Rust toolchain "
        "(https://rustup.rs) and re-run CMake configure. The recorder and legacy "
        "WinUI settings sidecar still build.")
endif()
