# Active Handover

## Product Summary

This project is Monolith, a lightweight native Windows 11 clipping and recording application. It runs in the background, exposes a tray icon and global hotkeys, keeps a rolling replay buffer, supports manual recording, and will integrate with an Elgato Stream Deck plugin later.

## Current Phase

- Milestone 1 complete: Win32 tray + hotkey app shell.
- Milestone 2 complete: WGC display capture + WASAPI loopback + mic ingress.
- Milestone 3 complete: FFmpeg encode + encoded-packet replay buffer save a valid MKV clip from the tray/hotkey app shell.
- Milestone 4 complete enough for MVP: manual recording start/stop/pause/resume is wired from tray and hotkeys.
- Milestone 5 complete: WinUI 3 Settings opens from the tray, saves config, persists across restarts, lazy-loads expensive runtime data, and reports realistic apply scopes instead of defaulting to full-app restart.
- Milestone 6 foundation implemented: Default audio mode, Custom routing config/UI, source enumeration, source removal, multiple input devices, and up to six encoded audio tracks are wired. Mixer/A/V soak testing remains open.
- Distribution milestone implemented: versioned builds, self-contained Settings sidecar, per-user Inno Setup installer, WinSparkle auto-update, and a tag-driven release pipeline publishing to a public release-only repo. One-time release setup (key generation, public repo, CI secrets) is documented in `docs/RELEASING.md` and still pending.
- v0.5.1: Active Game dynamic detection and Audio Mode UX improvements. See "v0.5.1 Changes" below.

## Locked Architecture

- Custom native Windows implementation, not an OBS fork.
- Single-process MVP with isolated `/libs`; two-process engine split deferred.
- C++23 + CMake.
- Windows.Graphics.Capture first; Desktop Duplication fallback later.
- WASAPI for audio capture.
- FFmpeg/libav for encode and mux/remux.
- Stream Deck IPC: localhost WebSocket/TCP JSON-RPC at `127.0.0.1:45991`.
- Stream Deck plugin is a remote controller only; no recording logic in plugin.

## v0.5.1 Changes

### Active Game — dynamic detection and switching (root cause fixed)

**Root cause:** `poll_active_game()` previously returned early whenever the captured PID was still alive, so the first game grabbed at startup was locked for the entire session. The new implementation re-evaluates every poll tick and uses debounce-based switching.

**New detection architecture:**
- `audio::detect_active_game(DetectConfig)` in `libs/audio/audio.cpp` scores candidates with explicit bonuses: +4 fullscreen, +4 foreground, +2 active audio session, +8 whitelisted/manual-game. Candidates below `min_confidence` (default 50, mapped as `score*10`) are discarded. Blacklisted executables are rejected before scoring.
- Default poll interval: **30 seconds** (configurable 3000–30000 ms). The 30 s baseline is combined with event-triggered fast scans for responsiveness.
- **Fast scan:** `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT)` triggers a rate-limited immediate rescan (minimum 1 s gap, default) when the foreground window changes. Debounce (default 3 s) still applies before any capture swap — fast scan only shortens *detection* latency, not the switch hysteresis.
- **Switch hysteresis:** the new candidate must remain the top scorer for `switch_debounce_ms` (default 3 s) before the capture is swapped. The new capture is started first (minimising gap to this source only), then the old one is stopped. Replay buffer and other sources are never restarted.
- **First acquire:** no debounce — if no game is captured and a valid candidate appears (including at startup), capture begins immediately.
- **Process-loopback fallback:** if `start_process_loopback` returns false, the failure is logged and `capture_mode = "unavailable"` is reported in runtime status. Default mode is never affected.
- Blacklist/whitelist/manual_games are loaded from `config/default-config.json` (`active_game` block) and clamped/defaulted in `settings_config.cpp`.

**Config keys** (in `active_game` JSON block):
- `detection_enabled`, `poll_interval_ms` (3000–30000, default 30000), `switch_debounce_ms` (1000–15000, default 3000), `min_confidence` (0–100, default 50), `fast_scan_enabled`, `fast_scan_min_interval_ms` (500–5000, default 1000), `blacklist_processes`, `whitelist_processes`, `manual_games`.

### Audio Mode UX

