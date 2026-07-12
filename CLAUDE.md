# CLAUDE.md

Dis file = work guide for agents and maintainers in repo.

## What This Is

Monolith = Windows-only, recorder-first screen capture + replay-buffer app.
Target Windows 11 x64. Built on native C++23 recording engine, Win32 tray/message-loop host, Tauri v2/WebView2 desktop UI sidecar, Elgato Stream Deck controller plugin.

App version = root `CMakeLists.txt` project version
(`1.4.2` local unless CI override `MONOLITH_VERSION` from tag).

## Build

Use root build script on Windows w/ MSVC, vcpkg, Rust/Cargo, Node/npm:

```bat
build.bat
```

Manual native build:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release --parallel
```

C++ build use vcpkg manifest mode (`vcpkg.json`) w/ pinned deps:
`nlohmann-json`, `sqlite3`, `winsparkle`, FFmpeg w/ hardware-encoder
features (`amf`, `nvcodec`, `qsv`) plus `x264`, `x265`, `gpl`.

Desktop UI live in `app/desktop-ui`:

- npm = build-only, bundle Preact frontend w/ Vite (`vite.config.ts`
  to `dist/`).
- Cargo build Tauri v2 host (`src-tauri/`).
- Result `monolith_ui.exe` copied by CMake to
  `<recorder-output>/ui/Monolith.UI.exe`.
- If npm or Cargo missing, CMake warn + skip UI target; recorder
  still build.

## Tests And Verification

No full auto test suite yet. No claim test verification
unless specific command run.

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
- Confirm tray icon appear + log reach "app shell ready".
- Save replay (`Ctrl+Shift+F8`), confirm clip + catalog row.
- Start/stop manual recording, confirm output + catalog row.
- Open `Monolith.UI.exe`, verify clip grid/settings, save a setting.
- Use Stream Deck plugin build output against IPC on `127.0.0.1:45991`.

## Architecture

Recorder = single-process MVP. Internal boundaries enforced by
CMake libs under `libs/`; future headless-engine split deferred.

Main modules:

- `app/recorder`: `Monolith.exe`, Win32 tray, message-only window, hotkeys,
  lifecycle, recording/replay orchestration, runtime status, updater.
- `app/desktop-ui`: `Monolith.UI.exe`, Tauri v2/WebView2 host + Preact UI.
  Rust modules serve bundled frontend, expose local HTTP APIs, read clip
  catalogs read-only, write settings, call engine JSON-RPC.
- `libs/capture`: Windows.Graphics.Capture D3D11 capture w/ CPU-readable BGRA
  staging path.
- `libs/audio`: WASAPI desktop/microphone/input/process-loopback capture +
  heuristic active-game detection.
- `libs/encoding`: FFmpeg video/audio encoders, thumbnail decode/gen,
  mux helpers, `TrackMixer`.
- `libs/replay-buffer`: encoded-packet ring buffer + clip save.
- `libs/recording`: manual recording writer.
- `libs/storage`: SQLite clip catalogs + `settings.db` key/value store.
- `libs/ipc`: localhost JSON-RPC server on `127.0.0.1:45991`.
- `plugins/stream-deck`: Node.js/TypeScript Stream Deck plugin using IPC
  contract.

Data crossing recording boundaries should = encoded packets, not raw frames.
Current capture path still do BGRA CPU readback + software
convert; GPU-resident encode = main perf target.

## Runtime State

- User media defaults: `Videos\Monolith\Clips` +
  `Videos\Monolith\Recordings`.
- Runtime data: `%LocalAppData%\Monolith`.
- Settings store of record: `%LocalAppData%\Monolith\settings.db`.
- Legacy `%LocalAppData%\Monolith\config.json` imported once, then renamed to
  `config.json.imported.bak`.
- Runtime capability/status file: `%LocalAppData%\Monolith\runtime-status.json`.
- Installed app path: `%LocalAppData%\Programs\Monolith`.
- Default seed config: `config/default-config.json`.

## Settings Apply Scopes

Settings UI write `settings.db` + call engine JSON-RPC
`reload_settings`, which post `WM_APP + 2` to recorder.

- Output folders + hotkeys apply live for future ops.
- Replay duration restart/reconfigure replay buffer.
- Replay memory budget fixed internal at 512 MB.
- Audio, capture, encoder changes restart pipelines only when manual
  recording not active; unsafe changes deferred during recording.
- Full app restart reserved for future settings that cannot reload
  safely.

## IPC Contract

Transport: newline-delimited JSON-RPC over TCP at `127.0.0.1:45991`.
Server thread one handler per connection (backlog 8), so UI + Stream
Deck plugin stay connected at once. Loopback-only (bound to
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

`get_status` return recording state + `clip_generation`, monotonic counter
used by UI host to refresh clip grid.

## Working Rules

- Keep MSVC `/W4 /permissive-` clean.
- No block tray/UI message loop on capture, encode, mux, disk I/O, or
  IPC.
- Keep capture/audio/encoding/storage/replay/recording/UI/plugin boundaries
  intact.
- No add heavy deps without concrete module need.
- Document non-obvious Windows API or architecture choices in `docs/DECISIONS.md`
  or nearby source comments.
- Update `docs/handover/ACTIVE_HANDOVER.md` at end of session.