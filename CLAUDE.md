# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Monolith is a Windows-only, recorder-first screen capture/replay-buffer app (an OBS alternative narrowed to recording + clip saving). Native C++23 engine + Win32 tray host in one process, with a Tauri v2 / WebView2 UI sidecar (`Monolith.UI.exe`). Windows 11, x64 only.

## Build

The native engine and the Tauri UI build together through CMake. Building requires Windows with MSVC, vcpkg, the Rust toolchain (cargo), and Deno (used only to bundle the UI frontend).

```bat
build.bat
```

`build.bat` locates `mt.exe` and vcpkg, configures with the Visual Studio 17 2022 generator into `build/`, and builds Release. Equivalent manual invocation:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release --parallel
```

- Dependencies are vcpkg manifest-mode (`vcpkg.json`), pinned via `builtin-baseline`: `nlohmann-json`, `winsparkle`, and `ffmpeg` (with `amf`, `nvcodec`, `qsv`, `x264`, `x265`, `gpl` features). To bump deps, change the baseline SHA deliberately.
- The new UI (`app/desktop-ui`) is a **Tauri v2 / WebView2** app: `deno` (2.9+) bundles the Preact frontend into `dist/` (`build.ts`) and `cargo build --release` compiles the self-contained `src-tauri/target/release/monolith_ui.exe`, which a CMake custom target copies to `<recorder-output>/ui/Monolith.UI.exe` (skipped with a warning if `deno` or `cargo` is absent; the tauri-cli is not needed). A missing toolchain is non-fatal — the recorder still builds.

## Tests

There are no automated tests yet — `tests/` is a placeholder. Do not claim a build is verified by tests. Verification is manual (run the app, save a clip, check output files/logs).

## Releasing

Releases are tag-driven; CI does the real build. See `docs/RELEASING.md`.

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

`.github/workflows/version-tag.yml` extracts the version, passes `-DMONOLITH_VERSION=X.Y.Z` to CMake (the single source of truth, flowing into the exe VERSIONINFO, C# assembly version, and installer), builds, runs Inno Setup (`installer/monolith.iss`), Ed25519-signs the installer, generates the WinSparkle appcast (`scripts/generate-appcast.ps1`), and publishes to the **public** `fraa2a/Monolith-releases` repo (this repo is private). Do not commit the WinSparkle private key; the public key is pinned in `app/recorder/src/updater.cpp`.

## Architecture

Single-process MVP. Module boundaries are enforced by library separation under `libs/`, not by process separation. The `app/recorder` executable links all libs and owns the Win32 message loop, tray, hotkeys, and lifecycle.

Actual targets (see root `CMakeLists.txt` — note this differs from the larger module list in `PROJECT_PLAN.md`, which is aspirational):

- `app/recorder` — the `Monolith.exe` engine + tray host. `src/main.cpp` (~2200 lines) is the orchestrator: message-only window, single-instance guard, global hotkeys, the recording/replay state, audio-source wiring, and Active Game polling. `settings_config.cpp` owns config schema/load/validate/clamp; `settings_window.cpp` bridges to the sidecar; `updater.cpp` is WinSparkle.
- `app/desktop-ui` — the new UI layer (`Monolith.UI.exe`), a **Tauri v2** (WebView2) app: Home = clip grid, Settings = popup. Launched from the tray; not always-on. Rust host (`src-tauri/`: `main.rs`, `server.rs`, `media.rs`, plus data modules `clip_catalog.rs`/`settings_store.rs`/`engine_rpc.rs`/`game_catalog.rs`/`paths.rs`) + Preact frontend (`ui/`). The host runs a loopback HTTP server and the window loads `http://127.0.0.1:<port>/`, so the frontend keeps its `fetch()`/`/media` route contract. Reads the clip DBs read-only; clip mutations + settings-reload go to the engine over JSON-RPC. See ADR-0011 / ADR-0012. (The old Deno host — `main.ts`, `server/`, `bindings/` — is being removed.)
- `libs/capture` — WGC (`Windows.Graphics.Capture`) display capture.
- `libs/audio` — WASAPI loopback/microphone/input + process-loopback capture, plus `detect_active_game()`.
- `libs/encoding` — FFmpeg H.264/AAC encode wrappers; `mux_common` for muxing.
- `libs/replay-buffer` — encoded-packet ring buffer + keyframe-safe MKV clip extraction.
- `libs/recording` — manual recording state/flow.
- `libs/ipc` — JSON-RPC server (Stream Deck transport; not fully wired yet).

Data crosses module boundaries as **encoded packets**, not raw frames. The replay buffer holds encoded packets and walks back to the nearest keyframe on clip save.

### Cross-cutting invariants

- **Never block the UI/tray thread** on capture, encode, disk I/O, or IPC.
- **Avoid CPU/GPU copies**; GPU-resident frame flow is the default design constraint. Prefer encoded-packet buffering over raw-frame buffering.
- **MKV is the active-write container** (salvageable after crash); MP4 only via remux after successful finalization.
- **Capability-gate the UI**: never expose encoder/UI options that runtime probing can't actually support on the current machine.
- No Electron, WebView2/CEF, or browser-shell anything (`docs/DEVELOPMENT_RULES.md`).

### Settings apply scopes

Settings is a sidecar: on Save it writes `settings.db` and posts `WM_APP + 2` to the recorder's message window. Changes apply at different scopes — output folders/hotkeys are live; replay duration restarts the replay buffer (the memory budget is fixed internally at 512 MB, not a setting); audio mode/routing/volume and capture/encoder changes restart their pipelines **only when no manual recording is active** (unsafe changes are deferred during recording). Avoid forcing a full app restart for reloadable settings. The encoder is configured as CPU/GPU + H.264/H.265 + CBR bitrate (see ADR-0013); resolution is a monitor-aspect preset.

### Audio V2 + Active Game

Default mode = desktop audio → track 1, primary mic → track 2. Custom mode persists routing for desktop audio, input devices, process sessions, and "Active Game" across up to six logical tracks, each with a per-source `volume` (0–1) that is **applied to the recording** — direct-track sources get gain via `apply_gain_inplace()` in `main.cpp`, mixed tracks via per-source gain in `TrackMixer` (`libs/encoding`). Multiple sources on one track ARE mixed (summed) by the `TrackMixer`; a single source on a track bypasses the mixer and feeds its encoder directly. `audio::detect_active_game()` is heuristic (fullscreen/foreground/audio-session/whitelist scoring); `poll_active_game()` in `main.cpp` is the single switch decision point (30 s timer + foreground-change fast scan) and swaps only the Active Game capture, never the replay buffer. Details in `docs/ARCHITECTURE.md`.

## Paths at runtime

- User video output: `Videos\Monolith\` (Clips, Recordings).
- Config + logs + runtime data: `%LocalAppData%\Monolith\` (`config.json`, `runtime-status.json`, `settings-startup.log`).
- Installed (per-user, no admin): `%LocalAppData%\Programs\Monolith\`.
- `config/default-config.json` is the seed schema; the live config lives under `%LocalAppData%\Monolith`.

## Working conventions

- MSVC builds with `/W4 /permissive-`; keep new code warning-clean.
- Document non-obvious Windows API choices in an ADR (`docs/DECISIONS.md`) or a nearby comment with rationale. Do not change architecture direction, process model, config schema shape, or the IPC contract without updating `docs/DECISIONS.md`.
- Keep modules isolated — no UI logic inside capture/audio/encoding/replay-buffer libs.
- Don't add heavy dependencies or broad abstractions before a concrete module needs them.