- Mode labels simplified: "Default: All Windows audio + microphone" / "Custom: Per-channel audio setup".
- Custom Sources panel hidden while Audio Mode is Default; config preserved internally.
- Switching Default → Custom shows a confirmation dialog; Cancel reverts to Default without saving.
- Active Game status panel (in Custom mode when Active Game source is active): shows detected game name/PID, confidence, signals, capture mode, poll interval, fast-scan state, and last switch time. Updated from runtime-status.json.
- No fake UI controls; no OS session mute/volume mutation.

### Runtime status extensions

`runtime-status.json` active_game object now includes: `confidence`, `reason`, `capture_mode`, `process_loopback_available`, `last_switch_time`, `poll_interval_ms`, `fast_scan_enabled`.

### Version

Bumped to 0.5.1 in `VERSION`, `CMakeLists.txt`, `installer/monolith.iss`.

## Implemented

- `app/recorder/src/main.cpp`: Win32 app shell, message-only window, tray menu, single-instance mutex, COM/WinRT init, log file.
- Global hotkeys: `Ctrl+Shift+F8` saves replay, `Ctrl+Shift+F9` starts recording, `Ctrl+Shift+F10` stops recording, and `Ctrl+Shift+F11` pauses/resumes recording.
- `libs/capture`: WGC display capture via `CreateFreeThreaded`, D3D11 device, staging BGRA readback, size-change pool recreate.
- `libs/audio`: WASAPI loopback, selected microphone/input-device capture, input-device enumeration, active render-session enumeration, foreground-process detection, and best-effort Windows process-loopback activation.
- `libs/encoding`: FFmpeg H.264 probing/open path (`h264_nvenc`, `h264_amf`, `h264_qsv`, `libx264`), BGRA-to-YUV video encode, AAC audio encode with resampling and logical audio track IDs 1-6.
- `libs/replay-buffer`: encoded-packet ring buffer, keyframe-based clip start, async Matroska clip writer with video-only fallback and up to six audio tracks.
- `libs/recording`: live MKV writer for manual recordings, fed by the same encoded packet stream as replay, with up to six audio tracks.
- App wiring: WGC frames and configured audio sources feed encoders; encoded packets feed replay buffer and, when armed, the manual recorder. Default audio mode routes desktop audio to track 1 and selected microphone to track 2.
- User media output defaults to `Videos\Monolith\Clips` and `Videos\Monolith\Recordings`; logs/runtime data stay under `AppData\Local\Monolith`.
- Settings: `AppData\Local\Monolith\config.json`, default/user JSON merge through `nlohmann-json`, and a WinUI 3 tray-launched settings sidecar. Settings has been manually tested: it opens, saves configuration, persists across restarts, and notifies the recorder to reload settings after save.
- App icon: `Monolith.ico` embedded via `app/recorder/monolith.rc` (tray + exe icon) and used by the Settings sidecar and installer.
- Replay clips support MP4 in addition to MKV: `replay_buffer.save_container` config field, Settings Output-tab format selector, and MP4 clip write path in `libs/replay-buffer` (video-only fallback preserved).
- Component toggles: `replay_buffer.enabled` / `recording.enabled` (Settings Components card) let low-end users disable the replay buffer or manual recording entirely; disabled components skip pipeline work and their tray menu items render grayed/disabled.
- Active Game audio source detection improved beyond raw foreground-process: `detect_active_game()` in `libs/audio` scores candidate windows (+2 fullscreen-sized, +2 foreground, +1 owns a render audio session), excludes shell processes, and falls back to the bare foreground process; still best-effort and fail-closed when no game is found.
- Versioning: single source of truth flows git tag → `-DMONOLITH_VERSION` (CMake cache var, default from `project(monolith VERSION ...)`) → generated `version.h` (consumed by the `.rc` VERSIONINFO block and the updater) and `dotnet publish -p:Version=`. `Monolith.exe` and `Monolith.Settings.exe` both carry the real file version.
- Self-contained sidecar: `Monolith.Settings.csproj` publishes with `SelfContained` + `WindowsAppSDKSelfContained` (`PublishTrimmed=false`; WinUI3 not trim-safe). No .NET or Windows App SDK prerequisites on target machines; payload stays flat next to `Monolith.exe`.
- Auto-update: WinSparkle (vcpkg dep, pinned `builtin-baseline`) integrated via `app/recorder/src/updater.{h,cpp}`; tray menu "Check for Updates…" (`CMD_CHECK_UPDATE`); `update.auto_check` config field (default true) with a Settings "Updates" card toggle, applied live on settings reload. Appcast URL points at `fraa2a/Monolith-releases` latest release; EdDSA public key placeholder (`kEdDsaPublicKey`) must be filled after one-time keygen (see `docs/RELEASING.md`).
- Installer: `installer/monolith.iss` — per-user (`PrivilegesRequired=lowest`, `{localappdata}\Programs\Monolith`, no UAC so WinSparkle can update silently), stable AppId GUID for in-place upgrades, GPLv3 license page, optional desktop/startup tasks, full self-contained payload; uninstall never touches user config in AppData.
- Release CI: `.github/workflows/version-tag.yml` extracts the version from tag `vX.Y.Z`, builds with pinned vcpkg, compiles the installer, signs it and generates `appcast.xml` (`scripts/generate-appcast.ps1`, Ed25519 via openssl), creates a GPLv3 source zip (`git archive`), and publishes everything to the public `fraa2a/Monolith-releases` repo via `RELEASES_REPO_PAT`.

