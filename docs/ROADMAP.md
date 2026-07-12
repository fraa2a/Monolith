# Roadmap

## Current Status

Monolith past early prototype. Baseline now:

- Native Win32 tray app, hotkeys.
- WGC video capture, WASAPI audio capture.
- FFmpeg encode, mux.
- Replay buffer, manual recording.
- SQLite clip catalogs, thumbnails.
- SQLite `settings.db` settings store.
- Tauri v2/WebView2 desktop UI, Preact frontend.
- Local JSON-RPC IPC.
- Stream Deck TypeScript plugin scaffold.
- Inno Setup installer, WinSparkle release pipeline.

## Completed Milestones

### M0: Repo Foundation

Done.

- Initial folder layout.
- Architecture/rules/decision docs.
- Default config seed.

### M1: Tray And Hotkeys

Done.

- Hidden Win32 shell.
- Tray menu.
- Global recording/replay hotkeys.
- Single-instance guard.

### M2: Capture And Audio Ingress

Done.

- Windows.Graphics.Capture display capture.
- WASAPI desktop loopback.
- Microphone/input-device capture.
- Process-loopback where available.

### M3: Replay Buffer

Done.

- Encoded-packet ring buffer.
- Keyframe-safe replay save.
- MKV clip write path.
- MP4 clip save path exist for replay container setting.

### M4: Manual Recording

Done for MVP.

- Start/stop/pause/resume state machine.
- Shared encoded-packet stream with replay.
- MKV/MP4 manual writer.
- Pause skip paused time, resume on next video keyframe.

### M5: Settings Store And UI

Done. Current build differ from old WinUI milestone text.

- Settings store now `settings.db`, not live `config.json`.
- UI now Tauri/Preact, not WinUI.
- Legacy `config.json` import-only migration input.
- Settings reload go through engine IPC.
- Folder picker, hotkey capture, capability-gated encoder/capture controls, friendly schema all built.

### M6: Audio V2 And Active Game

Foundation built; runtime soak remain.

- Default desktop + microphone routing.
- Custom source routing to tracks 1-6.
- Per-source volume applied.
- `TrackMixer` for multi-source tracks.
- Active Game source, heuristic detection.
- Foreground-change fast scan, fixed 5s poll cadence.

### M7: Clip Library

Built.

- Clip/recs catalogs in SQLite.
- Thumbnails.
- Favorites, hashtags, title, rename, delete, regen thumb.
- UI grid, filters, hover preview, detail view, fullscreen, context menu.
- Live refresh via `clip_generation`/SSE.

### M8: Distribution

Built local; external release setup remain.

- Version flow from tag/CMake into binaries, installer.
- Inno Setup per-user installer.
- WinSparkle integration.
- Appcast generation script.
- CI publish installer, appcast, GPL source archive to public release repo.

## Active Work

### Runtime Validation

Need:

- Long-session replay soak.
- Long-session manual recording soak.
- Default audio track validation.
- Custom mixed audio validation.
- Active Game switch validation.
- Pause/resume A/V sync checks.
- Clean shutdown while saving/recording.

### Test Harness

Need:

- Settings merge/load/save/migration tests.
- IPC parse/dispatch tests.
- ReplayBuffer save boundary tests.
- Catalog self-heal tests.
- Hotkey parse tests.

### Performance

Priority order:

1. GPU downscale before CPU readback.
2. Output-sized staging textures.
3. Explicit color range/matrix in CPU encode path.
4. D3D11/NV12 hardware encoder path.
5. AVPacket lifetime/zero-copy mux improvements.

### Release Setup

Done: WinSparkle Ed25519 key pair generated, public key in
`app/recorder/src/updater.cpp`, private key wired into CI as the
`WINSPARKLE_ED_PRIVATE_KEY` secret used by `.github/workflows/version-tag.yml`
to sign appcast.

External setup still need:

- Create public `fraa2a/Monolith-releases` repo.
- Add CI secret `RELEASES_REPO_PAT`.
- Push first `vX.Y.Z` tag, verify install/update path.

## Not Started Or Deferred

- Desktop Duplication fallback.
- Headless engine process split.
- GPU-resident encoder path.
- Discord Rich Presence.
- Background Discord game catalog refresh.
- Clean-VM update path verification.

## Near-Term Next Actions

1. Run full Release build, UI/plugin builds after this docs update.
2. Add focused tests for settings, IPC first.
3. Do runtime soak on default/custom audio.
4. Start GPU downscale-before-readback spike.
5. Finish first public release setup.