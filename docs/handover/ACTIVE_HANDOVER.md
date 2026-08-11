# Active Handover

Updated: 2026-08-11

## Session 2026-08-11 — Vice feature fusion: quick trim, bookmarks, replay storage RAM|Disk, AV1, collections

Executed the full plan at `local://monolith-vice-fusion-plan.md` (engine + IPC + storage + Rust + Preact UI). UI `npm install` + `npx tsc --noEmit` + `npx vite build` all GREEN on this machine. Native side is static-verified only — **no C++ build (MSVC/vcpkg absent) and no cargo on this host; Windows CI + real-machine build is the first step before release**.

Phase 1 — Engine foundation (native, static-verified):

- IPC (`libs/ipc`): `ClipMutation{start,end}`, `AddBookmarkFn` + `start()` 5th param, `"recording_add_bookmark"` handled on the IPC thread (bookmark timestamp accuracy), `"clip_trim"` whitelisted.
- Storage (`libs/storage`): `set_duration`, `video_file_for(id)`, `set_bookmark_time`, bookmark CRUD (INSERT OR REPLACE, `clip_exists` guard, `PRIMARY KEY(clip_id,seq)`), `remove_clip` cascade.
- Engine (`app/recorder/src/main.cpp`): recording-clock globals + `recording_elapsed_seconds()` (+ pause/resume), `PendingBookmark{time,label,color}` queue flushed on catalog with `catalog_clip(path, source, dur, on_cataloged)`; `CMD_ADD_BOOKMARK = 1008` with hotkey (default `Ctrl+Shift+F12`) + tray entry + wnd_proc dispatch; `add_bookmark_now` errors on "not recording"/"recording is paused". Fixed: auto-record stop path lacked the flush closure — now builds `folder = parent_path(path)` and passes `[folder](int64_t id){ flush_pending_bookmarks(id, folder); }` (same as the manual-stop path).

Phase 2 — Settings (`settings_config.{h,cpp}`, default-config.json): `hotkey_add_bookmark` (`Ctrl+Shift+F12`), `replay_buffer_storage` (`"ram"|"disk"`), codec accepts `"av1"`. 5-entry hotkey collision table in C++ and Rust (frontend auto-picks from its hotkey field list). Note: `settings.db` stores whole top-level sections as JSON blobs — all new sub-keys round-trip with zero Rust changes.

Phase 3 — Trim lib (`libs/encoding/trim.{h,cpp}`): ffmpeg `-c copy` remux window with keyframe-backward seek (playback-safe pre-roll), per-stream pts/dts re-anchor + `offset_sec` shift, trailer finalize, `concat_clip_segments` for multi-segment; lossless→reencode fallback. `main.cpp::trim_clip` validates, writes `.trimming<ext>` temp, `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`, `set_duration`, bookmark re-time (`set_bookmark_time(t-start)`/`remove_bookmark`), thumbnail regen + `g_clip_generation` bump. Fixed latent: output pb leak on error paths (`OutFmt` dtor `avio_closep` + `free_fmt` lambda in reencode path). CMakeLists already included `trim.cpp`.

Phase 4 — Replay-buffer disk backend: new `libs/disk-segments/{disk_segments.h,cpp,CMakeLists.txt}`; `replay_buffer` `Impl::disk` unique_ptr, `configure()` routes ram↔disk (forwards vsp/audio params), `push/clear/save_clip/stats` route; `main.cpp` `apply_runtime_settings` sets `rbcfg.storage` + `rbcfg.segment_dir = temp_directory`.

Phase 5 — AV1 (`libs/encoding`): `VideoCodec::AV1`, `resolve_video_encoder("h264"|"h265"|"av1")`, candidates `av1_nvenc/av1_amf/av1_qsv/libaom-av1`; libaom options `cpu-used=8` + `rc-end-usage=cbr` only (never preset/tune — unknown option fails `avcodec_open2`); `mux_common.cpp` AV1→`AV_CODEC_ID_AV1`; `vcpkg.json` ffmpeg features += libaom.

Phase 6 — Rust backend (`src-tauri/src`): `clip_catalog.rs` bookmark CRUD + `BOOKMARK_DDL` + `clip_by_id` + `clip_select_sql`/`map_clip_row` refactor; new `collections.rs` (single global `%LocalAppData%\Monolith\collections.db`, mixes replay+manual, prune-on-read, ISO-8601 `created_at_utc`); `commands.rs` registered 13 new commands (`clip_trim`, `recording_add_bookmark`, bookmark CRUD, collection CRUD + membership); `main.rs` invoke_handler verified — all 37 entries exist.