## UI Rewrite — Phase F1 (SQLite clip catalog + thumbnails + self-heal)

Foundation phase of the deno.md UI rewrite. This phase is engine-only; no UI code
yet. Plan phases F2–F6 (Deno UI, context-menu, detail view, settings→DB, audio/game)
follow. Locked decisions: engine will read settings from a DB (config.json to be
removed in F5); SQLite via vcpkg `sqlite3` + new `libs/storage`; phased delivery.

Implemented in F1:
- `vcpkg.json`: added `sqlite3` dependency (baseline unchanged).
- New `libs/storage` (`storage.{h,cpp}`, `CMakeLists.txt`): `ClipDb` over a
  per-folder SQLite DB in WAL mode. Schema = deno.md §5 `clips` table extended with
  `favorite` + a `clip_hashtags(clip_id,tag)` table (+ `game_catalog` reserved for
  F6). `ClipDb::open()` auto-creates/validates and refuses to overwrite a
  non-Monolith/wrong-schema `.db`. API: `insert_clip`, `remove_clip(remove_files)`,
  `set_favorite`, `add_hashtag`, `remove_hashtag`, `reconcile` (self-heal +
  migration import). `now_iso8601_utc()` helper.
- `libs/encoding/thumbnail.cpp`: `encoding::generate_thumbnail(video, thumb, max_dim)`
  — libav first-frame decode → RGB24 scale → PNG. Added to the encoding target.
- `app/recorder/src/main.cpp`: `catalog_clip()` generates the thumbnail + inserts
  the clip row off the UI thread; wired into the replay `save_clip` callback
  (`source="replay"`) and `CMD_RECORDING_STOP` (`source="manual"`, on a detached
  thread). `reconcile_catalogs()` runs both catalogs' self-heal on a background
  thread at startup (called from `wWinMain`). Game association fields left empty
  until F6.
