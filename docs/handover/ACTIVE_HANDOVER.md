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

## Locked Architecture

- Custom native Windows implementation, not an OBS fork.
- Single-process MVP with isolated `/libs`; two-process engine split deferred.
- C++23 + CMake.
- Windows.Graphics.Capture first; Desktop Duplication fallback later.
- WASAPI for audio capture.
- FFmpeg/libav for encode and mux/remux.
- Stream Deck IPC: localhost WebSocket/TCP JSON-RPC at `127.0.0.1:45991`.
- Stream Deck plugin is a remote controller only; no recording logic in plugin.

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

## Latest Continuation Notes

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

- PCM mixer for multiple simultaneous sources sharing the same output track.
- Long-session A/V sync soak tests for the new multi-track audio path.
- Strong game detection; Active Game is foreground-process best effort.
- True live audio re-route without restarting the audio pipeline.
- Real IPC server/client.
- Stream Deck plugin code.
- Desktop Duplication fallback.
- GPU-resident encoder path; current spike uses CPU-readable BGRA staging.
- MP4 replay-clip remux support; replay clips are MKV only.
- Automated tests for replay/manual recording.

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

## Next Steps

1. Add a real PCM mixer so multiple sources can safely share the same audio track.
2. Runtime-test Default mode: desktop track 1 plus microphone track 2 in replay and manual recording.
3. Runtime-test Custom mode with multiple input devices, process loopback, source removal, missing devices/processes, and save-triggered pipeline reload.
4. Add a small integration/unit test harness for settings merge/load behavior, audio routing validation, and `ReplayBuffer::save_clip`.
5. Continue runtime hardening: graceful shutdown, capture stop races, encoder failure UX/logs, and A/V sync checks.

## Open Risks

- Capture-to-encoder path still performs CPU readback; acceptable for spike, not final performance target.
- WGC callback and encoder flow need runtime soak testing.
- A/V sync needs real capture validation, especially around silence and long sessions.
- Manual recording pause/resume needs longer-session validation.
- WGC border suppression requires `GraphicsCaptureSession::IsBorderRequired` / `IGraphicsCaptureSession3`, introduced in `Windows.Foundation.UniversalApiContract` 12.0. The local SDK exposes it under Windows Kits `10.0.26100.0`.
- Per-app audio capture and active-game detection are expected to be best-effort on Windows and must fail closed without breaking default recording.
- WinUI 3 cold process startup is not eliminated; current work optimizes avoidable app-side work after process start.
