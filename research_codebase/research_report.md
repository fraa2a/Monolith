# Research Report: Monolith Codebase

Date: 2026-07-08

## Answer

Monolith is currently a Windows-native recorder app with a Tauri/Preact UI
sidecar and a Stream Deck controller plugin. The codebase has advanced past many
older documentation claims, so the documentation needed broad synchronization.

## Key Facts

- Recording engine: `app/recorder`, C++23, Win32 tray/message loop.
- UI: `app/desktop-ui`, Tauri v2/WebView2 host, Preact frontend, Deno build-only.
- Settings: `%LocalAppData%\Monolith\settings.db`.
- Clip catalogs: `clips.db` and `recs.db` beside output folders.
- IPC: JSON-RPC TCP on `127.0.0.1:45991`.
- Plugin: `plugins/stream-deck`, TypeScript, Elgato SDK, implemented scaffold.
- Release: Inno Setup installer, WinSparkle appcast, public release repo flow.

## Important Corrections Applied

- Replaced WinUI/`Monolith.Settings.exe` language with Tauri/`Monolith.UI.exe`.
- Reframed Deno as a build-only frontend bundler.
- Replaced live `config.json` language with `settings.db` migration language.
- Updated Stream Deck docs from "reserved/not implemented" to current scaffold.
- Updated release docs to remove stale .NET/WinAppSDK sidecar assumptions.
- Updated roadmap and handover around current MVP-hardening state.

## Gaps

- No broad automated test suite.
- Runtime soak tests still needed.
- First public release/update path still blocked by external setup.
- Video path still has high CPU/RAM copy cost.

## Sources

Local repository files:

- `CMakeLists.txt`
- `vcpkg.json`
- `config/default-config.json`
- `app/recorder/src/main.cpp`
- `app/recorder/src/settings_config.cpp`
- `libs/ipc/ipc_server.*`
- `libs/storage/storage.*`
- `app/desktop-ui/README.md`
- `app/desktop-ui/src-tauri/*`
- `plugins/stream-deck/*`
- Existing Markdown docs under root, `docs/`, `scripts/`, `app/desktop-ui/`,
  and `plugins/stream-deck/`.
