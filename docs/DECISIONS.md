# Decisions

This file is the architecture decision record index for the project.

## ADR-0001: Start from scratch instead of forking OBS Studio

- Status: accepted
- Decision:
  - Build a custom native recorder-first application rather than forking OBS Studio.
- Notes:
  - OBS remains a benchmark/reference point, not the product base.

## ADR-0002: Native Windows app instead of Electron/WebView

- Status: accepted
- Decision:
  - Use a native Windows desktop application architecture with no Electron or WebView runtime.

## ADR-0003: C++ and CMake as the initial implementation path

- Status: accepted
- Decision:
  - Use C++23 and CMake for the initial implementation.

## ADR-0004: FFmpeg/libav considered for the encoding backend

- Status: accepted
- Decision:
  - Prefer FFmpeg/libav for encoding and muxing unless early spikes force revision.

## ADR-0005: WASAPI for audio capture

- Status: accepted
- Decision:
  - Use WASAPI for loopback and microphone capture.

## ADR-0006: Win32 hotkeys and tray integration

- Status: accepted
- Decision:
  - Use `RegisterHotKey` for global shortcuts and `Shell_NotifyIcon` for tray integration.

## ADR-0007: Local IPC for Stream Deck plugin communication

- Status: accepted
- Decision:
  - Use local IPC between the native app and Stream Deck plugin.
- Notes:
  - Current docs contain both localhost TCP and named-pipe references.
  - Final transport must be normalized in a future ADR before implementation spreads.

## Open decisions

- Final IPC transport: named pipes vs localhost TCP for v1
- Final UI stack for settings: WinUI 3 vs fallback to Qt Widgets if necessary
- Exact process model timing: single-process prototype first vs immediate two-process split
- Final output/remux policy details for MP4 handling

