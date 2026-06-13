# Architecture

## Current recommendation

- Start from scratch rather than forking OBS Studio.
- Use a native Windows-first architecture.
- Keep the product recorder-first, not broadcaster-first.

## High-level shape

### Single-process MVP

- app shell (tray, hotkeys, settings UI)
- capture module (libs/capture)
- audio module (libs/audio)
- encoder module (libs/encoding)
- mux module (libs/mux)
- replay buffer (libs/replay-buffer)
- recording state machine (libs/domain)
- IPC server (libs/ipc)
- config (libs/config)
- logging (libs/logging)

### Future split target

Once the MVP is stable the recording engine (capture through replay) will be extracted into
a separate headless process. The UI/tray process and engine process model remains the
long-term target; the split is deferred to reduce early IPC complexity.

- UI/tray process (app/desktop-ui + app shell)
- Headless recording engine process (app/engine)

## Primary technical choices

- Language: C++23
- Build system: CMake
- Video capture: Windows.Graphics.Capture first, Desktop Duplication as fallback/benchmark
- Audio capture: WASAPI
- Tray/hotkeys: Win32 `Shell_NotifyIcon` and `RegisterHotKey`
- Encoding: FFmpeg/libav family under evaluation as the preferred backend
- Stream Deck: separate plugin folder and local IPC to the native app

## Current implementation state

- Native app lives in `app/recorder` as the single-process MVP.
- Win32 tray, message-only window, single-instance guard, and global hotkeys are implemented.
- WGC display capture, WASAPI loopback/microphone/input-device ingress, FFmpeg H.264/AAC encoding, replay-buffer MKV clip save, and manual recording are implemented.
- User-facing branding is Monolith. User video output defaults under `Videos\Monolith`; logs/runtime data default under `AppData\Local\Monolith`.
- WinUI 3 Settings is implemented as `Monolith.Settings.exe`; it opens from the tray, persists config under `AppData\Local\Monolith\config.json`, lazy-loads expensive runtime status pages, and posts a settings-reload message to the recorder after save.
- Audio V2 foundation is implemented: Default mode routes desktop audio to track 1 and the selected microphone to track 2; Custom mode persists source routing for desktop audio, input devices, process sessions, and Active Game; muxers can create up to six logical audio tracks.
- IPC server/client, Stream Deck plugin, Desktop Duplication fallback, and GPU-resident encoder path are still pending.

## Settings and Apply Scopes

- Settings is a sidecar process launched by the recorder, not a hidden always-on background process.
- First-window startup avoids audio/session/device/monitor enumeration. Audio and Capture page data is loaded on first navigation to those pages.
- Startup instrumentation is written to `AppData\Local\Monolith\settings-startup.log`.
- `Save` writes `config.json` and posts `WM_APP + 2` to the recorder message window.
- Live settings: output folders and hotkeys apply to future saves/commands without restarting the app.
- Replay settings: duration and memory budget restart/reconfigure the replay buffer and clear old packets to avoid mixing packet streams.
- Audio settings: mode, primary microphone, and custom source routing restart the audio pipeline when no manual recording is active.
- Capture/encoder settings: monitor, resolution, border, bitrate, backend, and FFmpeg options restart the capture/encoder pipeline when no manual recording is active.
- Unsafe pipeline changes are deferred while manual recording is active. Full app restart should be reserved for future settings that truly cannot be reloaded safely.

## Audio V2 Implementation Notes

- Default mode records all desktop/system audio to track 1 and the selected primary microphone to track 2 when the devices start successfully.
- Custom mode supports up to six logical audio tracks and lets one source route to multiple tracks.
- Sources include all desktop audio, multiple microphone/input devices, detected process audio sessions, and a best-effort Active Game source.
- Detected sources are separate from configured sources: removing a configured source deletes its saved routing entry, while detected devices/apps can still be added later.
- Per-process and Active Game capture use Windows process-loopback activation and fail closed if unsupported or unavailable.
- Missing devices, closed processes, and no active game are tolerated without crashing or corrupting config.
- Current limitation: multiple sources assigned to the same track are not mixed yet; the runtime logs and skips the later source for that track to avoid corrupt timing.
- The muxer/encoder layer owns encoded stream creation; Settings only exposes controls that are wired, read-only, or explicitly marked with an apply scope.

## Active Game Detection (v0.5.1)

- `audio::detect_active_game(DetectConfig)` in `libs/audio/audio.cpp` scores candidate windows with explicit bonuses (+4 fullscreen, +4 foreground, +2 active audio session, +8 whitelist/manual). Blacklisted exes are rejected before scoring. Returns `ActiveGameResult` with process info, score, confidence (0–100), reason string, session/fullscreen flags.
- Config-driven blacklist/whitelist/manual_games loaded from the `active_game` JSON block, clamped in `settings_config.cpp`. Default poll 30 s; switch debounce 3 s; min_confidence 50.
- `poll_active_game()` in `main.cpp` is the single decision point called by both the WM_TIMER (30 s) and WM_FAST_SCAN (foreground change, rate-limited ≥1 s). It re-evaluates every tick, applies debounce, and swaps **only** the Active Game WasapiCapture when a switch is confirmed. Replay buffer and other sources are never restarted.
- Fast scan: `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT)` installed on the UI thread; callback posts `WM_FAST_SCAN` to the main HWND with coalescing via `g_fast_scan_pending`. Rate limit enforced in the WM_FAST_SCAN handler.
- Runtime status: `settings::ActiveGameStatus` (extended from `RuntimeAudioSession`) includes confidence, reason, capture_mode, process_loopback_available, last_switch_time, poll_interval_ms, fast_scan_enabled. Written to `runtime-status.json` and read by Settings.
- Settings: Custom Sources panel hidden in Default mode. Default → Custom confirmation dialog. Active Game status panel in Custom mode shows live detection info.
- Honest limitations: detection is heuristic; process-loopback requires Windows 10 21H2+; no OS mute/volume mutation; no claiming perfect detection in docs or UI.

