# Active Handover

## Product summary

This project is a lightweight native Windows 11 clipping and recording application. It is intended to run in the background, expose a tray icon and global hotkeys, keep a rolling replay buffer, support manual recording, and later integrate with an Elgato Stream Deck plugin.

## Current phase

- Milestone 1 complete: Win32 tray + hotkey app shell
- Milestone 2 complete: WGC display capture + WASAPI loopback + mic — ingress proven, log output confirmed

## Recommended architecture

- Recorder-first native Windows architecture
- Prefer custom implementation rather than an OBS fork
- Single-process MVP with isolated internal modules (/libs); two-process target is deferred
- Use OBS as a reference/benchmark only

## Chosen stack

- Native language: C++23
- Build system: CMake
- Video capture: Windows.Graphics.Capture first, Desktop Duplication as fallback/benchmark
- Audio capture: WASAPI
- Hotkeys: Win32 `RegisterHotKey`
- Tray: Win32 `Shell_NotifyIcon`
- Encoding direction: FFmpeg/libav preferred, not implemented
- IPC transport: localhost WebSocket/TCP JSON-RPC (host 127.0.0.1, port 45991)
- Stream Deck: separate plugin project under plugins/stream-deck/, remote controller only

## What already exists in the repo

- `TASK.md`
- `PROJECT_PLAN.md`
- native app scaffold under `app/recorder/` (single MVP executable)
- future placeholders under `app/engine/` and `app/desktop-ui/`
- library placeholders under `libs/` (capture, audio, encoding, mux, replay-buffer, config, ipc, platform-win, logging, domain)
- docs scaffold under `docs/`
- Stream Deck placeholder docs under `plugins/stream-deck/`
- default config draft under `config/default-config.json` (snake_case rich schema, schema_version 1)
- root `CMakeLists.txt` wiring `app/recorder/`
- placeholder script/test readmes

## What has been implemented

- `app/recorder/src/main.cpp`: Win32 app shell (WinMain, HWND_MESSAGE window, tray, hotkey, COM/WinRT init)
- System tray icon with context menu (Save Replay, Start/Stop Recording, Pause/Resume, Settings grayed, Exit)
- Global hotkey: Ctrl+Shift+F8 → save_replay (MOD_NOREPEAT)
- Single-instance mutex guard
- Thread-safe structured log → `%LOCALAPPDATA%\WindowsRecorder\recorder.log` + OutputDebugString
- `libs/capture/`: WGC display capture (CreateFreeThreaded, D3D11, GPU-resident frames, size-change Recreate)
- `libs/audio/`: WASAPI loopback + mic (polling capture thread, PacketInfo callbacks, mix format auto-detect)

## What has not been implemented yet

- no WGC capture code
- no Desktop Duplication fallback
- no WASAPI capture
- no FFmpeg or Media Foundation integration
- no replay buffer
- no recording state machine
- no real IPC server/client
- no Stream Deck plugin code
- Settings window (grayed in tray menu)

## Next module to implement

Milestone 3: Replay Buffer Proof of Concept (ROADMAP.md)
- `libs/encoding/`: FFmpeg/libav wrapper — video encode (H.264 via NVENC/AMF/QSV/software fallback) + AAC audio
- `libs/replay-buffer/`: encoded-packet ring buffer with keyframe index, configurable duration + memory cap
- Clip save path: walk back to nearest keyframe, remux forward into MKV output file
- Wire into main.cpp: encode incoming WGC frames + WASAPI packets, feed ring buffer, Ctrl+Shift+F8 triggers clip save

## Exact next steps for the next AI session

1. Read `PROJECT_PLAN.md` §2 subsystems 3–6 and this handover.
2. Add FFmpeg (via vcpkg or prebuilt) to the build.
3. Implement `libs/encoding/` — encoder capability probe (NVENC/AMF/QSV/software), encode video + audio.
4. Implement `libs/replay-buffer/` — ring buffer over `AVPacket`, keyframe index, clip materializer.
5. Wire into main.cpp: start encode pipeline after capture/audio init.
6. Test: Ctrl+Shift+F8 → MKV clip saved to output.clips_directory.
7. Update this handover at end of session.

## Known risks and blockers

- IPC transport is not fully normalized yet across docs.
- Settings UI stack is not finally locked if WinUI 3 friction becomes high.
- OBS-informed benchmarking is still planned, not executed.
- Replay buffer design depends on encoder-path decisions that are still ahead.
- Capture/encoding complexity remains the main technical risk area.

## Decisions already made

- Start from scratch instead of forking OBS Studio
- Native Windows app, no Electron/WebView
- C++23 + CMake as the initial implementation path
- WASAPI for audio capture
- Win32 tray and global hotkeys
- FFmpeg/libav as the preferred encoding direction
- IPC transport for v1: localhost WebSocket/TCP JSON-RPC (ADR-0007)
- Process model for MVP: single-process with modular internals (ADR-0008)

## Decisions still open

- Final settings UI framework choice (WinUI 3 vs Qt Widgets)
- Final mux/remux policy details for MP4 output

