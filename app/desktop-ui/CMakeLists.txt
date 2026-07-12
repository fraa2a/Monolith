# Monolith Desktop UI Tauri v2 (WebView2). `cargo build --release` produces a
# single self-contained exe (src-tauri/target/release/monolith_ui.exe) that starts
# its own loopback HTTP server and opens the WebView2 window. It is copied to
# <recorder-output>/ui/Monolith.UI.exe; the recorder's tray "Settings…" entry
# launches it (settings_window.cpp), preferring it over the legacy WinUI sidecar.

find_program(NPM_EXECUTABLE   NAMES npm npm.cmd)
find_program(CARGO_EXECUTABLE NAMES cargo cargo.exe)

if(NPM_EXECUTABLE AND CARGO_EXECUTABLE)
    set(UI_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAURI_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri")
    set(CARGO_EXE_OUT "${TAURI_DIR}/target/release/monolith_ui.exe")
    set(UI_INSTALL_DIR "$<TARGET_FILE_DIR:monolith>/ui")

    add_custom_target(desktop_ui ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UI_INSTALL_DIR}"
        # 1. Install frontend deps (no-op if node_modules is already current) and
        #    bundle the Preact frontend into dist/ (embedded into the exe at
        #    compile time by include_dir!, so dist/ must exist before cargo runs).
        COMMAND "${NPM_EXECUTABLE}" install
        COMMAND "${NPM_EXECUTABLE}" run build
        # 2. Compile the Tauri host. Plain `cargo build --release` is used (not
        #    `cargo tauri build`) so the tauri-cli is not required — bundling is
        #    disabled (tauri.conf.json bundle.active=false) and we ship the bare exe.
        #    --features custom-protocol is REQUIRED: without it tauri's build
        #    script sets cfg(dev)=true and the shipped exe loads build.devUrl
        #    (localhost:1420) instead of the embedded frontend. `cargo tauri
        #    build` would add this flag for us; plain cargo does not.
        COMMAND "${CARGO_EXECUTABLE}" build --release --features custom-protocol --manifest-path "${TAURI_DIR}/Cargo.toml"
        # 3. Drop the exe next to the recorder as ui/Monolith.UI.exe.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CARGO_EXE_OUT}" "${UI_INSTALL_DIR}/Monolith.UI.exe"
        WORKING_DIRECTORY "${UI_SRC_DIR}"
        COMMENT "Building Monolith.UI (Tauri v2, self-contained exe)"
        VERBATIM)
    add_dependencies(desktop_ui monolith)
else()
    message(WARNING
        "npm and/or cargo not found: the Monolith.UI Tauri app will NOT be built. "
        "Install Node.js (https://nodejs.org) and the Rust toolchain "
        "(https://rustup.rs) and re-run CMake configure. The recorder and legacy "
        "WinUI settings sidecar still build.")
endif()