Phase 7 — UI (`app/desktop-ui/ui`): `lib/api.ts` `clipApi.{trim,listBookmarks,addBookmark,updateBookmark,deleteBookmark,recordingAddBookmark}` + `collectionsApi.{list,create,rename,remove,clips,addClip,removeClip}` + `BookmarkRow`/`CollectionSummary` (color swatch, no cover thumb); `icons.tsx` += `scissors/bookmark/album/plus-circle/flag`. Settings popup: 5th hotkey field, Replay Storage segmented (Ram|Disk), AV1 codec option + encoder label. Detail view: trim mode with draggable/keyboard handles (min span 0.5s, Esc cancels, Apply → `clip_trim`), bookmark markers on the scrubber, bookmark list with edit/delete + 8-swatch palette, add-bookmark button gated to `clip.source === "manual"`. Collections: sidebar 3rd nav (album icon), `collections-view` (grid + create/rename/delete), `collection-detail` (self-contained: its own DetailView/Fullscreen/ctx-menu — collections have no cover image, cards use color swatch + album icon), `collection-picker` (from clip context menu "Add to collection…" + detail-view album button), titlebar `Collections · <name>`.

Verification on this machine (all done this session):

- **cargo build (Linux) green — MUST use `--features custom-protocol`**: `cargo build --release --features custom-protocol` in `src-tauri` → `target/release/monolith_ui` (24 MB, ELF x86-64, stripped) and the UI **loads** (verified live). Without the feature the release binary falls back to `devUrl` (`http://localhost:1420`) — WebView shows a white screen with "Could not connect to localhost: Connection refused" (that error is the dev server, NOT the engine's 45991). The plain `cargo build --release` in this file's older sessions and the Open-items command below had this latent trap; on Windows too, production builds must pass `--features custom-protocol` (or use `npm run tauri build`, which enables it via the CLI). Two more cross-platform fixes: `src-tauri/icons/icon.png` was missing (tauri-build requires it on Linux; Windows uses the ico) — copied from `app/assets/icon.png`; `exe_icon.rs` (SHDefExtractIconW) is Windows-only but was compiled unconditionally — `mod exe_icon`, the `commands::exe_icon` import/fn and its invoke_handler entry are now `#[cfg(target_os = "windows")]` (leaves 2 dead-code warnings in `game_catalog.rs` on Linux only — reachable on Windows, intentional).
- `npm install` (91 packages), `npx tsc --noEmit` → 0 errors, `npx vite build` → OK (37 modules, 109 kB JS / 35 kB CSS).
- Fixed one pre-existing UI type bug found by tsc: `saveCapturedThumb` went through `ok()` which discarded the `thumbnail_file` return value while `clip-card.tsx` reads `res.thumbnail_file` — now returns the invoke payload on the envelope.
- Cross-stack parity greps: `clip_trim`/`recording_add_bookmark` present in ipc_server.h/.cpp + main.cpp + commands.rs + api.ts; `catalog_clip` def + 3 callsites (auto-stop/replay/manual-stop); 5 hotkey registrations; bookmark DDL C++/Rust aligned; `replay_buffer_storage`/`hotkey_add_bookmark` wired in settings C++ + engine; all 13 commands in `commands.rs` and invoke_handler.

Open items — Windows-only, cannot run here:

1. Build: `cmake --build build --config Release` (vcpkg add `libaom` via ffmpeg features) + `cargo build --release --features custom-protocol --manifest-path src-tauri\Cargo.toml` (the feature is required — without it the webview falls back to devUrl and shows a white screen).
2. Runtime smoke: trim a manual clip (verify keyframe pre-roll quality + duration/timebase), add bookmark mid-recording via `Ctrl+Shift+F12` and via tray, replay-buffer storage=Disk (segment files under temp dir), AV1 encoding path (`libaom-av1` or hw AV1), collections create/rename/delete/add-remove clip (replay AND manual), bookmark re-time after trim.
3. Confirm `%LocalAppData%\Monolith\collections.db` schema on first collections use.

Previous session entry below (2026-07-12) unchanged.

## Session 2026-07-12 (b) — border removal + shared-exe game resolution + titlebar crash fix

Native engine NOT built locally (vcpkg absent) — runtime unverified, need CI + real-machine smoke. UI `vite build` green.

- **Capture border**: always suppressed. `main.cpp` force `options.show_border = false` (ignore `g_settings.show_capture_border`); log branch simplified. UI toggle removed from `settings-popup.tsx`. Config key `show_capture_border` left inert (load/save kept for `settings.db` back-compat).
- **Shared-executable game resolution** (root cause of "Minecraft detected as Spiral Knights"): DB mapped ONE game per exe and dropped Discord's `>` prefix.
  - `libs/gamelist`: `GameMap` is now `unordered_map<exe, vector<GameEntry>>` (`GameList`) — every game sharing an exe is kept. `basename_lower` strips a leading `>` (Discord marks child-process exes like `>javaw.exe` for Minecraft, which previously never matched the real `javaw.exe`). `parse_detectables` appends + dedups by `discord_app_id`. SQLite schema v2: composite PK `(exe_lower, discord_app_id)`; `open_db` migrates via `meta.schema_version` (drops stale `games`, re-syncs). `lookup` now returns a `GameList`.
  - `libs/audio` `detect_game_candidates`: candidate gets provisional identity = first game for its exe; after the window pass, if the exe has >1 game AND a window title exists, pick the game whose name matches the title (`title_matches_db_name`: whole-name or alnum token len>=3, case-insensitive) and set `display_name`/`discord_app_id`/`title_matches_db`. No match → provisional stands (normal fallback). Single game → nothing to resolve.
  - Reverted the earlier same-session `ambiguous_suppressed_pids` approach (wrong model: it only fired with two running same-exe processes and compared against the single stored name).
- **Titlebar crash** (`titlebar.tsx`): `subject` rendered the whole `runtime.active_game` object → "Objects are not valid as a child" + Titlebar re-render loop that hung the clip library (Preact injects `__,__b,__i,__u` treating the object as a vnode). Now renders `appLabel(active_game.display_name, active_game.process_name)`.

Open items: CI build + real-game smoke — verify Minecraft (`javaw.exe`, title "Minecraft* …") now resolves to Minecraft not Spiral Knights; confirm first post-upgrade launch rebuilds `game_list.db` (schema v2) and re-syncs. NVENC/QuickSync codec wiring still pending (extend `libs/encoding` + fallback cascade, NOT libobs — hw encoders already in FFmpeg vcpkg feature set).

## Session 2026-07-12 — DB-gated detection + auto-record features

Landed 4 commits on `main` (Windows CI green; native engine NOT built locally — vcpkg absent — so runtime unverified, need real-machine smoke test).

- **Bug fixes**: topbar exe-icon call missing `processName` arg (`titlebar.tsx`) — fixed; topbar now show DB `display_name` verbatim (no `prettyAppName`). Thumbnail failures were silent (verbose logging off by default) and frontend `<video>` fallback cannot decode `.mkv` in WebView2 — added always-on `logging::log_error` channel, frontend now hand off to engine FFmpeg regenerator, and `clip_regen_thumb` bump `clip_generation` so grid reload.
- **New `libs/gamelist`**: recorder-owned SQLite cache of Discord detectable list (`https://discord.com/api/v10/applications/detectable`), fetched via WinHTTP on worker thread (startup + 72h + refetch-if-missing), lock-free snapshot. `%LocalAppData%\Monolith\game_list.db`.
- **DB-gated detection** (`libs/audio`): `detect_game_candidates()` enumerate all processes (Toolhelp), gate on game-list membership; heuristic scoring + foreground-fallback removed. `detect_active_game()` now return best DB-matched candidate.
- **Engine state machine** (`main.cpp`): 3 s cadence; `poll_active_game()` resolve effective target (user selection by exe, else most-recently-focused) and publish `game_candidates[]` + `selected_game_pid`; `evaluate_capture_mode()` run auto-record (startup 60 s focus grace, auto-next-on-close, manual switch). New `set_selected_game` IPC command + `SelectGameFn`. `capture_mode.clip_without_game` setting; idle-timeout now honored. Pacer hold last frame while captured game window minimized (game_only only). Screen mode auto-follow game's monitor unless pinned.
- **UI**: topbar multi-game picker + Clip-Without-Game toggle; Settings > Game page toggles (mode, auto-record, clip-without-game, idle timeout).

Open items next session:
- Runtime smoke test on real machine with real game (detection match, auto-record start/stop/switch, frozen frame, thumbnail gen) — none verifiable from CI compile alone.
- Confirm live Discord detectable JSON shape match parser in `gamelist.cpp` (`[{id, name, executables:[{name, os, is_launcher}]}]`).
- Rust `game_catalog.db` still hold artwork only; `discord_app_id` from new gamelist now on candidates but not yet threaded into clip rows for artwork.

Updated (previous): 2026-07-10

## Product Summary

Monolith = Windows 11 clipping/recording app. Run in background, expose tray commands + hotkeys, keep rolling replay buffer, support manual recording, has Tauri/Preact desktop UI for clip library/settings, include Stream Deck controller plugin.

## Current Phase

Repo in MVP-hardening + release-readiness, not early prototype.

Implemented:

- Win32 tray app and hotkeys.
- WGC video capture.
- WASAPI audio capture, including process-loopback where available.
- FFmpeg encode/mux.
- Replay buffer and manual recording.
- SQLite clip catalogs and thumbnails.
- SQLite `settings.db`.
- Tauri v2/WebView2 UI sidecar.
- Preact clip library and settings popup.
- JSON-RPC IPC on `127.0.0.1:45991`.
- Stream Deck TypeScript plugin actions.
- Inno Setup installer and WinSparkle appcast tooling.

Still open:

- Runtime soak tests.
- Automated test harness.
- GPU-resident or lower-copy video path.
- First public release setup.
- Clean-VM installer/update verification.

## Locked Architecture

- Custom native Windows recorder, not OBS fork.
- Single-process recording MVP with strict `libs/` boundaries.
- Future headless engine process split is deferred.
- C++23 + CMake + vcpkg for native code.
- Tauri v2/WebView2 is allowed only in `app/desktop-ui`.
- Deno is build-only for frontend bundling.
- Settings store of record is `settings.db`.
- Engine is the single writer for clip catalogs.
- Stream Deck plugin is a remote controller only.

## Build Commands

Root native build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release --parallel
```

UI build:

```powershell
cd app\desktop-ui
deno run -A build.ts
cargo build --release --manifest-path src-tauri\Cargo.toml
```

Stream Deck plugin:

```powershell
cd plugins\stream-deck
npm run build
npm run package
```

## Current Runtime Paths

- Settings: `%LocalAppData%\Monolith\settings.db`.
- Runtime status: `%LocalAppData%\Monolith\runtime-status.json`.
- Log: `%LocalAppData%\Monolith\monolith.log`.
- Clips: `Videos\Monolith\Clips`.
- Recordings: `Videos\Monolith\Recordings`.
- Installed app: `%LocalAppData%\Programs\Monolith`.

## IPC

Transport: newline-delimited JSON-RPC over TCP on `127.0.0.1:45991`.

Commands:

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

`get_status` return `clip_generation`; UI host use it for live clip-grid refresh.

## Important Current Facts

- Old WinUI settings sidecar gone.
- Deno Desktop shell gone.
- `Monolith.Settings.exe` stale doc only; current sidecar = `Monolith.UI.exe`.
- Live settings not `config.json`. `config.json` = legacy migration input.
- Active Game timing tunables scrubbed from settings; engine poll every 5 s + fast-scan foreground changes.
- Audio tracks with multiple sources use `TrackMixer`; single-source tracks feed encoders directly.
- Replay memory budget fixed at 512 MB.

## Recent Documentation Pass

This session updated all Markdown docs to match current code:

- Root agent guide.
- Codebase report.
- Architecture.
- Decisions.
- Development rules.
- Roadmap.
- Releasing.
- Handover.
- Scripts README.
- Desktop UI README.
- Stream Deck docs.
- Stream Deck image README.
- Research notes under `research_codebase/`.

## Verification Status

Docs updated from source inspection. Build/test verification should run after doc pass:

```powershell
cmake --build build --config Release --parallel
cd app\desktop-ui
deno run -A build.ts
cargo build --release --manifest-path src-tauri\Cargo.toml
cd ..\..\plugins\stream-deck
npm run build
```

## Next Steps

1. Run verification commands above.
2. Add tests for settings migration + IPC request handling.
3. Runtime-test replay + manual recording with default audio.
4. Runtime-test custom audio with multiple sources on one track.
5. Runtime-test Active Game switching.
6. Start GPU downscale-before-readback spike.
7. Complete first public release setup from `docs/RELEASING.md`.

## Open Risks

- CPU/RAM cost from WGC BGRA CPU readback + software conversion.
- Long-session A/V sync not fully validated.
- Process-loopback + active-game detection best-effort on Windows.
- Update path not fully verifiable until first public release exists.
- No broad automated test suite yet.

## desktop-ui: UI redesign pass (UI_Todo.md)

Executed 9-task redesign plan in `UI_Todo.md` to bring `app/desktop-ui` closer to feature parity/polish with competitors while keeping `PRODUCT.md`'s "quiet, precise, native" identity.

Done:

- New `--accent`/`--accent-hi`/`--accent-soft`/`--accent-ink` design tokens (desaturated lavender-ice) applied to all interactive/active/selected UI (`.btn-primary`, `.toggle.on`, `.seg.active`, `.side-item.active`, `.settings-tab.active`, input/select focus, `.rec-dot.clip`). Red stays recording-only, gold stays favorites-only — verified no overlap. `PRODUCT.md` "Brand Personality" + Design Principle 3 updated to document third accent's single meaning.
- Titlebar status cluster flex ratios fixed (`.tb-brand` no longer grow to consume space, `.tb-status` anchored after it) + persistent chip background on `.capture-feed`.
- Settings popup: grouped nav (Recording / Output / Advanced sections), content-driven modal height (was fixed 640px regardless of content), + real "About" section on General page using Tauri's `getVersion()` (previously that page near-empty).
- Library clip cards: persistent (non-hover) action row — favorite, open containing folder, delete — reusing app's existing `ConfirmDialog` flow for delete rather than native `confirm()`. Added new Tauri command `reveal_in_explorer` (`explorer /select,<path>`) since no folder-reveal capability existed anywhere before this session.
- Typography coherence pass: weight/tracking bumps on active nav/tab states + section/card titles for clearer hierarchy. No new font bundled.

Intentionally skipped (documented in `UI_Todo.md`'s own terms — don't fake data or add scope app doesn't support):

- **Sidebar storage/space indicator** — `RuntimeStatus` has no disk/storage fields; adding needs new backend work (`settings_store.rs` + `commands.rs` + `settings-api.ts`) out of scope for UI pass.
- **Bundled display font** — marked optional in spec; needs separate licensing/packaging decision.

Build verified this session: `npm run build` (Vite) + `cargo build --release --manifest-path src-tauri\Cargo.toml` both succeed. Fixed one unrelated pre-existing compile error found along the way (`game_catalog.rs::resolve_artwork` was missing `last_updated` field after Discord cache redesign in prior commit).

Not done: manual runtime smoke test (build machine only, no interactive session) — verify titlebar/sidebar/library grid/settings modal/card actions visually before shipping, per `CLAUDE.md`'s manual runtime smoke checklist.

## desktop-ui: clip card/detail/settings follow-up pass

Follow-up session on top of redesign pass above, addressing user feedback:

- Clip card: removed delete + "open containing folder" buttons entirely (`.card-actions`/`.card-act` gone). Reveal-in-explorer moved into detail view: clicking clip title/name there now call `clipApi.revealInExplorer(clip)` instead of just being a label.
- Detail view player: added fullscreen button next to volume slider (`onFullscreen(clip)`, wired through `app.tsx` into existing `Fullscreen` component).
- Clip card compact meta row reordered to `[game/source icon] · date · size` (previously size/date/icon), matching requested "Game, Date, Size" order.
- Settings popup no longer resize when switching categories: `.settings` changed from `height: auto; max-height: min(640px, 90vh)` to fixed `height: min(640px, 90vh)`. `.settings-body`'s existing `overflow-y: auto` now does all scrolling; sparse page just leaves empty space instead of shrinking whole popup.
- Audio settings: new "Track Layout" section/toggle (`audio.track_layout`: `"single" | "separate"`). Microphone always keep own track; every other source (game desktop audio + other apps) either share track 1 or get own free track (3-6). Purely frontend — confirmed via reading `settings_config.h`/`encoding.h` that C++ engine already mix whatever `tracks: []` values a source is given, and via `settings_store.rs` that config round-trips as untyped JSON blob, so no backend/schema changes needed.
- Audio settings "Other sources" now re-poll `getRuntimeStatus()` every 5s while Settings popup open, so newly-detected audio-producing apps show up without closing/reopening Settings. Note: refresh happens at popup-mount level + 5s poll, not as independent re-fetch on every tab switch within one already-open popup session — in practice 5s poll means data never more than 5s stale regardless of active tab, but flagging distinction since not asked literally.

Build verified this session: `npm run build` (Vite) + `cargo build --release --manifest-path src-tauri\Cargo.toml` both succeed, no compile errors.

Not done: manual runtime smoke test of these specific changes (build machine only) — verify card layout, detail-view name-click/fullscreen button, settings popup fixed sizing across all category pages, and audio track layout toggle actually changing recorded track assignment, before shipping.

No git commit/push made — awaiting explicit user confirmation per standing instruction.