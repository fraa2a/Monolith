# Decisions

File be architecture decision record index for Monolith.

## ADR-0001: Start From Scratch Instead Of Forking OBS

- Status: accepted.
- Decision: build custom native recorder-first app.
- Notes: OBS stay benchmark/reference, not product base.

## ADR-0002: Native Windows Recording Engine

- Status: accepted, narrowed by ADR-0011 and ADR-0012.
- Decision: capture/recording engine be native Windows code, not Electron,
  CEF, or browser runtime.
- Current scope: `app/desktop-ui` may use Tauri/WebView2 as sidecar UI only.

## ADR-0003: C++23 And CMake

- Status: accepted.
- Decision: use C++23 and CMake for native engine and libs.

## ADR-0004: FFmpeg/libav Encoding Backend

- Status: accepted.
- Decision: use FFmpeg/libav for encode, mux, remux, thumbnail decode unless
  future spike prove better backend.

## ADR-0005: WASAPI Audio Capture

- Status: accepted.
- Decision: use WASAPI for desktop loopback, mic/input devices, and
  Windows process-loopback where available.

## ADR-0006: Win32 Tray And Hotkeys

- Status: accepted.
- Decision: use Win32 message loop, `Shell_NotifyIcon`, and global keyboard
  handling for tray/hotkey control.

## ADR-0007: Local JSON-RPC IPC For Controllers

- Status: accepted.
- Decision: use newline-delimited JSON-RPC over TCP at `127.0.0.1:45991`.
- Notes: named pipes stay deferred alternative. Stream Deck and UI mutations
  use this transport.

## ADR-0008: Single-Process Recording MVP

- Status: accepted.
- Decision: keep recording engine in `Monolith.exe` with strict library
  boundaries under `libs/`.
- Future: split into headless engine process after MVP stable.

## ADR-0009: SQLite Settings Store

- Status: accepted, supersedes original `config.json`/WinUI settings model.
- Decision:
  - Store live settings in `%LocalAppData%\Monolith\settings.db`.
  - Use `libs/storage` as generic top-level-section key/value store.
  - Keep `config/default-config.json` as default seed/fallback.
  - Import legacy `%LocalAppData%\Monolith\config.json` once when DB empty,
    then rename to `config.json.imported.bak`.
  - UI writes `settings.db` and calls engine IPC `reload_settings`.
- Notes: engine load/save logic stay in `settings_config.cpp`.

## ADR-0010: SQLite Clip Catalogs Beside Output Folders

