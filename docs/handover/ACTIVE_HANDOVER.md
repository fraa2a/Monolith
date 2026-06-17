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
