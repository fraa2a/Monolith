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
- Notes: superseded in part by ADR-0015 - UI-driven favorite/hashtag/title/
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
  - `engine_rpc.rs` (JSON-RPC over TCP to `127.0.0.1:45991`) unchanged -
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

## ADR-0017: Vice Feature Fusion - Collections, Bookmarks, Engine-Side Trim, Disk Replay, AV1

- Status: accepted (2026-08-11).
- Decision:
  - **Collections in AppData**: single global `%LocalAppData%\Monolith\collections.db`
    (via `paths.rs`), tables `collections` + `collection_clips(collection_id,
    source, clip_id)` with PK de-dup. Collections span both `replay` and
    `manual` catalogs and survive output-folder moves. UI (Rust/rusqlite,
    WAL/busy_timeout like ADR-0015) is the writer; membership rows whose
    clip vanished are pruned on read, so engine/Stream Deck clip deletion
    never leaves orphans.
  - **Bookmarks live in clip catalogs**: `clip_bookmarks(clip_id, seq,
    time_seconds, label, color)` in `recs.db` (and `clips.db` for
    uniformity), engine-side schema with `IF NOT EXISTS` so existing DBs
    migrate transparently. During recording bookmarks accumulate in the
    engine (`recording_elapsed_seconds()` clock excludes pause) and are
    written to the catalog row at stop via `catalog_clip`'s `on_cataloged`
    callback. `recording_add_bookmark` is handled on the IPC thread because
    timestamp accuracy matters. Trim re-times bookmarks (`−start`, out-of-range
    dropped).
  - **Trim is engine-side, in-place**: FFmpeg remux lives only in the
    recorder process; new `clip_trim` JSON-RPC mutation (same channel as
    `clip_regen_thumb`) copies packets losslessly (`-c copy` semantics,
    keyframe-backward seek keeps pre-roll), writes `<file>.trimming.<ext>`,
    then `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` - file replaced
    in place like Vice, original intact on any failure. Re-encode is a
    safety-net fallback only. `duration_seconds` re-probed, thumbnail
    regenerated, `clip_generation` bumped so the UI grid refreshes.
  - **Replay buffer RAM|Disk**: new `storage` config field
    (`"ram"|"disk"`); RAM = existing 512 MB ring (unchanged); Disk = new
    `DiskSegmentBuffer` in `libs/replay-buffer` muxing ~2 s `.mkv` segments
    into a scratch dir (`%LocalAppData%\Monolith\clip-buffer-segments`),
    cleaned on startup and on RAM↔Disk switch; `save_clip` trims the first
    segment + concatenates the rest via `concat_clip_segments` (shared with
    trim), temp + atomic rename so a failed concat leaves no partial file.
  - **AV1**: `VideoCodec::AV1` + `resolve_video_encoder("av1")` cascade
    `av1_nvenc → av1_amf → av1_qsv → libaom-av1` (vcpkg ffmpeg feature
    `libaom`, not GPL). libaom gets only `cpu-used=8` + `rc-end-usage=cbr`
    (no preset/tune - unknown options fail `avcodec_open2`).
- Notes: no-10-bit, no-CRF/CQ, no timeline editor (out of scope). Manual
  recordings stay always-on-disk. UI text stays English.

## ADR-0018: Component Self-Updater (Supersedes WinSparkle Full-Installer Updates)

- Date: 2026-08-16
- Status: accepted
- Context: WinSparkle compared only `Monolith.exe`'s FileVersion against a
  single appcast version and re-ran the full Inno installer (engine + FFmpeg
  DLLs + UI) for every update. A UI-only change therefore required bumping
  the engine version/tag, and clients re-downloaded ~everything for nothing.
  The update UX was WinSparkle's stock native dialog.