- Layout choice: DB (`clips.db`/`recs.db`) and `.thumbs\` are co-located with the
  existing video files inside the configured output folder; the video write path
  and default dirs are unchanged (keeps blast radius small). See ADR-0010.
- CMake: `add_subdirectory(libs/storage)` in root; `storage` linked into
  `app/recorder`. `libs/storage` links `encoding` + `unofficial::sqlite3::sqlite3`.
- Docs: ADR-0010 added to `docs/DECISIONS.md`.

NOT yet verified: **the build was not run in this environment** (no vcpkg toolchain
/ installed ffmpeg+sqlite reachable here; the local `build/` is stale from another
source path). Code was reviewed for compile-correctness only. Next session must run
`build.bat` (or the manual cmake configure+build) on a machine with vcpkg, then do
the F1 runtime verification: save a clip → confirm `clips.db` row + `.thumbs\*.png`;
delete an mp4 and restart → row removed; delete a thumb and restart → regenerated.

## UI Rewrite — Phase F6 + Deno Desktop build (game, capture_mode, audio, Discord)

Engine + UI. Game detection, capture_mode, audio redesign scaffold, Discord toggle,
and the switch to **real Deno Desktop** (was the @webview/webview stopgap).

Engine (C++):
- `main.cpp` `catalog_clip`: fills clip game fields (heuristic `detect_active_game()`
  at save) → the Home game filter populates. `evaluate_capture_mode(hwnd)` on the
  WM_TIMER honors `capture_mode: game_only` (stops replay capture after
  idle_timeout with no game, resumes on detect; never touches a manual recording).
- `settings_config`: `capture_mode {mode, idle_timeout_seconds}` + `discord` +
  new `audio` §7 fields (capture_mode/separate_tracks/app_preferences/extra_
  microphones) added to defaults; parsed/clamped where the engine needs them,
  round-tripped otherwise.

Deno UI:
- **Real Deno Desktop** (`deno desktop`): dropped `@webview/webview` and the manual
  port/webview wiring. `main.ts` = set `DENO_SQLITE_PATH` if a bundled dll exists →
  optional `Deno.BrowserWindow({title,width,height})` → `Deno.serve(handle)` (Deno
  Desktop auto-binds the window). `deno.json` tasks use `deno desktop`.
- Game catalog: `libs`-free — `scripts/download-game-db.ts` fetches the Discord
  detectable list into `%LocalAppData%\Monolith\game_catalog.db` (weekly refresh from
  `main.ts`); `bindings/game-catalog.ts` looks up display names + resolves icons
  lazily (RPC→CDN, cached). `/api/game-catalog`, `/api/game-icon`. Cards show the
  game icon (placeholder otherwise).
- Settings popup: Game Detection page gained capture_mode controls; Advanced gained
  the Discord toggle; Audio page redesigned (`settings/audio-settings.tsx`, Medal-
  style: All PC / Specific apps, separate-tracks, dynamic app list from
  runtime-status audio_sessions with enabled/volume, static Game/Mic, mic dropdown).
- `settings_window.cpp`: launches the Deno Desktop **bundle**
  (`ui/Monolith.UI/laufey_webview.exe --runtime Monolith.UI.dll`, cwd = bundle),
  falling back to the legacy WinUI exe. `app/desktop-ui/CMakeLists.txt` runs
  `deno desktop --icon MONOLITH.ico --include dist --output <out>/ui/Monolith.UI`.

BUILD VERIFIED (this session, Deno 2.9.1):
- `deno check` passes for backend (`main.ts`) and frontend (`ui/src/main.tsx`) and
  all entry points. `deno task ui:build` bundles `dist/` (app.js ~37KB). Router
  smoke (fake Requests to /, /api/*) returns without crashing.
- `deno task compile` (= `deno desktop`) **succeeds**, producing
  `build/app/desktop-ui/Monolith.UI/` (laufey_webview.exe + 82MB Monolith.UI.dll +
  WebView2). Launching it: "Runtime loaded successfully / Runtime started",
  WebView2 initializes. Only gap: `@db/sqlite` fetches `sqlite3.dll` on first run
  and the sandbox blocked the download (os error 10013) — not a code bug. Ship the
  denodrivers `sqlite3.dll` next to the launcher (main.ts auto-sets
  DENO_SQLITE_PATH) for a fully offline app, or allow the one-time download.

Remaining engine work (deferred, needs build+runtime verification on a full machine):
- The **C++ engine is still unbuilt here** (no vcpkg toolchain): F1–F6 C++ changes
  (storage, thumbnails, settings.db, IPC clip mutations + rename, capture_mode,
  game fields) are review-only. Build with `build.bat` and run the per-phase checks.
- Audio §7 deep engine (per-process volume/mixing via IAudioSessionManager2,
  static-source routing) is scaffolded in config/UI only; libs/audio still consumes
  the old audio model. Discord Rich Presence is config-only (no named-pipe client).

## UI Rewrite — Phase F5 (settings popup + settings → SQLite)

Engine + UI. Config store moves from `config.json` to `settings.db`. ADR-0009 rewritten.

Engine (C++):
- `libs/storage`: global settings KV store in `<app_data>\settings.db` (WAL) —
  `settings_get_all` / `settings_replace_all` (generic string KV, JSON-agnostic;
  table `settings(key,value)`).
- `settings_config.cpp` `load()`/`save()` rewritten: persistence is now `settings.db`
  (one KV row per top-level config section, split/assembled with nlohmann), **not**
  `config.json`. All existing merge-over-defaults + validate/clamp logic is unchanged.
  One-time migration: empty DB + legacy `config.json` present → import it, seed the
  DB, rename the file to `config.json.imported.bak`. `Config` gained `app_data_dir`
  (used by `save()`); `user_config_path` now points at `settings.db` for logging.
  `config/default-config.json` is still the default seed / compiled fallback.
- Reload path unchanged in shape: IPC `reload_settings` → WM_APP+2 →
  `reload_settings_from_disk` → `settings::load()` (now reads `settings.db`).

Deno UI:
- `bindings/settings.ts`: read/write `settings.db` (section rows) via `@db/sqlite`;
  UI is the settings writer. `bindings/config.ts` now resolves output dirs from
  `settings.db` (not config.json). `bindings/runtime.ts`: reads `runtime-status.json`
  for capability-gating.
- `server/router.ts`: `/api/settings` GET/POST (POST writes settings.db then calls
  `reloadSettings()` IPC) and `/api/runtime-status` GET.
- `ui/src/settings/settings-popup.tsx`: custom overlay with left page nav
  (General/Output/Clip/Capture/Audio/Hotkeys/Advanced/Game Detection) + right form.
  Edits a draft of the whole config (generic get/set-by-path field helpers:
  bool/text/num/select/list), Save POSTs it. Encoder/monitor选项 capability-gated by
  runtime-status. Opened from the Filters gear (`onOpenSettings`). `ui/src/lib/
  settings-api.ts` client. CSS added.

Caveats / open: the Audio page covers the current schema (mode + primary mic); the
full Medal-style audio redesign is F6. `flatten`-free per-section storage avoids the
empty-array-drop pitfall. Config is edited whole and replace-written — concurrent
engine writes don't happen (engine only writes at first-run seed).

NOT verified: not built/run here. Next verify: open Settings from gear → change a
live setting (e.g. clips folder) → Save → engine applies without restart (check log)
and `settings.db` holds the value, `config.json` gone (backed up). Change a deferred
setting during recording → deferred.

## UI Rewrite — Phase F4 (detail view + inline rename)

Engine + UI. Builds on F2/F3.

Engine (C++):
- `libs/storage` `ClipDb::rename_clip(id, new_stem)`: validates stem (rejects
  path sep / dot / extension), collision-guards the target, moves the video file
  first (source of truth) then the thumbnail (best-effort; on thumb-move failure
  the stored thumbnail name is nulled so reconcile rebuilds it), updates the row.
- `libs/ipc`: new `clip_rename` method + `new_name` param on `ClipMutation`.
  `main.cpp::handle_clip_mutation` routes it to `rename_clip(utf8_to_wide(new_name))`.

Deno UI:
- `ui/src/home/detail-view.tsx`: centered (not fullscreen) overlay — player with
  scrubbable timeline (seek) + volume slider, details panel with inline-editable
  name (pencil → input + extension shown + Save; Enter/Esc; errors surfaced from
  the engine), hashtags (opens the F3 HashtagDialog), and ◀ ▶ arrows (+ Left/Right
  keys) to move between clips in the filtered list without closing. Clicking a card
  opens it (`onOpenDetail` → `detailIndex`). Index is clamped to the live list and
  the view closes when the list empties (handles rename/delete refetch).
- `bindings/ipc-bridge.ts`, `server/router.ts` allow-list, `ui/src/lib/api.ts`
  (`clipApi.rename`) extended for `clip_rename`.
- `styles.css`: detail overlay, timeline/volume, nav arrows.

Still open: F5 settings popup + settings→SQLite (rewrites ADR-0009, removes
config.json), F6 game detection + audio redesign + Discord. Inline name edit on the
card itself (deno.md §2.1 pencil) is currently handled via the detail view; a direct
card-inline editor can be added later if wanted.

NOT verified: not built/run here (no vcpkg / no Deno in this env). Review-only.
Next verify (with toolchains): open a card → detail overlay; seek + volume work;
◀ ▶ navigate; rename via pencil persists (video + thumb renamed on disk, DB row
updated, grid reflects it); hashtags editable from detail.

## UI Rewrite — Phases F2 + F3 (Deno UI: Home grid + clip context menu)

Engine + new UI layer. Builds on F1's clip catalog. See ADR-0011.

Engine (C++) changes:
- Renamed `app/desktop-ui` (WinUI) → `app/OLD-settings-ui` (kept building during
  migration). New `app/desktop-ui` = Deno app. Root `CMakeLists.txt` builds both.
- `settings_window.cpp`: tray now launches `Monolith.UI.exe` (candidates under
  `ui/`), falling back to the legacy `Monolith.Settings.exe`.
- `libs/ipc`: new JSON-RPC methods `clip_set_favorite`, `clip_add_hashtag`,
  `clip_remove_hashtag`, `clip_delete` (params: source/id/tag/favorite) and
  `reload_settings`. `start()` now takes a `ClipMutationFn`. Implemented in
  `main.cpp::handle_clip_mutation`, which opens the right `storage::ClipDb`
  (clips/recs dir from a mutex-guarded snapshot `g_clips_dir_snapshot` /
  `g_recs_dir_snapshot`, updated in `apply_runtime_settings`) — engine stays the
  single writer.

New Deno UI (`app/desktop-ui`, unverified — not built here):
- Backend `main.ts`: WebView2 window (`jsr:@webview/webview`) + localhost
  `Deno.serve` HTTP server. `server/router.ts` serves the built frontend, a JSON
  API (`/api/clips|games|hashtags|mutate`), and range-streamed `/media` + `/thumb`
  (`server/media.ts`). `bindings/clips.ts` reads `clips.db`/`recs.db` **read-only**
  (`jsr:@db/sqlite`, WAL) and stat's file sizes; `bindings/ipc-bridge.ts` is the
  JSON-RPC client for mutations; `bindings/config.ts` resolves output dirs from
  `config.json` (→ settings.db in F5). `games.ts`/`audio.ts` are F6 stubs.
- Frontend `ui/` (Preact + esbuild via Deno loader, bundled by `build.ts` → `dist/`):
  Home grid, rounded clip cards (static PNG at rest, hover preview + mute/fullscreen
  overlay, **placeholder + still-plays when thumb missing/broken** — user req #2 UI
  side), metadata (name, game, 1-decimal size, Today/Yesterday/`Jun 23`/`Jun 23, 2025`
  date). F3: global native-contextmenu suppression + custom **rounded** menu shown
  only over cards (favorite / hashtags… / fullscreen / **delete in red**); delete →
  custom secondary confirm popup; hashtag add/remove popup; filters by game/hashtag/
  favorite/search; fullscreen overlay at original framerate. All mutations go through
  `/api/mutate` → engine IPC.
- CMake `app/desktop-ui/CMakeLists.txt`: `deno task ui:build` + `deno compile` →
  `<recorder-output>/ui/Monolith.UI.exe` when `deno` is on PATH; warns+skips otherwise.
- Docs: ADR-0011 added; DEVELOPMENT_RULES rule 2 narrowed (webview = UI only);
  CLAUDE.md target list updated.

Not done in F2/F3 (later phases): F4 detail view (§2.2) + inline rename (needs a
rename IPC + file move); F5 settings popup + settings→SQLite (rewrites ADR-0009,
removes config.json, engine reads settings.db); F6 game detection + audio redesign +
Discord. The hover "15 fps preview" currently plays the full clip (a dedicated
low-fps preview asset is a future optimization). Autoplay-with-audio on hover may be
blocked by WebView2 until a user gesture — mute toggle provided.

NOT verified: **neither the C++ nor the Deno app was built/run here** (no vcpkg
toolchain and no Deno reachable in this environment). Code reviewed for
compile/type consistency only. Next session with the toolchains must: `build.bat`
(engine), then in `app/desktop-ui` `deno task ui:build` + `deno task dev` (with the
engine running), and verify F2/F3 end-to-end: grid shows clips + thumbnails +
placeholder; right-click card → rounded menu; elsewhere → nothing; favorite/hashtag
persist (query DB); delete asks for confirm then removes row+files; hashtag filter
works; fullscreen plays.

## Latest Continuation Notes

- `libs/encoding/mux_common.cpp`: removed `+faststart` for mp4. It relocated the
  moov atom to the file front at `av_write_trailer()` time, rewriting the entire
  file on stop — a multi-GB manual recording froze the UI/hotkeys for seconds on
  the message-loop thread. moov is now left at the end (OBS default); stop is
  near-instant; local playback/editing unaffected. (Interrupted mp4 still needs a
  moov to play — use mkv for crash resilience, same as before.)
- `libs/capture/capture.cpp`: changed WGC surface QI from `as<IDirect3DDxgiInterfaceAccess>()` to `try_as<...>()` so a missing interface does not throw on the frame callback thread.
- `libs/audio/audio.cpp`: silent WASAPI packets now preserve `data_bytes`.
- `libs/encoding/encoding.cpp`: `AudioEncoder::push_pcm` now converts null PCM input into zeroed silence, preserving AAC timeline during silent stretches.
- `app/recorder/src/main.cpp`: system-audio encoder now receives silent packets too.
- `libs/replay-buffer/replay_buffer.cpp`: MKV writer now copies packet bytes into `AVPacket` via `av_new_packet` instead of borrowing vector memory.
- `libs/encoding/encoding.cpp`: video/audio encoder public methods are now mutex-protected because WGC and WASAPI drive them from different worker threads.
- Build tooling was installed and verified: CMake 4.3.3, Visual Studio Build Tools 2022 with MSVC, and local ignored `vcpkg/`.
- `vcpkg.json` now uses valid FFmpeg features: `avcodec`, `avformat`, `swscale`, `swresample`, `amf`, `nvcodec`, `qsv`, `x264`, `gpl`.
- Release build succeeds: `build/app/recorder/Release/Monolith.exe`.
- Runtime smoke test succeeds: app starts, WGC captures `2560x1600`, H.264 opens lazily from the first real WGC frame, `Ctrl+Shift+F8` saves `20260611_064508_30s_clip.mkv`.
- Fix: video encoder is now opened lazily from actual WGC frame dimensions instead of `GetMonitorInfoW` virtual DPI coordinates, which previously caused all 2560x1600 frames to be skipped by a 1706x1066 encoder.
- Legacy app branding changed to Monolith for user-facing app identity and runtime folders.
- Manual recording start/stop/pause/resume now has accepted/rejected state transitions instead of log-only tray stubs.
- Manual recording pause omits paused time from the output file and resumes writing on the next video keyframe.
- Settings startup now logs `settings-startup.log` under `AppData\Local\Monolith`; current trace shows WinUI/Windows App SDK process startup dominates before app code runs, while config load and first page construction are small.
- Settings defers runtime-status loading, audio device/process enumeration, monitor data, and capture warnings until the Audio or Capture page is opened. XAML construction itself is measured small and kept reliable.
- Output tab owns replay clip folder, manual recording folder, and format selection. Replay clips remain MKV only; manual recordings expose MKV/MP4 because the live recording writer has both muxers.
- Recording tab was removed; recording storage and format controls live in Output, while pause behavior remains an internal fixed `timestamp_gap` policy.
- Settings exposes replay duration, replay memory budget, capture monitor, output resolution, capture border, video bitrate, encoder backend, controlled FFmpeg options, and hotkey display/editing for the current config format.
- Settings save applies output paths and hotkeys live for future operations; replay settings live-reconfigure the replay buffer; audio routing and capture/encoder changes restart their pipelines when no manual recording is active and are deferred while recording.
- WGC capture now attempts `GraphicsCaptureSession::IsBorderRequired(false)` before `StartCapture`; failure is ignored so capture still starts.
- Audio V2 foundation added: `audio.mode`, primary microphone selection, saved source routing, source add/disable/remove UI, multiple input-device sources, process session sources, Active Game source, and 1-6 track validation.
- Custom audio UI preserves the selected source by stable source ID while editing, avoids full list rebuilds for track/enable toggles, and shows all six track buttons in a readable two-row layout.
- Removing a configured source deletes its saved routing entry; detected apps/devices stay available to add again but are not auto-restored as configured.
- Custom routing currently duplicates one source to multiple tracks. Multiple sources assigned to the same track are detected and skipped for that track until a real PCM mixer is added.
- Process and Active Game capture use Windows process loopback when available and fail closed if activation fails or the process is missing.

## Not Implemented Yet

- (done) PCM mixer for multiple simultaneous sources sharing the same output track:
  `encoding::TrackMixer` resamples each source to a canonical 48k/2ch float format,
  buffers per-source FIFOs, and a wall-clock-paced thread sums + clips into the
  track encoder. Routing in `main.cpp` counts sources per track; tracks with >= 2
  sources go through a mixer, single-source tracks push directly. Needs runtime
  soak testing for A/V sync and overrun behavior.
- Long-session A/V sync soak tests for the new multi-track audio path.
- Game detection is heuristic/best-effort; confidence scoring reduces false positives but is not infallible. Process-loopback requires Windows 10 21H2+ and is not guaranteed for all games/launchers.
- True live audio re-route without restarting the audio pipeline (Active Game source swaps its own capture only; other sources still require a pipeline restart).
- Real IPC server/client.
- Stream Deck plugin code.
- Desktop Duplication fallback.
- GPU-resident encoder path; current spike uses CPU-readable BGRA staging.
- Automated tests for replay/manual recording.
- Release one-time setup: public repo `fraa2a/Monolith-releases`, Ed25519 keygen + public key in `updater.cpp`, CI secrets `WINSPARKLE_ED_PRIVATE_KEY` / `RELEASES_REPO_PAT` (see `docs/RELEASING.md`).
- Clean-VM verification of the self-contained installer and a real end-to-end update-path test (needs first public release).

## Build/Verification

Current build command:

```powershell
& '<cmake>\cmake.exe' -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="<vcpkg>\scripts\buildsystems\vcpkg.cmake"
& '<cmake>\cmake.exe' --build build --config Release
```

Known result:

- Release build succeeds and publishes both `Monolith.exe` and `Monolith.Settings.exe` into `build/app/recorder/Release`.
- Settings has been manually tested by the user: opens from the app, saves config, persists across restarts, and implemented settings behave correctly.
- Settings/runtime UX build succeeds as of 2026-06-12 via `dotnet build app\desktop-ui\Monolith.Settings.csproj -c Release -r win-x64 --no-restore` and `cmake --build build --config Release --parallel`; MSBuild emits a non-fatal `CS1668` warning for a missing local ATL/MFC `LIB` path during the full build.
- Settings smoke succeeds: published `Monolith.Settings.exe` opens/closes, Audio page initializes without exception, and UIAutomation can find Track 1 through Track 6 controls.
- Runtime smoke succeeds for replay command dispatch: `20260612_191331_30s_clip.mkv` was written under `Videos\Monolith\Clips`.
- Runtime smoke succeeds for manual recording with the current user config `recording.container=mp4`: `20260612_191451_recording.mp4` was written under `Videos\Monolith\Recordings` after a longer recording window.
- Distribution build verified locally as of 2026-06-12: Release build with `winsparkle` from pinned vcpkg baseline succeeds; `Monolith.exe` FileVersion 0.3.0 and `Monolith.Settings.exe` 0.3.0.0; payload is self-contained (519 files, ~239MB incl. `WinSparkle.dll`, `coreclr.dll`, `hostfxr.dll`, `Microsoft.WindowsAppRuntime.dll`); runtime smoke "app shell ready".
- Installer verified locally: Inno Setup 6.7.3 compiles `installer/monolith.iss` → `MonolithSetup-0.3.0.exe` (~65.8MB).
- `scripts/generate-appcast.ps1` verified for both unsigned (warning) and signed paths with a throwaway Ed25519 key (64-byte signature, valid base64 edSignature).

## Next Steps

1. Add a real PCM mixer so multiple sources can safely share the same audio track.
2. Runtime-test Default mode: desktop track 1 plus microphone track 2 in replay and manual recording.
3. Runtime-test Custom mode with multiple input devices, process loopback, source removal, missing devices/processes, and save-triggered pipeline reload.
4. Add a small integration/unit test harness for settings merge/load behavior, audio routing validation, and `ReplayBuffer::save_clip`.
5. Continue runtime hardening: graceful shutdown, capture stop races, encoder failure UX/logs, and A/V sync checks.
6. Complete release one-time setup per `docs/RELEASING.md` (public repo, Ed25519 keys, secrets), then push a `vX.Y.Z` tag and verify the public release + update path end to end.
7. Test the installed build on a clean VM/user without .NET or Windows App SDK.

## Open Risks

- Capture-to-encoder path still performs CPU readback; acceptable for spike, not final performance target.
- WGC callback and encoder flow need runtime soak testing.
- A/V sync needs real capture validation, especially around silence and long sessions.
- Manual recording pause/resume needs longer-session validation.
- WGC border suppression requires `GraphicsCaptureSession::IsBorderRequired` / `IGraphicsCaptureSession3`, introduced in `Windows.Foundation.UniversalApiContract` 12.0. The local SDK exposes it under Windows Kits `10.0.26100.0`.
- Per-app audio capture and active-game detection are expected to be best-effort on Windows and must fail closed without breaking default recording.
- WinUI 3 cold process startup is not eliminated; current work optimizes avoidable app-side work after process start.
