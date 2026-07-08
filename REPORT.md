# Monolith Codebase Exploration Report

Date: 2026-07-08

## Executive Summary

Monolith is no longer just the early tray/capture prototype described by parts
of the older docs. The repository now contains a working native recorder core,
a Tauri/Preact desktop UI, SQLite-backed settings and clip catalogs, and a
TypeScript Stream Deck plugin scaffold that already speaks to the local IPC
server.

The main documentation drift found during this pass:

- Several docs still referenced WinUI 3 and `Monolith.Settings.exe`.
- Several docs still treated Deno Desktop as the shipped UI shell, while the
  current shell is Tauri v2/WebView2.
- Some docs still described `config.json` as the live settings store; current
  code uses `settings.db` and only imports legacy `config.json`.
- Stream Deck docs said "not implemented", but the plugin has TypeScript
  actions, package scripts, and JSON-RPC client code.
- Active Game docs mixed old 30s configurable polling with the current fixed
  5s engine cadence and scrubbed timing tunables.

All Markdown docs have been updated to reflect the current code shape.

## Repository Map

```text
app/recorder/          Native C++ Win32 tray app and recording orchestrator
app/desktop-ui/        Tauri v2/WebView2 host plus Preact frontend
libs/capture/          Windows.Graphics.Capture D3D11 capture
libs/audio/            WASAPI capture and active-game detection
libs/encoding/         FFmpeg encode, mux helpers, thumbnails, audio mixer
libs/replay-buffer/    Encoded-packet ring buffer and clip save
libs/recording/        Manual recording state and writer
libs/storage/          SQLite clip catalogs and settings.db store
libs/ipc/              Local JSON-RPC TCP server
plugins/stream-deck/   Elgato Stream Deck Node/TypeScript plugin
installer/             Inno Setup packaging
scripts/               Release helper scripts
docs/                  Architecture, ADRs, release, roadmap, handover
config/                Default settings seed
```

## Current Architecture

The app is still a single-process MVP for recording, but it has a separate UI
sidecar process:

- `Monolith.exe` owns capture, encode, replay, recording, hotkeys, tray, catalog
  writes, settings reload, runtime status, and WinSparkle.
- `Monolith.UI.exe` is a Tauri v2 host. It embeds the Preact frontend, runs a
  loopback HTTP server, reads clip catalogs read-only, writes `settings.db`, and
  calls the engine over JSON-RPC.
- The Stream Deck plugin is a remote controller only and has no recording logic.

The recording pipeline is still CPU-readable-frame based:

```text
WGC texture
  -> D3D11 staging BGRA readback
  -> pacer BGRA buffer
  -> FFmpeg BGRA to encoder pixel format
  -> encoded packets
  -> replay buffer and manual recorder
```

This is functional but remains the main CPU/RAM performance target.

## Build System

- Root native build: CMake 3.27+, C++23, Visual Studio 2022 generator.
- Dependencies: vcpkg manifest mode.
- UI build: Deno bundles frontend; Cargo builds Tauri host.
- Plugin build: npm/TypeScript.
- Installer: Inno Setup, tag-driven versioning, WinSparkle appcast generation.

## Runtime Data Model

- Settings store: `%LocalAppData%\Monolith\settings.db`.
- Clip catalogs: `clips.db` / `recs.db` beside media output folders.
- Thumbnails: `.thumbs\` under each output folder.
- Runtime status: `%LocalAppData%\Monolith\runtime-status.json`.
- Logs: `%LocalAppData%\Monolith\monolith.log`.

## UI/UX Review Notes

The current UI direction is suitable for a recorder/library product:

- Clip grid and settings popup are task-oriented rather than marketing-style.
- Deno is correctly limited to frontend bundling; runtime UI shell is Tauri.
- UI reads catalogs and routes mutations through the engine, preserving the
  single-writer rule.
- `audioTracks` support is correctly treated as WebView2-dependent.

Open UI quality risks:

- Keep all icon-only controls labelled for accessibility.
- Preserve keyboard focus and Escape/close behavior in overlays/dialogs.
- Keep touch/click targets at least 44px where controls are compact.
- Do not rely only on color for delete/favorite/status state.
- Continue replacing stale emoji-like labels in plugin key titles with image
  assets or plain text where Stream Deck glyph rendering is inconsistent.

## Primary Risks

1. GPU/CPU copy cost remains high because capture still maps BGRA frames to CPU.
2. Long-session audio/video sync soak testing is still needed.
3. Release one-time setup is still external-state blocked until public repo,
   Ed25519 keys, and CI secrets exist.
4. Clean-VM installer/update verification remains pending.
5. Documentation must be kept in lockstep with ADR-0012/ADR-0013 because those
   rewrote the UI shell, settings store, IPC, and schema.

## Recommended Next Engineering Work

1. Add targeted tests for settings load/save/migration and IPC method parsing.
2. Add a small replay-buffer save harness around keyframe and packet retention.
3. Runtime soak test: default audio, custom mixed audio, active-game switching,
   manual recording pause/resume, and long clips.
4. Start GPU downscale-before-readback as the best risk/reward performance step.
5. Later, add D3D11 hardware encoder path and keep BGRA CPU path as fallback.

## Documentation Updates Made

All existing Markdown files were updated:

- `CLAUDE.md`
- `REPORT.md`
- `docs/ARCHITECTURE.md`
- `docs/DECISIONS.md`
- `docs/DEVELOPMENT_RULES.md`
- `docs/ROADMAP.md`
- `docs/RELEASING.md`
- `docs/handover/ACTIVE_HANDOVER.md`
- `scripts/README.md`
- `app/desktop-ui/README.md`
- `plugins/stream-deck/README.md`
- `plugins/stream-deck/manifest-notes.md`
- `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/imgs/README.md`

Research notes were added under `research_codebase/`.