- Status: accepted.
- Decision:
  - Store replay metadata in `<clips folder>\clips.db`.
  - Store manual recording metadata in `<recordings folder>\recs.db`.
  - Store thumbnails under `.thumbs\` in each output folder.
  - Engine be single writer. UI opens catalogs read-only.
  - Use WAL and `busy_timeout`.
  - Self-heal on startup: remove rows for missing media, regen missing
    thumbnails, import pre-existing media.
- Notes: superseded in part by ADR-0015 — UI-driven favorite/hashtag/title/
  rename/delete ops now write catalog directly from UI process
  (rusqlite, same WAL/busy_timeout discipline) so they work without engine
  running. `clip_regen_thumb` still go through engine IPC cuz thumbnail
  regen decodes frame via FFmpeg, which only engine links against.
  Engine stay writer too (own recording/replay/import paths); this
  be second writer under same WAL contract, not new single-writer.

## ADR-0011: Web UI Layer Exception

- Status: accepted in scope, shell impl superseded by ADR-0012.
- Decision:
  - No-browser rule applies to recording engine.
  - `app/desktop-ui` may use system WebView2 UI layer.
  - UI and engine talk through JSON-RPC.
  - Engine stay single writer for clip catalogs.
- Historical note: first planned impl used Deno Desktop. That
  shell replaced by Tauri cuz Deno Desktop no reliably show
  window.

## ADR-0012: Tauri v2/WebView2 Desktop UI Shell

- Status: accepted, frontend bundler superseded by ADR-0014, transport
  superseded by ADR-0015.
- Decision:
  - Use Tauri v2/WebView2 for `Monolith.UI.exe`.
  - Rust host lives in `app/desktop-ui/src-tauri`.
  - Preact frontend stays in `app/desktop-ui/ui`.
  - Deno was build-only for frontend bundle (now Vite, see ADR-0014).
  - `cargo build --release` creates `monolith_ui.exe`, copied to
    `<recorder-output>\ui\Monolith.UI.exe`.
  - Tauri CLI bundling not required; app ships as bare exe.
  - ~~Rust host runs loopback HTTP server so frontend keeps normal
    `/api`, `/media`, `/thumb`, static asset, and SSE contracts.~~ Replaced by
    native Tauri IPC, see ADR-0015.
  - `rusqlite` with bundled SQLite removes old first-run DLL issue.
- Notes:
  - Old WinUI and Deno Desktop hosts removed.
  - Installer ships `ui/Monolith.UI.exe`.

## ADR-0013: Settings And Clip Library Overhaul

- Status: accepted.
- Decision:
  - Schema version be `3`.
  - Encoder config be friendly: `device` (`gpu`/`cpu`), `codec`
    (`h264`/`h265`), `bitrate_kbps`, `fps`, `scaling_filter`, and
    `extra_ffmpeg_options`.
  - Concrete FFmpeg encoder resolved at runtime from capabilities.
  - Resolution be preset: `source`, `480p`, `720p`, `1080p`, `1440p`.
  - Replay memory budget and temp folder internal; memory budget fixed at
    512 MB.
  - Active-game timing tunables not user-facing. Engine cadence fixed at
    5s plus foreground-change fast scans.
  - Audio source volume applied in direct routes and `TrackMixer`.
  - Clip catalog has display `title` independent from filename.
  - IPC adds `clip_set_title`, `clip_regen_thumb`, and `clip_generation`.
  - UI host exposes SSE at `/api/events` for clip refresh.
  - UI uses native folder picker route instead of manual path entry.
  - Emoji-like structural UI icons replaced by inline Lucide-style icons in
    frontend.
- Notes: WebView2 multi-track playback depends on `HTMLVideoElement.audioTracks`;
  unavailable engines fall back to default track playback.

## ADR-0014: Vite Frontend Bundler (Supersedes Deno In ADR-0012)

- Status: accepted.
- Decision:
  - `app/desktop-ui` Preact frontend bundled with Vite
    (`vite.config.ts`), not Deno + esbuild.
  - `npm install && npm run build` replaces `deno run -A build.ts`.
  - CMake looks for `npm`/`npm.cmd` instead of `deno`/`deno.exe`; behavior on
    missing toolchain (warn and skip UI target) unchanged.
  - `tauri.conf.json` `beforeBuildCommand` updated for correctness even
    though actual build path (CMake -> npm run build -> plain
    `cargo build --release`) no invoke Tauri CLI and so never reads
    it.
- Notes: Deno stayed fine build-only tool, but standard npm/Vite
  pipeline be more conventional Tauri setup and removes second package
  ecosystem (Deno imports) alongside npm, which repo already needs for
  `plugins/stream-deck`.

## ADR-0015: Native Tauri IPC (Supersedes The Loopback HTTP Server In ADR-0012)

- Status: accepted.
- Decision:
  - `app/desktop-ui` frontend↔backend comms moved from `tiny_http`
    loopback HTTP server (`/api/*` REST, hand-rolled SSE at `/api/events`,
    Range-streamed `/media/*`/`/thumb/*`) to native Tauri v2 IPC.
  - Window loads bundled assets directly (`WebviewUrl::App("index.html")`)
    instead of navigating to `http://127.0.0.1:<port>/`.
  - Backend calls be `#[tauri::command]` functions in `src-tauri/src/
    commands.rs`, invoked from frontend with `invoke()`.
  - Live clip-list refresh uses `app.emit("clips", ())` from poll thread plus
    frontend `listen("clips", ...)`, replacing SSE stream.
  - Clip video/thumbnail playback uses Tauri asset protocol
    (`convertFileSrc()` over clip's absolute `video_path`/
    `thumbnail_path`, already present on `Clip` struct) instead of
    `/media`/`/thumb` routes. Requires `protocol-asset` Cargo feature on
    `tauri` dep and `app.security.assetProtocol.enable = true` in
    `tauri.conf.json`. Scope (re)computed from configured clip/
    recording output folders on startup and after every settings save
    (`src-tauri/src/asset_scope.rs`), since Windows Range-seek on `<video>`
    handled by asset protocol itself.
  - Window chrome (minimize/maximize/close/drag) moved from HTTP-routed
    `AppHandle` calls to frontend calling `@tauri-apps/api/window`
    (`getCurrentWindow()`) directly; no Rust-side window commands remain.
  - `tauri.conf.json`'s `security.csp` now explicit policy (was `null`,
    only safe for external http: origin) allowing `ipc:`/
    `http://ipc.localhost` and `asset:`/`http://asset.localhost`.
  - `src-tauri/capabilities/default.json` declares ACL for main
    window: `core:default` plus explicit `core:window:allow-minimize`/
    `allow-toggle-maximize`/`allow-close`/`allow-start-dragging`.
  - `server.rs` and `media.rs` deleted; `tiny_http`, `include_dir`, `url`,
    and `percent-encoding` dropped from `Cargo.toml`.
  - `engine_rpc.rs` (JSON-RPC over TCP to `127.0.0.1:45991`) unchanged —
    out of scope for this migration, still used by `plugins/stream-deck`.
- Notes: this closes gap identified in ADR-0010/ADR-0012 where UI
  mutations went through engine IPC even though UI process could write
  catalog directly (see ADR-0010 note); it also removes loopback HTTP
  server as attack surface and source of CSP friction on `asset:`/`ipc:`
  origins.

## ADR-0016: Multi-Client Local IPC Server (Backlog, Per-Connection Threads)

- Status: accepted; token-auth part reverted (see below).
- Decision:
  - `libs/ipc/ipc_server.cpp` spawns one thread per accepted connection
    (`accept_loop()` + `handle_client()` per socket) instead of handling
    single client at a time; `listen()` backlog raised from 1 to 8.
  - `status_fn`/`mutation_fn` callbacks passed to `ipc::start()` may be
    invoked concurrently from multiple client-handler threads and must be
    internally thread-safe (they already were: `handle_clip_mutation` opens
    fresh DB handle per call and only touches mutex-guarded globals).
- Reverted: per-request auth token (32-char random, written to
  `<app_data_dir>\ipc_token`) added then removed. It broke
  Tauri UI's recorder controls (save replay/start/stop recording all failed)
  and was never layer actually causing "Origin header is not a valid
  URL" / 500 errors on UI load, which come from separate WebView2/Tauri
  invoke bridge, not this TCP server. Server stays loopback-only
  (`127.0.0.1`) with no request-level auth.

## Open Decisions

- Final MP4 remux/finalization policy for interrupted recordings.
- GPU-resident encoder API shape and fallback contract.
- Future engine/UI process split boundary.