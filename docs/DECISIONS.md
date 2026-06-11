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

## ADR-0009: First settings UI uses Win32, config uses JSON

- Status: accepted
- Decision:
  - Use `config/default-config.json` as the default/sample schema.
  - Store the per-user config at `AppData\Local\Monolith\config.json`.
  - Use `nlohmann-json` for parsing, merging defaults with user overrides, and writing user config.
  - Use a minimal native Win32 settings window for the first pass.
- Notes:
  - WinUI 3 remains a future UI option, but it is deferred to avoid Windows App SDK churn while the recorder core is still stabilizing.
  - Hotkey rebinding is deferred; current shortcuts are displayed read-only.

## Open decisions

- Final output/remux policy details for MP4 handling

