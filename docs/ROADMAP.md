# Roadmap

## Current Status

Monolith has moved beyond the early prototype milestones. Current baseline:

- Native Win32 tray app with hotkeys.
- WGC video capture and WASAPI audio capture.
- FFmpeg encode and mux.
- Replay buffer and manual recording.
- SQLite clip catalogs and thumbnails.
- SQLite `settings.db` settings store.
- Tauri v2/WebView2 desktop UI with Preact frontend.
- Local JSON-RPC IPC.
- Stream Deck TypeScript plugin scaffold.
- Inno Setup installer and WinSparkle release pipeline.

## Completed Milestones

### M0: Repo Foundation

Complete.

- Initial folder layout.
- Architecture/rules/decision docs.
- Default config seed.

### M1: Tray And Hotkeys

Complete.

- Hidden Win32 shell.
- Tray menu.
- Global recording/replay hotkeys.
- Single-instance guard.

### M2: Capture And Audio Ingress

Complete.

- Windows.Graphics.Capture display capture.
- WASAPI desktop loopback.
- Microphone/input-device capture.
- Process-loopback support where available.

### M3: Replay Buffer

Complete.

- Encoded-packet ring buffer.
- Keyframe-safe replay save.
- MKV clip write path.
- MP4 clip save path exists for replay container setting.

### M4: Manual Recording

Complete for MVP.

- Start/stop/pause/resume state machine.
- Shared encoded-packet stream with replay.
- MKV/MP4 manual writer.
- Pause omits paused time and resumes on next video keyframe.

### M5: Settings Store And UI

Complete, current implementation differs from old WinUI milestone text.

- Settings store is `settings.db`, not live `config.json`.
- UI is Tauri/Preact, not WinUI.
- Legacy `config.json` is import-only migration input.
- Settings reload goes through engine IPC.
- Folder picker, hotkey capture, capability-gated encoder/capture controls, and
  friendly schema are implemented.

### M6: Audio V2 And Active Game

Foundation implemented; runtime soak remains.

- Default desktop + microphone routing.
- Custom source routing to tracks 1-6.
- Per-source volume applied.
- `TrackMixer` for multi-source tracks.
- Active Game source and heuristic detection.
- Foreground-change fast scan and fixed 5s poll cadence.

### M7: Clip Library

Implemented.

- Clip/recs catalogs in SQLite.
- Thumbnails.
- Favorites, hashtags, title, rename, delete, regen thumb.
- UI grid, filters, hover preview, detail view, fullscreen, context menu.
- Live refresh via `clip_generation`/SSE.

### M8: Distribution

Implemented locally; external release setup remains.

- Version flows from tag/CMake into binaries and installer.
- Inno Setup per-user installer.
- WinSparkle integration.
- Appcast generation script.
- CI publishes installer, appcast, and GPL source archive to public release repo.

## Active Work

### Runtime Validation

Needed:

- Long-session replay soak.
- Long-session manual recording soak.
- Default audio track validation.
- Custom mixed audio validation.
- Active Game switch validation.
- Pause/resume A/V sync checks.
- Clean shutdown while saving/recording.

### Test Harness

Needed:

- Settings merge/load/save/migration tests.
- IPC parse/dispatch tests.
- ReplayBuffer save boundary tests.
- Catalog self-heal tests.
- Hotkey parse tests.

### Performance

Priority order:

1. GPU downscale before CPU readback.
2. Output-sized staging textures.
3. Explicit color range/matrix in the CPU encode path.
4. D3D11/NV12 hardware encoder path.
5. AVPacket lifetime/zero-copy mux improvements.

### Release Setup

Done: WinSparkle Ed25519 key pair generated, public key in
`app/recorder/src/updater.cpp`, private key wired into CI as the
`WINSPARKLE_ED_PRIVATE_KEY` secret used by `.github/workflows/version-tag.yml`
to sign the appcast.

External setup still required:

- Create public `fraa2a/Monolith-releases` repo.
- Add CI secret `RELEASES_REPO_PAT`.
- Push first `vX.Y.Z` tag and verify install/update path.

## Not Started Or Deferred

- Desktop Duplication fallback.
- Headless engine process split.
- GPU-resident encoder path.
- Discord Rich Presence implementation.
- Background Discord game catalog refresh.
- Clean-VM update path verification.

## Near-Term Next Actions

1. Run full Release build and UI/plugin builds after this documentation update.
2. Add focused tests for settings and IPC first.
3. Perform runtime soak on default/custom audio.
4. Start GPU downscale-before-readback spike.
5. Complete first public release setup.
