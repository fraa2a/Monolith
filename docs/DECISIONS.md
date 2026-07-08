# Decisions

This file is the architecture decision record index for Monolith.

## ADR-0001: Start From Scratch Instead Of Forking OBS

- Status: accepted.
- Decision: build a custom native recorder-first application.
- Notes: OBS remains a benchmark/reference, not the product base.

## ADR-0002: Native Windows Recording Engine

- Status: accepted, narrowed by ADR-0011 and ADR-0012.
- Decision: the capture/recording engine is native Windows code, not Electron,
  CEF, or a browser runtime.
- Current scope: `app/desktop-ui` may use Tauri/WebView2 as a sidecar UI only.

## ADR-0003: C++23 And CMake

- Status: accepted.
- Decision: use C++23 and CMake for the native engine and libraries.

## ADR-0004: FFmpeg/libav Encoding Backend

- Status: accepted.
- Decision: use FFmpeg/libav for encode, mux, remux, and thumbnail decode unless
  a future spike proves a better backend.

## ADR-0005: WASAPI Audio Capture

- Status: accepted.
- Decision: use WASAPI for desktop loopback, microphone/input devices, and
  Windows process-loopback where available.

## ADR-0006: Win32 Tray And Hotkeys

- Status: accepted.
- Decision: use Win32 message loop, `Shell_NotifyIcon`, and global keyboard
  handling for tray/hotkey control.

## ADR-0007: Local JSON-RPC IPC For Controllers

- Status: accepted.
- Decision: use newline-delimited JSON-RPC over TCP at `127.0.0.1:45991`.
- Notes: named pipes remain a deferred alternative. Stream Deck and UI mutations
  use this transport.

## ADR-0008: Single-Process Recording MVP

- Status: accepted.
- Decision: keep the recording engine in `Monolith.exe` with strict library
  boundaries under `libs/`.
- Future: split into a headless engine process after the MVP is stable.

## ADR-0009: SQLite Settings Store

- Status: accepted, supersedes the original `config.json`/WinUI settings model.
- Decision:
  - Store live settings in `%LocalAppData%\Monolith\settings.db`.
  - Use `libs/storage` as a generic top-level-section key/value store.
  - Keep `config/default-config.json` as the default seed/fallback.
  - Import legacy `%LocalAppData%\Monolith\config.json` once when the DB is empty,
    then rename it to `config.json.imported.bak`.
  - The UI writes `settings.db` and calls engine IPC `reload_settings`.
- Notes: engine load/save logic remains in `settings_config.cpp`.

## ADR-0010: SQLite Clip Catalogs Beside Output Folders

- Status: accepted.
- Decision:
  - Store replay metadata in `<clips folder>\clips.db`.
  - Store manual recording metadata in `<recordings folder>\recs.db`.
  - Store thumbnails under `.thumbs\` in each output folder.
  - The engine is the single writer. The UI opens catalogs read-only.
  - Use WAL and `busy_timeout`.
  - Self-heal on startup: remove rows for missing media, regenerate missing
    thumbnails, import pre-existing media.
- Notes: UI-driven favorite/hashtag/title/rename/delete/regen operations go
  through engine IPC.

## ADR-0011: Web UI Layer Exception

- Status: accepted in scope, shell implementation superseded by ADR-0012.
- Decision:
  - The no-browser rule applies to the recording engine.
  - `app/desktop-ui` may use a system WebView2 UI layer.
  - UI and engine communicate through JSON-RPC.
  - The engine remains the single writer for clip catalogs.
- Historical note: the first planned implementation used Deno Desktop. That
  shell was replaced by Tauri because Deno Desktop did not reliably show a
  window.

## ADR-0012: Tauri v2/WebView2 Desktop UI Shell

- Status: accepted.
- Decision:
  - Use Tauri v2/WebView2 for `Monolith.UI.exe`.
  - Rust host lives in `app/desktop-ui/src-tauri`.
  - Preact frontend remains in `app/desktop-ui/ui`.
  - Deno is build-only for the frontend bundle.
  - `cargo build --release` creates `monolith_ui.exe`, copied to
    `<recorder-output>\ui\Monolith.UI.exe`.
  - Tauri CLI bundling is not required; the app ships as a bare exe.
  - The Rust host runs a loopback HTTP server so the frontend keeps normal
    `/api`, `/media`, `/thumb`, static asset, and SSE contracts.
  - `rusqlite` with bundled SQLite removes the old first-run DLL issue.
- Notes:
  - Old WinUI and Deno Desktop hosts have been removed.
  - The installer ships `ui/Monolith.UI.exe`.

## ADR-0013: Settings And Clip Library Overhaul

- Status: accepted.
- Decision:
  - Schema version is `3`.
  - Encoder config is friendly: `device` (`gpu`/`cpu`), `codec`
    (`h264`/`h265`), `bitrate_kbps`, `fps`, `scaling_filter`, and
    `extra_ffmpeg_options`.
  - Concrete FFmpeg encoder is resolved at runtime from capabilities.
  - Resolution is a preset: `source`, `480p`, `720p`, `1080p`, `1440p`.
  - Replay memory budget and temp folder are internal; memory budget is fixed at
    512 MB.
  - Active-game timing tunables are not user-facing. Engine cadence is fixed at
    5s plus foreground-change fast scans.
  - Audio source volume is applied in direct routes and `TrackMixer`.
  - Clip catalog has display `title` independent from filename.
  - IPC adds `clip_set_title`, `clip_regen_thumb`, and `clip_generation`.
  - UI host exposes SSE at `/api/events` for clip refresh.
  - UI uses a native folder picker route instead of manual path entry.
  - Emoji-like structural UI icons were replaced by inline Lucide-style icons in
    the frontend.
- Notes: WebView2 multi-track playback depends on `HTMLVideoElement.audioTracks`;
  unavailable engines fall back to default track playback.

## Open Decisions

- Final MP4 remux/finalization policy for interrupted recordings.
- GPU-resident encoder API shape and fallback contract.
- Future engine/UI process split boundary.
