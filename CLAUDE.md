# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Monolith is a Windows-only, recorder-first screen capture/replay-buffer app (an OBS alternative narrowed to recording + clip saving). Native C++23 engine + Win32 tray host in one process, with a WinUI 3 (C# / .NET 8) settings sidecar. Windows 11, x64 only.

## Build

The native app and the settings sidecar build together through CMake. Building requires Windows with MSVC, vcpkg, and the .NET 8 SDK.

```bat
build.bat
```

`build.bat` locates `mt.exe` and vcpkg, configures with the Visual Studio 17 2022 generator into `build/`, and builds Release. Equivalent manual invocation:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release --parallel
```

- Dependencies are vcpkg manifest-mode (`vcpkg.json`), pinned via `builtin-baseline`: `nlohmann-json`, `winsparkle`, and `ffmpeg` (with `amf`, `nvcodec`, `qsv`, `x264`, `x265`, `gpl` features). To bump deps, change the baseline SHA deliberately.
- The settings app (`app/desktop-ui`) is published self-contained via `dotnet publish` as a CMake custom target and dropped into `<recorder-output>/settings/`. If the .NET SDK is missing, CMake warns and skips it but still builds the recorder.

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
- `app/desktop-ui` — `Monolith.Settings.exe`, a WinUI 3 sidecar launched from the tray. Not an always-on process. MVVM under `Models/`, `ViewModels/`, `Services/`.
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

Settings is a sidecar: on Save it writes `config.json` and posts `WM_APP + 2` to the recorder's message window. Changes apply at different scopes — output folders/hotkeys are live; replay duration/budget restart the replay buffer; audio mode/routing and capture/encoder changes restart their pipelines **only when no manual recording is active** (unsafe changes are deferred during recording). Avoid forcing a full app restart for reloadable settings.

### Audio V2 + Active Game

Default mode = desktop audio → track 1, primary mic → track 2. Custom mode persists routing for desktop audio, input devices, process sessions, and "Active Game" across up to six logical tracks. Current limitation: multiple sources on one track are **not mixed** — the runtime logs and skips the later source to avoid corrupt timing. `audio::detect_active_game()` is heuristic (fullscreen/foreground/audio-session/whitelist scoring); `poll_active_game()` in `main.cpp` is the single switch decision point (30 s timer + foreground-change fast scan) and swaps only the Active Game capture, never the replay buffer. Details in `docs/ARCHITECTURE.md`.

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
