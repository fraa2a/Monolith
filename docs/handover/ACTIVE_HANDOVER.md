# Active Handover

## Product Summary

This project is Monolith, a lightweight native Windows 11 clipping and recording application. It runs in the background, exposes a tray icon and global hotkeys, keeps a rolling replay buffer, supports manual recording, and will integrate with an Elgato Stream Deck plugin later.

## Current Phase

- Milestone 1 complete: Win32 tray + hotkey app shell.
- Milestone 2 complete: WGC display capture + WASAPI loopback + mic ingress.
- Milestone 3 complete: FFmpeg encode + encoded-packet replay buffer save a valid MKV clip from the tray/hotkey app shell.
- Milestone 4 in progress: manual recording controls and a minimal recording state machine have been added; build/runtime verification is the next checkpoint.

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
- `libs/audio`: WASAPI loopback and mic capture with packet callbacks and mix-format detection.
- `libs/encoding`: FFmpeg H.264 probing/open path (`h264_nvenc`, `h264_amf`, `h264_qsv`, `libx264`), BGRA-to-YUV video encode, AAC audio encode with resampling.
- `libs/replay-buffer`: encoded-packet ring buffer, keyframe-based clip start, async Matroska clip writer.
- `libs/recording`: live MKV writer for manual recordings, fed by the same encoded packet stream as replay.
- App wiring: WGC frames and WASAPI system audio feed encoders; encoded packets feed replay buffer and, when armed, the manual recorder.
- User media output defaults to `Videos\Monolith\Clips` and `Videos\Monolith\Recordings`; logs/runtime data stay under `AppData\Local\Monolith`.

## Latest Continuation Notes

- `libs/capture/capture.cpp`: changed WGC surface QI from `as<IDirect3DDxgiInterfaceAccess>()` to `try_as<...>()` so a missing interface does not throw on the frame callback thread.
- `libs/audio/audio.cpp`: silent WASAPI packets now preserve `data_bytes`.
- `libs/encoding/encoding.cpp`: `AudioEncoder::push_pcm` now converts null PCM input into zeroed silence, preserving AAC timeline during silent stretches.
- `app/recorder/src/main.cpp`: system-audio encoder now receives silent packets too.
- `libs/replay-buffer/replay_buffer.cpp`: MKV writer now copies packet bytes into `AVPacket` via `av_new_packet` instead of borrowing vector memory.
- `libs/encoding/encoding.cpp`: video/audio encoder public methods are now mutex-protected because WGC and WASAPI drive them from different worker threads.
- Build tooling was installed and verified: CMake 4.3.3, Visual Studio Build Tools 2022 with MSVC, and local ignored `vcpkg/`.
- `vcpkg.json` now uses valid FFmpeg features: `avcodec`, `avformat`, `swscale`, `swresample`, `amf`, `nvcodec`, `qsv`, `x264`, `gpl`.
- Release build succeeds: `build/app/recorder/Release/windows_recorder.exe`.
- Runtime smoke test succeeds: app starts, WGC captures `2560x1600`, H.264 opens lazily from the first real WGC frame, `Ctrl+Shift+F8` saves `20260611_064508_30s_clip.mkv`.
- Fix: video encoder is now opened lazily from actual WGC frame dimensions instead of `GetMonitorInfoW` virtual DPI coordinates, which previously caused all 2560x1600 frames to be skipped by a 1706x1066 encoder.
- Legacy app branding changed to Monolith for user-facing app identity and runtime folders.
- Manual recording start/stop/pause/resume now has accepted/rejected state transitions instead of log-only tray stubs.
- Manual recording pause omits paused time from the output file and resumes writing on the next video keyframe.

## Not Implemented Yet

- Real IPC server/client.
- Stream Deck plugin code.
- Settings window; tray menu item remains disabled.
- Desktop Duplication fallback.
- GPU-resident encoder path; current spike uses CPU-readable BGRA staging.
- Microphone mixing into replay/manual recording output; mic is logged only.
- MP4 remux policy details.
- Automated tests for replay/manual recording.

## Build/Verification

Previous native C++ build verification succeeded in the earlier environment. Current verification should use:

```powershell
& '<cmake>\cmake.exe' -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="<vcpkg>\scripts\buildsystems\vcpkg.cmake"
& '<cmake>\cmake.exe' --build build --config Release
```

## Next Steps

1. Build and smoke-test replay save plus manual recording start/stop/pause/resume on the Windows dev machine.
2. Add a small integration/unit test harness for `ReplayBuffer::save_clip` and the manual recording writer.
3. Continue runtime hardening: graceful shutdown, capture stop races, encoder failure UX/logs, and A/V sync checks.

## Open Risks

- Capture-to-encoder path still performs CPU readback; acceptable for spike, not final performance target.
- WGC callback and encoder flow need runtime soak testing.
- A/V sync needs real capture validation, especially around silence and long sessions.
- Manual recording pause/resume needs runtime validation across longer sessions.
- Settings UI framework is still WinUI 3 preferred, Qt fallback if Windows App SDK friction is too high.
