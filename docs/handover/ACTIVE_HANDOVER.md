# Active Handover

Updated: 2026-09-08

## Publication version bump

After the focused U6 test and final Terra-medium review passed, the modified components were bumped for publication: engine `1.6.2`, desktop UI `1.4.1`; updater remains unchanged. Windows CI and runtime validation remain required after push.

## Audit phase 2A — media timeline and disk retention (review pending)

Parent accepted phase 1 after independent review; its historical entry below is
preserved. Implemented this stage: **R2/R3/R4/T1/T2/T3/T4**, plus the specifically
approved backend-only disk-budget setting. Quota-interrupted partial edits were
inspected and continued; no model/tooling workaround, install, commit, push,
release or version bump. Existing DLL/phase-1 work and intentional file removals
were preserved.

- RAM snapshot selection is temporal before copying/mux; min/max stats no longer
  use deque endpoints; one common anchor preserves video composition/A/V offsets.
- Disk opens only on video keys, including after save; snapshots pin only their
  inputs, retention continues during slow save, and pins release after readers
  close but before callbacks. Exact owned files are deleted; failed unlink stays
  charged/retryable and blocks admission; cancel/clear/teardown join readers.
- **`replay_buffer.disk_budget_mb`** persists through native settings/default seed
  and live reload: integer **512..65536 MiB**, default **2048**, invalid/legacy
  values default safely. The **Settings UI control remains phase-5 work** by
  user decision. No frontend edits. Budget changes do not restart recording;
  physical eviction is on next admission/snapshot, not new message-loop I/O.
- Budget counts live+pinned encoded payload; 256 MiB/segment, 4096 records,
  512 MiB producer free-space reserve. Mux/index/allocator overhead and save
  output are extra. A pinned old snapshot can exceed a reduced budget temporarily.
  Drops/pressure/deletion failures have stats and explicit recorder logging.
- Trim rescales PTS/DTS/duration after header, preserves a common exact source
  anchor, and concatenates from actual original positions. `TrimResult` reports
  actual start/probed duration; recorder catalog/bookmarks use it and surface DB
  errors. Lossless remains keyframe-aware, NOT exact: reference-frame end
  extension and packet-boundary audio precision are documented in `report.md`.
- Fallback decodes preroll, filters/drains decoded frames, preserves fractional/
  VFR timestamps, incrementally muxes same-codec video and ALL original audio.
  Unsupported compatibility fails before replacing the original. Missing leading
  Matroska DTS use bounded lookahead from known input DTS/durations, not clamps.

Executed Linux commands (existing FFmpeg n9.0.1/dev libs, GCC 16.2.1):

```sh
cmake -S tests/phase2 -B /tmp/monolith-phase2-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/monolith-phase2-build --parallel
ctest --test-dir /tmp/monolith-phase2-build --output-on-failure
cmake -S tests/phase2 -B /tmp/monolith-phase2-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/monolith-phase2-asan --parallel
ctest --test-dir /tmp/monolith-phase2-asan --output-on-failure
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build --output-on-failure
```

Final results: phase 2 normal **2/2 PASS, 8.30s**; ASan/UBSan **2/2 PASS, 8.89s**
(no diagnostics); reused phase 1 **2/2 PASS, 67.35s**. `tests/phase2/README.md`
distinguishes the small Win32 path shim from real FFmpeg integration. Sixteen
trim combinations have independent ffprobe/frame-hash/audio-payload/offset
checks; additional coverage includes slow pinned save/cancellation/failed unlink,
non-key disk restart, budget parsing, and a 60-second >50 MiB CBR reencode.

Required next: independent retained reviewer, then Windows/MSVC/native settings
reload + file-locking/real-device tests. No Windows execution or performance
improvement is claimed. Full-budget saturation, HEVC/AV1 matrix and allocation/
thread-failure injection remain unexecuted. Filesystem replacement/temp-path and
DB/bookmark transaction/crash durability are **phase 2B**, not silently fixed.
R5 synchronous disk/encode work also remains. Pending audit IDs: V1–V8, R5–R12,
U1–U6, I3, A1–A7; do not start the next phase without parent authorization.

## Audit phase 1 — bounded replay and IPC (review pending)

Implemented only R1/I1/I2; original audit remains in `report.md` with a separate
phase-1 evidence appendix. Previous DLL changes and pre-existing roadmap deletion
were preserved; no commit/push/release or version bump. All other report IDs
(V1–V8, R2–R12, T1–T4, U1–U6, I3, A1–A7) remain pending.

- RAM replay: hard logical payload and 262144-packet caps; whole-GOP eviction,
  no audio/dependent frames before a key, rejection of oversized packets,
  steady-clock residence and minimum-DTS/watermark age enforcement. Invalid
  limits fail closed. A missing/oversized video packet invalidates the chain.
  Expiration is lazy on push/configure/stats/save, not a timer. Empty stats/save
  expose temporary replay unavailability. Clock skew can shorten replay; R2
  selection/reordering is still open.
- The 512 MiB setting bounds current-ring **payload**, not process RAM: a slow
  single-flight snapshot can pin another old cap, plus metadata/allocator/mux
  and unrelated engine memory. No Windows memory measurements were performed.
- IPC: 16 workers, 64 KiB lines, JSON depth 64, 30s first-byte partial-line and
  total send deadlines; **no idle timeout**, compatible with Stream Deck's 5s
  poll. Bounded accept polling reaps completed threads; lifecycle/vector/socket
  ownership prevents late registration/close-reuse races during shutdown.
  Thread creation failure cleanup is coded but not fault-injected. Callbacks
  remain synchronous and can delay stop; this is not a callback timeout fix.

Commands executed on Linux (GCC 16.2.1; existing nlohmann-json 3.12.0):

