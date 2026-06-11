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

- Documentation scaffold created
- Native app folder scaffold created
- Scaffold reorganised to /app + /libs + /plugins layout
- No real capture, encoding, tray, hotkey, or IPC implementation yet

