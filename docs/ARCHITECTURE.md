# Architecture

## Current recommendation

- Start from scratch rather than forking OBS Studio.
- Use a native Windows-first architecture.
- Keep the product recorder-first, not broadcaster-first.

## High-level shape

- UI/tray process:
  - tray icon
  - hotkeys
  - settings UI
  - user notifications
- Engine process:
  - video capture
  - audio capture
  - synchronization
  - encoding/muxing
  - replay buffer
  - recording state machine

## Primary technical choices

- Language: C++23
- Build system: CMake
- Video capture: Windows.Graphics.Capture first, Desktop Duplication as fallback/benchmark
- Audio capture: WASAPI
- Tray/hotkeys: Win32 `Shell_NotifyIcon` and `RegisterHotKey`
- Encoding: FFmpeg/libav family under evaluation as the preferred backend
- Stream Deck: separate plugin folder and local IPC to the native app

## Current implementation state

- Documentation scaffold created
- Native app folder scaffold created
- No real capture, encoding, tray, hotkey, or IPC implementation yet