```sh
cmake -S tests/phase1 -B /tmp/monolith-phase1-build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build -V
cmake -S tests/phase1 -B /tmp/monolith-phase1-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/monolith-phase1-asan --parallel
ctest --test-dir /tmp/monolith-phase1-asan --output-on-failure
```

Both suites **2/2 PASS** (normal 67.34s, ASan/UBSan 69.67s, no sanitizer
findings). Initial strict build found a missing test initializer, corrected.
See `tests/phase1/README.md` for coverage and the Linux socket adapter limitation;
port 45991 must be free. No heavy dependencies installed.

Required before acceptance/release: independent parent review; MSVC Windows
build; actual Winsock shutdown/churn/short-send tests, Stream Deck + UI smoke,
Windows handle/private-byte soak, keyframe-starvation/oversize replay and slow
snapshot save with FFmpeg decode. Linux tests do not establish these Windows
results. Do not begin phase 2 without parent authorization.

## Session 2026-09-08 — native runtime DLL packaging fix

Hardened packaging against Windows startup failures caused by native dependencies (reported as missing `aom.dll`) being present in the CMake/vcpkg build output but omitted from shipped packages. The old installer and component-update rules selected DLLs by filename prefixes (`av*`, `sw*`, `lib*`, plus `sqlite3.dll`), which excluded dependencies whose names did not match that list.

- The incoming HEAD (`6232dc2`) already corrected `installer/monolith.iss` to package every `*.dll` emitted beside `Monolith.exe`, and the release workflow to copy every build-output `*.dll` into the engine component zip. These wildcard rules were verified, not introduced by this session.
- `scripts/verify-runtime-dlls.ps1` inspects the EXE/DLL files directly inside the supplied runtime directory and follows their transitive imports with `dumpbin /DEPENDENTS`, requiring imported vcpkg DLLs to be colocated while allowing system/API-set DLLs available on the CI host. Both Windows CI workflows run it on the root native payload; release CI runs it again on the staged engine component. This is dependency-name agnostic rather than an `aom.dll`-only assertion. It does not scan subdirectories: the separate `ui/` sidecar and dynamically loaded dependencies are outside this check; a clean-Windows smoke test remains required.
- No capture, encoding, or other performance behavior changed.

Static validation was run on Linux (`git diff --check` plus source assertions for wildcard packaging and workflow verifier calls). PowerShell, MSVC/vcpkg Windows build output, Inno Setup, and Windows runtime were unavailable here. Before release, require Windows CI green and smoke both a fresh installer and an engine component update on a clean Windows 11 machine.

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
Phase-2A accounting clarification: incremental mux bounds application packet
queues, not all library-owned MP4 indexes/Matroska cues; those metadata can grow
with output length. System FFmpeg libraries were not rebuilt with sanitizers.

## Phase 2B entry — concat continuity checkpoint (independent review pending)

Phase2A's prior acceptance was reopened by the requested continuous-clock test:
real AAC loss at a disk-segment B-frame boundary (source DTS5.952s skipped; next
output DTS6.016s), not just intentional repeated-fixture timestamp resets.
Fixed concat-only audio boundaries in `libs/encoding/trim.cpp`: internal ownership
uses common source segment timeline rather than a new video PTS trim per segment.
Standalone trim, actual TrimResult and multitrack/video-reference behavior remain.
New 21s fractional/VFR two-audio fixture asserts contiguous source payloads,
strict DTS, <=2ms interval/intertrack-offset rounding and bounded tail omission;
also exercises mid-GOP save and next-key restart after clearing retained history.

Final commands: build + ctest in `/tmp/monolith-phase2-build` (2/2,11.71s),
`/tmp/monolith-phase2-asan` (2/2,11.34s, no diagnostics), and
`/tmp/monolith-phase1-build` (2/2,67.51s). Linux only; no Windows/Rust build.
See append-only report.md checkpoint for exact commands and initial failure.

Supervisor explicitly stopped expansion here pending LOW re-review. R7/R8/U6
remain unimplemented. Approved subsequent architecture: engine-only delete/rename
RPC (engine-off reads allowed, mutations actionable failure), engine-owned
recovery journal, bounded owned recording control/completion worker and durable
per-session bookmarks/drain. Inspect all Rust writer/reconcile bypasses first.
No partial journal/worker/routing changes introduced. Full mux writer/backpressure
remains R5 phase3; requested 2B failure/restart tests remain pending. Preserve
existing DLL work and intentional deletions. Phase5 disk-budget UI still pending
(default persisted budget 2048MiB). No commits/releases/version bumps.

## Phase 2B U6 — in progress, not ready for review

Delete/rename Tauri commands have been routed to engine RPC, preserving offline
read-only browsing and returning the connection failure rather than a direct
Rust fallback. `storage.cpp` has an operation-specific journal/quarantine
attempt with startup recovery. Do **not** accept it yet: no production fault
tests or Windows build were run and durable file identity checks are incomplete.
The old `file_mutation.h` remains an unused scaffold. R7, R8 and trim atomic
replacement are still open.

## U6 pragmatic durability (current session)

Delete/rename UI calls now require the running recorder and route over existing JSON-RPC; no UI SQLite fallback exists. Storage uses a validated operation journal and no-overwrite moves, delete quarantine, startup recovery, and a catalog-keyed Windows mutex. Focused Linux storage shim coverage exercised normal mutation, prepared delete/rename recovery, committed delete cleanup, ambiguous state rejection, and invalid journal rejection; phase2 tests and offline Cargo check also passed.

Residual work: this does not prove filesystem/SQLite atomicity, stable file identity, reparse safety, Windows runtime/cross-process behavior, or a comprehensive fault matrix. R7/R8 remain pending.