- Decision: a dedicated Tauri v2 process - `Updater.exe` (`app/updater`,
  deployed at the app root next to `Monolith.exe`) - owns the entire flow:
  - Feed: `update-manifest.json` on the stable
    `releases/latest/download/` URL with per-component entries
    (version / url / size / sha256 / EdDSA signature).
  - Independent component versions: engine from root `CMakeLists.txt`
    `project(VERSION)`, ui and updater from their `tauri.conf.json`. The git
    tag only names the release and versions the full installer.
  - Download only what changed; verify sha256 + Ed25519 (same key pair as
    the WinSparkle era, CI secret `WINSPARKLE_ED_PRIVATE_KEY`) before apply.
  - Apply via the rename-to-`.old` dance (works for loaded DLLs and running
    images): ui first (engine closes the UI over `update_close_ui`), then
    engine (`update_engine_exit` → graceful WM_CLOSE, swap, relaunch), the
    updater itself last (self-swap). `*.old` swept at next engine start.
  - UI: borderless WebView2 window (Preact, main app's design tokens) with
    per-component cards, live progress and a bottom "buffer bar".
  - Recording guard: Update-now is disabled while the engine reports
    `recording` (an engine update restarts the process).
- Migration: `appcast.xml` keeps being generated every release so legacy
  WinSparkle installs roll forward once through the full installer and land
  on the component-updater build.
- Consequences: WinSparkle dependency dropped (vcpkg, DLL, updater.cpp
  wrapper). Two new JSON-RPC methods on the engine (`update_close_ui`,
  `update_engine_exit`). `get_status` now also reports the engine version
  (Settings shows interface + engine versions side by side). No binary
  delta-patching (component granularity only) and no download resume -
  retry re-downloads; both are accepted v1 limitations.

## Open Decisions

- Final MP4 remux/finalization policy for interrupted recordings.
- GPU-resident encoder API shape and fallback contract.
- Future engine/UI process split boundary.

## ADR-0019: App-local Windows runtime closure

Date: 2026-10-08

The native recorder and vcpkg dependencies retain dynamic MSVC linkage. CMake copies the redistributable runtime beside the engine. Rust UI and updater binaries use a static CRT, matching their independent single-executable component payloads.

The verifier scans the root and sidecars, requires imports beside each binary and never treats VC runtime DLLs installed on a CI host as Windows prerequisites. All component staging directories are checked before packaging. Dynamically loaded driver/WebView2 dependencies and actual loader behavior require a clean Windows 11 smoke test.

## ADR-0020: Audit bounds and error propagation

Date: 2026-10-08

Collection hydration distinguishes unavailable catalogs from absent clips, loads IDs in batches, and prunes only after all required catalogs are readable. Bookmark sequence allocation uses an immediate transaction. Existing direct UI metadata writes remain a documented single-writer violation.

Both Rust engine clients require bounded JSON-RPC response envelopes tied to the request ID. Updater components require validated size, SHA256 and Ed25519 metadata; unsigned components have no bypass. Archive extraction is a separate module with Windows path/link/resource limits. Per-file replacement restoration does not provide component-wide rollback.

Manual recording uses one presentation origin across all streams to preserve composition and intertrack offsets. Failed packet/finalization output is not returned for cataloging. Synchronous recording I/O and pause/resume continuity remain follow-up work.

## ADR-0021: Recoverable component installation

Date: 2026-10-08

Replace the independent component swaps with one app-local transaction containing every selected payload file and the final component metadata. Preparation copies and syncs the entire plan before stopping the engine. A schema-versioned journal stores old/new hashes and a prepared/committed/rolled-back phase. Files are parked and placed with Windows `MoveFileExW` using write-through; prepared journals restore the previous plan, while committed journals retain the new files. Journal replacement uses a synced temporary file.

Recover before querying installed versions. Preserve unexpectedly changed files and surface recovery errors. Keep transaction backups until commit or complete rollback; retry cleanup if loaded images prevent removal. Read installed updater version resources and report engine spawn errors. Do not start a restored engine while recovery is still pending.

This provides recoverable disk installation, not instantaneous multi-file visibility or certified power-loss durability. A missing updater executable between its two moves can require a retained updater/reinstallation to invoke recovery. Stable file identity, concurrent external writers, engine readiness and clean Windows installer execution remain separate work. See `docs/audit/2026-10-08-followup.md` for test evidence.

## ADR-0022: Owned recording work and stable media identity

Date: 2026-10-09

The engine remains the only media/catalog writer. Persistent catalog and clip UIDs guard identity; media revisions guard duration, thumbnail and timeline updates. Collections quarantine ambiguous legacy membership rather than attaching it to reused IDs. Read-only UI hydration uses one settings-folder snapshot for both database and media paths.

Trim uses catalog mutation locking and a recoverable operation journal covering media, metadata and bookmark retiming. Reconciliation preserves unavailable rows and excludes owned/temporary media. Publication ownership spans catalog insertion and final bookmark batches, rejecting competing destructive edits until complete.

Shared capture epochs, bounded encoded-write jobs, an owned lifecycle worker and session bookmark recovery batches replace global session state and synchronous producer writes. Optional audio arriving after the mux header waits for the next session. Failed finalization preserves valid partial media, exposes an error and blocks a new session until retained work is resolved. Replay callbacks, catalog tasks and logging drain before teardown.

The UI stages thumbnail bytes in a fixed upload directory and sends an opaque token over RPC. The engine validates size, PNG signature, identity and revision before publication. Media reads use a bounded custom protocol checking current canonical roots rather than an accumulating asset scope.

This preserves the native engine and Tauri sidecars. Queue bounds and recovery journals do not imply unlimited throughput, instantaneous multi-resource atomicity or certified power-loss durability.

## ADR-0023: Signed release identity and stable updater recovery

Date: 2026-10-09

Retain legacy ZIP signatures and field compatibility. Add a separate Ed25519 signature over exact UTF-8 `monolith-component-v1\n{key}\n{version}\n{size}\n{lowercase_sha256}\n`. New clients require the identity signature, validate payload version resources and reject downgrades. Release generation without a valid matching key fails; verification streams bounded chunks and observes cancellation.

Sync a stable updater recovery copy outside the replacement plan before journaling. The engine detects pending recovery and launches that entry. Retain actual engine process handles, resolve executable path aliases consistently, and require version readiness from a live restarted child. Serialize cancellation against Applying and order snapshots/events using monotonic revisions.

Old clients accept the compatible manifest, but their first upgrade still runs old transaction code. New recovery cannot fix an interruption before the stable entry has been installed; reinstall remains the migration fallback. Clean Windows installation and power-loss tests remain release requirements.
