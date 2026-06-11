# Active Handover

## Product summary

This project is a lightweight native Windows 11 clipping and recording application. It is intended to run in the background, expose a tray icon and global hotkeys, keep a rolling replay buffer, support manual recording, and later integrate with an Elgato Stream Deck plugin.

## Current phase

- Planning/setup phase
- Repository foundation created
- No production multimedia implementation started

## Recommended architecture

- Recorder-first native Windows architecture
- Prefer custom implementation rather than an OBS fork
- Two-process target design:
  - UI/tray process
  - recording engine process
- Use OBS as a reference/benchmark only

## Chosen stack

- Native language: C++23
- Build system: CMake
- Video capture: Windows.Graphics.Capture first, Desktop Duplication as fallback/benchmark
- Audio capture: WASAPI
- Hotkeys: Win32 `RegisterHotKey`
- Tray: Win32 `Shell_NotifyIcon`
- Encoding direction: FFmpeg/libav preferred, not implemented
- Stream Deck: separate plugin project, local IPC to native app

## What already exists in the repo

- `TASK.md`
- `PROJECT_PLAN.md`
- native app scaffold under `apps/windows-recorder/`
- docs scaffold under `docs/`
- Stream Deck placeholder docs under `plugins/streamdeck/`
- default config draft under `config/default-config.json`
- placeholder script/test readmes

## What has not been implemented yet

- no tray icon logic
- no hotkey registration logic
- no WGC capture code
- no Desktop Duplication fallback
- no WASAPI capture
- no FFmpeg or Media Foundation integration
- no replay buffer
- no recording state machine
- no real IPC server/client
- no Stream Deck plugin code

## First module to implement next

Implement the minimal Win32 tray + hotkey prototype in `apps/windows-recorder/`:

- hidden window/message loop
- tray icon ownership
- one placeholder hotkey registration
- placeholder command dispatch logging

This is the safest first implementation slice because it proves the app lifecycle and control surface without pulling in capture or encoder dependencies.

## Exact next steps for the next AI session

1. Read `TASK.md`, `PROJECT_PLAN.md`, and this handover.
2. Keep scope limited to Milestone 1 only.
3. Implement a minimal Win32 app shell:
   - hidden window
   - message loop
   - tray icon
   - tray menu with placeholder commands
4. Add one configurable hotkey parser stub and register one test hotkey.
5. Add lightweight logging to stdout or a simple file for tray/hotkey events.
6. Do not implement capture, encoding, or replay buffer yet.
7. Update this handover at the end of the session with what changed and what remains.

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

## Decisions still open

- Final IPC transport for v1: named pipes vs localhost TCP
- Final settings UI framework choice
- Exact point where the app splits into two processes during implementation
- Final mux/remux policy details for MP4 output

