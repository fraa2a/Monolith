# CLAUDE.md

This file is the working guide for agents and maintainers in this repository.

## What This Is

Monolith is a Windows-only, recorder-first screen capture and replay-buffer app.
It targets Windows 11 x64 and is built around a native C++23 recording engine,
a Win32 tray/message-loop host, a Tauri v2/WebView2 desktop UI sidecar, and an
Elgato Stream Deck controller plugin.

Current app version source is the root `CMakeLists.txt` project version
(`1.4.2` locally unless CI overrides `MONOLITH_VERSION` from a tag).

## Build

Use the root build script on Windows with MSVC, vcpkg, Rust/Cargo, and Node/npm:

```bat
build.bat
```

Manual native build:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release --parallel
```

The C++ build uses vcpkg manifest mode (`vcpkg.json`) with pinned dependencies:
`nlohmann-json`, `sqlite3`, `winsparkle`, and FFmpeg with hardware-encoder
features (`amf`, `nvcodec`, `qsv`) plus `x264`, `x265`, and `gpl`.

The desktop UI lives in `app/desktop-ui`:

- npm is build-only and bundles the Preact frontend with Vite (`vite.config.ts`
  to `dist/`).
- Cargo builds the Tauri v2 host (`src-tauri/`).
- The resulting `monolith_ui.exe` is copied by CMake to
  `<recorder-output>/ui/Monolith.UI.exe`.
- If npm or Cargo is missing, CMake warns and skips the UI target; the recorder
  can still build.

## Tests And Verification

There is no complete automated test suite yet. Do not claim test verification
unless a specific command was run.

Useful checks:

```bat
cmake --build build --config Release --parallel
cd app\desktop-ui
npm install
npm run build
cargo build --release --manifest-path src-tauri\Cargo.toml
cd plugins\stream-deck
npm run build
```

Manual runtime smoke:

- Start `Monolith.exe`.
- Confirm tray icon appears and log reaches "app shell ready".
- Save a replay (`Ctrl+Shift+F8`) and confirm a clip plus catalog row.
- Start/stop manual recording and confirm output plus catalog row.
- Open `Monolith.UI.exe`, verify clip grid/settings, and save a setting.
- Use Stream Deck plugin build output against IPC on `127.0.0.1:45991`.

## Architecture

The recorder remains a single-process MVP. Internal boundaries are enforced by
CMake libraries under `libs/`; the future headless-engine split is deferred.

Main modules:

- `app/recorder`: `Monolith.exe`, Win32 tray, message-only window, hotkeys,
  lifecycle, recording/replay orchestration, runtime status, updater.
- `app/desktop-ui`: `Monolith.UI.exe`, Tauri v2/WebView2 host plus Preact UI.
  Rust modules serve the bundled frontend, expose local HTTP APIs, read clip
  catalogs read-only, write settings, and call engine JSON-RPC.
- `libs/capture`: Windows.Graphics.Capture D3D11 capture with CPU-readable BGRA
  staging path.
- `libs/audio`: WASAPI desktop/microphone/input/process-loopback capture and
  heuristic active-game detection.
- `libs/encoding`: FFmpeg video/audio encoders, thumbnail decode/generation,
  mux helpers, and `TrackMixer`.
- `libs/replay-buffer`: encoded-packet ring buffer and clip save.
- `libs/recording`: manual recording writer.
- `libs/storage`: SQLite clip catalogs and `settings.db` key/value store.
- `libs/ipc`: localhost JSON-RPC server on `127.0.0.1:45991`.
- `plugins/stream-deck`: Node.js/TypeScript Stream Deck plugin using the IPC
  contract.

Data crossing recording boundaries should be encoded packets, not raw frames.
The current capture path still performs BGRA CPU readback and software
conversion; GPU-resident encode remains the main performance target.

## Runtime State

- User media defaults: `Videos\Monolith\Clips` and
  `Videos\Monolith\Recordings`.
- Runtime data: `%LocalAppData%\Monolith`.
- Settings store of record: `%LocalAppData%\Monolith\settings.db`.
- Legacy `%LocalAppData%\Monolith\config.json` is imported once, then renamed to
  `config.json.imported.bak`.
- Runtime capability/status file: `%LocalAppData%\Monolith\runtime-status.json`.
- Installed app path: `%LocalAppData%\Programs\Monolith`.
- Default seed config: `config/default-config.json`.

## Settings Apply Scopes

The settings UI writes `settings.db` and calls engine JSON-RPC
`reload_settings`, which posts `WM_APP + 2` to the recorder.

- Output folders and hotkeys apply live for future operations.
- Replay duration restarts/reconfigures the replay buffer.
- Replay memory budget is fixed internally at 512 MB.
- Audio, capture, and encoder changes restart their pipelines only when manual
  recording is not active; unsafe changes are deferred during recording.
- A full app restart should be reserved for future settings that cannot be
  reloaded safely.

## IPC Contract

Transport: newline-delimited JSON-RPC over TCP at `127.0.0.1:45991`. The
server threads one handler per connection (backlog 8), so the UI and Stream
Deck plugin can stay connected at once. Loopback-only (bound to
`127.0.0.1`); no request-level auth token.

Recorder commands:

- `save_replay`
- `recording_start`
- `recording_stop`
- `pause_resume`
- `get_status`
- `reload_settings`

Clip mutations:

- `clip_set_favorite`
- `clip_add_hashtag`
- `clip_remove_hashtag`
- `clip_rename`
- `clip_set_title`
- `clip_regen_thumb`
- `clip_delete`

`get_status` returns recording state plus `clip_generation`, a monotonic counter
used by the UI host to refresh the clip grid.

## Working Rules

- Keep MSVC `/W4 /permissive-` clean.
- Do not block the tray/UI message loop on capture, encode, mux, disk I/O, or
  IPC.
- Keep capture/audio/encoding/storage/replay/recording/UI/plugin boundaries
  intact.
- Do not add heavy dependencies without a concrete module need.
- Document non-obvious Windows API or architecture choices in `docs/DECISIONS.md`
  or nearby source comments.
- Update `docs/handover/ACTIVE_HANDOVER.md` at the end of a session.
