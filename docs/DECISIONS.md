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

- Status: accepted / resolved
- Decision:
  - v1 IPC transport = localhost WebSocket/TCP JSON-RPC (JSON-RPC over TCP).
  - Named pipes deferred; rationale preserved for future evaluation.
- Notes:
  - host: 127.0.0.1, port: 45991 (matches default-config.json).

## ADR-0008: Single-process MVP with modular internals

- Status: accepted
- Decision:
  - Build the MVP as a single executable (app/recorder) with strict internal module
    boundaries enforced by /libs separation. Two-process split is the future target
    once the MVP is stable.
- Notes:
  - Deferred rationale: crash isolation and independent restart remain the long-term goal.

## Open decisions

- Final UI stack for settings: WinUI 3 vs fallback to Qt Widgets if necessary
- Final output/remux policy details for MP4 handling

