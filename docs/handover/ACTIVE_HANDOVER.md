# Active Handover

Updated: 2026-07-12

## Session 2026-07-12 — DB-gated detection + auto-record features

Landed across 4 commits on `main` (all Windows CI green; native engine NOT
built locally — vcpkg absent — so runtime behavior is unverified and needs a
real-machine smoke test).

- **Bug fixes**: topbar exe-icon call was missing its `processName` arg
  (`titlebar.tsx`) — fixed; topbar now shows the DB `display_name` verbatim (no
  `prettyAppName`). Thumbnail failures were silent (verbose logging off by
  default) and the frontend `<video>` fallback can't decode `.mkv` in WebView2 —
  added an always-on `logging::log_error` channel, the frontend now hands off to
  the engine FFmpeg regenerator, and `clip_regen_thumb` bumps `clip_generation`
  so the grid reloads.
- **New `libs/gamelist`**: recorder-owned SQLite cache of the Discord detectable
  list (`https://discord.com/api/v10/applications/detectable`), fetched via
  WinHTTP on a worker thread (startup + 72h + refetch-if-missing), lock-free
  snapshot. `%LocalAppData%\Monolith\game_list.db`.
- **DB-gated detection** (`libs/audio`): `detect_game_candidates()` enumerates
  all processes (Toolhelp), gates on game-list membership; heuristic scoring and
  foreground-fallback removed. `detect_active_game()` now returns the best
  DB-matched candidate.
- **Engine state machine** (`main.cpp`): 3 s cadence; `poll_active_game()`
  resolves an effective target (user selection by exe, else most-recently-
  focused) and publishes `game_candidates[]` + `selected_game_pid`;
  `evaluate_capture_mode()` runs auto-record (startup 60 s focus grace,
  auto-next-on-close, manual switch). New `set_selected_game` IPC command +
  `SelectGameFn`. `capture_mode.clip_without_game` setting; idle-timeout now
  honored. Pacer holds the last frame while the captured game window is minimized
  (game_only only). Screen mode auto-follows the game's monitor unless pinned.
- **UI**: topbar multi-game picker + Clip-Without-Game toggle; Settings > Game
  page toggles (mode, auto-record, clip-without-game, idle timeout).

Open items for next session:
- Runtime smoke test on a real machine with a real game (detection match,
  auto-record start/stop/switch, frozen frame, thumbnail generation) — none of
  this is independently verifiable from CI compile alone.
- Confirm the live Discord detectable JSON shape matches the parser in
  `gamelist.cpp` (`[{id, name, executables:[{name, os, is_launcher}]}]`).
- Rust `game_catalog.db` still holds artwork only; `discord_app_id` from the new
  gamelist is now on candidates but not yet threaded into clip rows for artwork.

Updated (previous): 2026-07-10

## Product Summary

Monolith is a Windows 11 clipping and recording app. It runs in the background,
exposes tray commands and hotkeys, keeps a rolling replay buffer, supports manual
recording, has a Tauri/Preact desktop UI for clip library/settings, and includes
a Stream Deck controller plugin.

## Current Phase

The repository is in MVP-hardening and release-readiness work, not early
prototype work.

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

`get_status` returns `clip_generation`; the UI host uses it for live clip-grid
refresh.

## Important Current Facts

- The old WinUI settings sidecar is gone.
- The Deno Desktop shell is gone.
- `Monolith.Settings.exe` is stale documentation only; current sidecar is
  `Monolith.UI.exe`.
- Live settings are not `config.json`. `config.json` is legacy migration input.
- Active Game timing tunables are scrubbed from settings; the engine polls every
  5 seconds and also fast-scans foreground changes.
- Audio tracks with multiple sources use `TrackMixer`; single-source tracks feed
  encoders directly.
- Replay memory budget is fixed at 512 MB.

## Recent Documentation Pass

This session updated all Markdown docs to match the current code:

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

Documentation was updated from source inspection. Build/test verification should
be run after the doc pass:

```powershell
cmake --build build --config Release --parallel
cd app\desktop-ui
deno run -A build.ts
cargo build --release --manifest-path src-tauri\Cargo.toml
cd ..\..\plugins\stream-deck
npm run build
```

## Next Steps

1. Run the verification commands above.
2. Add tests for settings migration and IPC request handling.
3. Runtime-test replay and manual recording with default audio.
4. Runtime-test custom audio with multiple sources on one track.
5. Runtime-test Active Game switching.
6. Start GPU downscale-before-readback spike.
7. Complete first public release setup from `docs/RELEASING.md`.

## Open Risks

- CPU/RAM cost from WGC BGRA CPU readback and software conversion.
- Long-session A/V sync not fully validated.
- Process-loopback and active-game detection are best-effort on Windows.
- Update path cannot be fully verified until first public release exists.
- No broad automated test suite yet.

## desktop-ui: UI redesign pass (UI_Todo.md)

