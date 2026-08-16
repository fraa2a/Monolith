# Monolith Updater (Tauri v2 / WebView2). `cargo build --release` produces a
# single self-contained exe (src-tauri/target/release/updater.exe) — the
# component self-updater that fetches update-manifest.json, downloads only
# the changed components (engine / ui / updater), verifies Ed25519
# signatures and swaps the files on disk. It is copied to
# <recorder-output>/Updater.exe (app root, next to Monolith.exe); the
# recorder's tray "Check for Updates…" entry and the startup auto-check
# launch it (updater.cpp).

find_program(NPM_EXECUTABLE   NAMES npm npm.cmd)
find_program(CARGO_EXECUTABLE NAMES cargo cargo.exe)

if(NPM_EXECUTABLE AND CARGO_EXECUTABLE)
    set(UPDATER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TAURI_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri")
    set(CARGO_EXE_OUT   "${TAURI_DIR}/target/release/updater.exe")
    set(ENGINE_OUT_DIR  "$<TARGET_FILE_DIR:monolith>")

    add_custom_target(updater_app ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ENGINE_OUT_DIR}"
        # 1. Install frontend deps and bundle the Preact frontend into dist/
        #    (embedded into the exe at compile time, so dist/ must exist
        #    before cargo runs; npm run build's postbuild step also invokes
        #    cargo — the explicit cargo step below is idempotent).
        COMMAND "${NPM_EXECUTABLE}" install
        COMMAND "${NPM_EXECUTABLE}" run build
        # 2. Compile the Tauri host. Plain `cargo build --release` (not
        #    `cargo tauri build`): bundling is disabled (tauri.conf.json
        #    bundle.active=false) and we ship the bare exe. --features
        #    custom-protocol is REQUIRED so the shipped exe loads the
        #    embedded frontend instead of devUrl (localhost:1421).
        COMMAND "${CARGO_EXECUTABLE}" build --release --features custom-protocol --manifest-path "${TAURI_DIR}/Cargo.toml"
        # 3. Drop the exe next to the recorder as Updater.exe.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CARGO_EXE_OUT}" "${ENGINE_OUT_DIR}/Updater.exe"
        WORKING_DIRECTORY "${UPDATER_SRC_DIR}"
        COMMENT "Building Updater.exe (Tauri v2 component updater)"
        VERBATIM)
    add_dependencies(updater_app monolith)
else()
    message(WARNING
        "npm and/or cargo not found: the Updater.exe Tauri app will NOT be "
        "built. Install Node.js (https://nodejs.org) and the Rust toolchain "
        "(https://rustup.rs) and re-run CMake configure.")
endif()
