# Active Handover

Updated: 2026-07-15

## Session 2026-07-15 — external ffmpeg.exe refactor (in progress, branch)

Driven by a comparison with the Record-able Minecraft mod (see `REPORT.md`).
Native engine NOT built locally (vcpkg absent) — CI is the only compile check;
runtime a/v behaviour needs a real-machine smoke test.

**Landed on `main` (Windows CI green):**
- Resolution UI simplified to mod-style presets (native/480p/720p/1080p/1440p),
  scaling-filter selector removed, scaler fixed to bilinear engine-side.
- Encoder section shows the resolved encoder ("Encoding with: <vendor> · <codec>")
  via a new `encoderLabel()` helper instead of a bare codec / buried help text.
- `libs/encoding/ffmpeg_process.{h,cpp}`: locate/launch ffmpeg.exe (no libav*),
  `FfmpegProcess` (stdin frame pipe + drained stderr), `run_ffmpeg_capture`,
  `ffmpeg_available_encoders`/`ffmpeg_resolve_encoder`. Setting `ffmpeg_path`.

**On branch `refactor/ffmpeg-external` (PR #1 draft — do NOT merge until smoke-tested):**
- `libs/encoding/segment_replay.{h,cpp}`: `SegmentReplay` — external ffmpeg writes
  rotating mpegts segments; raw video on stdin + one named pipe per audio track
  (multi-track preserved); `save_clip()` concats recent segments (stream copy).
- `libs/encoding/recording_process.{h,cpp}`: `RecordingProcess` — same model,
  one continuous output file.
- `libs/encoding/ffmpeg_process.cpp`: `ffmpeg_available_encoders` /
  `ffmpeg_resolve_encoder` (parse `ffmpeg -encoders`, CPU+H.265 falls back to HW
  HEVC then H.264).
- Replay/recording **mutual exclusion** (default): a recording suspends the replay
  buffer; advanced setting `allow_concurrent_capture` (off) permits both with a UI
  cost warning. `g_replay_suspended_for_recording` + `replay_active()` in main.cpp.
- **main.cpp core rewire (done)**: in-process `g_video_enc`/`g_audio_encoders` +
  `ReplayBuffer`/`ManualRecorder` packet tee replaced by the external engines.
  Pacer feeds raw BGRA (stride-compacted via `compact_bgra`); every audio track
  routes through a `TrackMixer` to the canonical f32le pipe format. `vpipe_*`
  coordinator fans out to the running engine(s); paused recording is skipped.
  Engine lifecycle is lazy on the first captured frame (so g_enc_w/h are known).
  `RecState` + `rec_*` facade replaces `recording::RecordingState`. Encoder
  resolved via ffmpeg probe; perf log trimmed to capture+pacer.
- **P5 (done)**: `vcpkg.json` LGPL (drop gpl/x264/x265 + HW features);
  `libs/replay-buffer` + `libs/recording` removed from the build and link line;
  ADR-0017 documents the licensing/architecture decision.

All commits compile on Windows CI (the P5 commit triggers a full LGPL FFmpeg
rebuild). Runtime a/v behaviour is UNVERIFIED — needs the real-machine smoke test.

**Smoke test checklist (before merging PR #1):**
1. Bundle an `ffmpeg.exe` (+`ffprobe.exe`) next to `Monolith.exe` or on PATH
   (Settings > Advanced > FFmpeg path also works).
2. Enable replay buffer → confirm `replay-ffmpeg` logs, segments appear under
   `%LocalAppData%\Monolith\replay-segments`, Save Replay produces a playable
   clip with correct A/V sync and multiple audio tracks (game + mic).
3. Manual record start/stop → playable file, multi-track audio, faststart mp4.
4. Start a recording while replay is on → replay suspends (tray Save Replay
   greys out), restores on stop. Toggle `allow_concurrent_capture` → both run.
5. Pause/resume a recording → time is cut at the pause (expected), no desync.
6. Settings > Advanced "Encoding with" shows the resolved vendor (NVENC/AMF/
   QuickSync/x264) for CPU/GPU + H.264/H.265.

**Deferred/known:**
- Orphaned `VideoEncoder`/`AudioEncoder` in `encoding.cpp` are dead code (kept
  beside `TrackMixer`); could be split out later.
- `audio_changed` settings-reload restarts audio but not the replay segmenter;
  a track-count change mid-session may need a full pipeline restart. Low-risk
  edge; revisit if it bites.
- ffmpeg.exe bundling/first-run download not wired into CI packaging yet.

## Session 2026-07-15 — external ffmpeg.exe refactor (in progress, branch)
- Licensing route: external ffmpeg.exe process (keeps GPL x264/x265 out of the ARR
  binary). Replay stays efficient via segment muxer on disk (NOT the mod's
  GB-scale raw-frame RAM buffer).
- Replay keeps multi-track audio via N named pipes (not video-only like the mod).
- Concurrent clip+record: off by default (mutex), opt-in advanced toggle.
- Recording pause = **cut time** (stop feeding ffmpeg while paused); the current
  pts-stitching pause is not replicable with an external encoder.

**NOT yet done (next):**
- The core rewire of `main.cpp`: replace the in-process `g_video_enc` +
  `g_replay`/`g_recording` packet tee with pacer→`push_video` (stride-compacted)
  and audio routes→`push_audio` (per-track named pipe, via TrackMixer to the
  canonical f32le format). Wire `ffmpeg_resolve_encoder` (device/codec + vendor
  display, Point 3). Update perf stats / `is_open()` / `media_start`/`media_stop`.
- P5: vcpkg LGPL-ify for the remaining in-process users (thumbnail.cpp +
  probe_duration decode are non-GPL and can stay), bundle ffmpeg.exe+ffprobe.exe,
  license notices, `docs/DECISIONS.md`.
- Points 6 (audio codecs) intentionally skipped; Point 7 (timing) checked — no fix.

Open items: full runtime smoke on a real machine — save replay (a/v sync,
multi-track), manual record start/stop, the replay↔recording suspend/restore, and
the external ffmpeg segment rotation + concat. None verifiable from CI alone.

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