# Active Handover

## Product Summary

This project is a lightweight native Windows 11 clipping and recording application. It runs in the background, exposes a tray icon and global hotkeys, keeps a rolling replay buffer, supports manual recording later, and will integrate with an Elgato Stream Deck plugin.

## Current Phase

- Milestone 1 complete: Win32 tray + hotkey app shell.
- Milestone 2 complete: WGC display capture + WASAPI loopback + mic ingress.
- Milestone 3 in progress: FFmpeg encode + encoded-packet replay buffer are wired into the app shell.

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
- Global hotkey: `Ctrl+Shift+F8` triggers `save_replay`.
- `libs/capture`: WGC display capture via `CreateFreeThreaded`, D3D11 device, staging BGRA readback, size-change pool recreate.
- `libs/audio`: WASAPI loopback and mic capture with packet callbacks and mix-format detection.
- `libs/encoding`: FFmpeg H.264 probing/open path (`h264_nvenc`, `h264_amf`, `h264_qsv`, `libx264`), BGRA-to-YUV video encode, AAC audio encode with resampling.
- `libs/replay-buffer`: encoded-packet ring buffer, keyframe-based clip start, async Matroska clip writer.
- App wiring: WGC frames and WASAPI system audio feed encoders; encoded packets feed replay buffer; hotkey snapshots buffer and writes a clip.

## Latest Continuation Notes

- `libs/capture/capture.cpp`: changed WGC surface QI from `as<IDirect3DDxgiInterfaceAccess>()` to `try_as<...>()` so a missing interface does not throw on the frame callback thread.
- `libs/audio/audio.cpp`: silent WASAPI packets now preserve `data_bytes`.
- `libs/encoding/encoding.cpp`: `AudioEncoder::push_pcm` now converts null PCM input into zeroed silence, preserving AAC timeline during silent stretches.
- `app/recorder/src/main.cpp`: system-audio encoder now receives silent packets too.
- `libs/replay-buffer/replay_buffer.cpp`: MKV writer now copies packet bytes into `AVPacket` via `av_new_packet` instead of borrowing vector memory.

## Not Implemented Yet

- Native build verification on this machine.
- Recording state machine.
- Real IPC server/client.
- Stream Deck plugin code.
- Settings window; tray menu item remains disabled.
- Desktop Duplication fallback.
- GPU-resident encoder path; current spike uses CPU-readable BGRA staging.
- Microphone mixing into replay output; mic is logged only.
- MP4 remux policy details.

## Build/Verification Blocker

On 2026-06-11, `dotnet` existed, but `cmake` was not on PATH and `vswhere` did not report an MSBuild installation. Native C++ build verification could not be completed in this continuation.

## Next Steps

1. Install or expose CMake + MSVC Build Tools.
2. Configure with vcpkg manifest mode and verify `find_package(FFMPEG REQUIRED)` resolves.
3. Build `windows_recorder`.
4. Run the app and test `Ctrl+Shift+F8`.
5. Verify a valid MKV appears under `%LOCALAPPDATA%\WindowsRecorder\Clips`.
6. If FFmpeg API/version errors appear, fix `libs/encoding` and `libs/replay-buffer` first.
7. Add a small integration/unit test harness for `ReplayBuffer::save_clip` once build tooling is available.

## Open Risks

- Capture-to-encoder path still performs CPU readback; acceptable for spike, not final performance target.
- WGC callback and encoder flow need runtime soak testing.
- A/V sync needs real capture validation, especially around silence and long sessions.
- Settings UI framework is still WinUI 3 preferred, Qt fallback if Windows App SDK friction is too high.