Executed the 9-task redesign plan in `UI_Todo.md` to bring `app/desktop-ui`
closer to feature parity/polish with competitor products while keeping
`PRODUCT.md`'s "quiet, precise, native" identity.

Done:

- New `--accent`/`--accent-hi`/`--accent-soft`/`--accent-ink` design tokens
  (desaturated lavender-ice) applied to all interactive/active/selected UI
  (`.btn-primary`, `.toggle.on`, `.seg.active`, `.side-item.active`,
  `.settings-tab.active`, input/select focus, `.rec-dot.clip`). Red stays
  recording-only, gold stays favorites-only — verified no overlap.
  `PRODUCT.md` "Brand Personality" and Design Principle 3 updated to document
  the third accent's single meaning.
- Titlebar status cluster flex ratios fixed (`.tb-brand` no longer grows to
  consume space, `.tb-status` sits anchored after it) plus a persistent chip
  background on `.capture-feed`.
- Settings popup: grouped nav (Recording / Output / Advanced sections),
  content-driven modal height (was a fixed 640px regardless of content), and
  a real "About" section on the General page using Tauri's `getVersion()`
  (previously that page was near-empty).
- Library clip cards: persistent (non-hover) action row — favorite, open
  containing folder, delete — reusing the app's existing `ConfirmDialog` flow
  for delete rather than a native `confirm()`. Added a new Tauri command
  `reveal_in_explorer` (`explorer /select,<path>`) since no folder-reveal
  capability existed anywhere in the codebase before this session.
- Typography coherence pass: weight/tracking bumps on active nav/tab states
  and section/card titles for clearer hierarchy. No new font bundled.

Intentionally skipped (documented in `UI_Todo.md`'s own terms — don't fake
data or add scope the app doesn't support):

- **Sidebar storage/space indicator** — `RuntimeStatus` has no disk/storage
  fields; adding them needs new backend work (`settings_store.rs` +
  `commands.rs` + `settings-api.ts`) out of scope for a UI pass.
- **Bundled display font** — marked optional in the spec; needs a separate
  licensing/packaging decision.

Build verified this session: `npm run build` (Vite) and
`cargo build --release --manifest-path src-tauri\Cargo.toml` both succeed.
Fixed one unrelated pre-existing compile error found along the way
(`game_catalog.rs::resolve_artwork` was missing the `last_updated` field
after the Discord cache redesign in a prior commit).

Not done: manual runtime smoke test (build machine only, no interactive
session) — verify titlebar/sidebar/library grid/settings modal/card actions
visually before shipping, per `CLAUDE.md`'s manual runtime smoke checklist.

## desktop-ui: clip card/detail/settings follow-up pass

Follow-up session on top of the redesign pass above, addressing user feedback:

- Clip card: removed the delete and "open containing folder" buttons entirely
  (`.card-actions`/`.card-act` gone). Reveal-in-explorer moved into detail view:
  clicking the clip title/name there now calls `clipApi.revealInExplorer(clip)`
  instead of just being a label.
- Detail view player: added a fullscreen button next to the volume slider
  (`onFullscreen(clip)`, wired through `app.tsx` into the existing `Fullscreen`
  component).
- Clip card compact meta row reordered to `[game/source icon] · date · size`
  (previously size/date/icon), matching the requested "Game, Date, Size" order.
- Settings popup no longer resizes when switching categories: `.settings`
  changed from `height: auto; max-height: min(640px, 90vh)` to a fixed
  `height: min(640px, 90vh)`. `.settings-body`'s existing `overflow-y: auto`
  now does all the scrolling; a sparse page just leaves empty space instead of
  shrinking the whole popup.
- Audio settings: new "Track Layout" section/toggle (`audio.track_layout`:
  `"single" | "separate"`). Microphone always keeps its own track; every other
  source (game desktop audio + other apps) either shares track 1 or gets its
  own free track (3-6). This is purely a frontend concern — confirmed via
  reading `settings_config.h`/`encoding.h` that the C++ engine already mixes
  whatever `tracks: []` values a source is given, and confirmed via
  `settings_store.rs` that config round-trips as an untyped JSON blob, so no
  backend/schema changes were needed.
- Audio settings "Other sources" now re-polls `getRuntimeStatus()` every 5s
  while the Settings popup is open, so newly-detected audio-producing apps
  show up without closing/reopening Settings. Note: this refresh happens at
  the popup-mount level plus the 5s poll, not as an independent re-fetch on
  every tab switch within one already-open popup session — in practice the 5s
  poll means data is never more than 5s stale regardless of which tab is
  active, but flagging the distinction since it wasn't asked for literally.

Build verified this session: `npm run build` (Vite) and
`cargo build --release --manifest-path src-tauri\Cargo.toml` both succeed, no
compile errors.

Not done: manual runtime smoke test of these specific changes (build machine
only) — verify card layout, detail-view name-click/fullscreen button,
settings popup fixed sizing across all category pages, and the audio track
layout toggle actually changing recorded track assignment, before shipping.

No git commit/push has been made — awaiting explicit user confirmation per
standing instruction.
