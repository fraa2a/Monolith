# Findings: Codebase State

## Engine

The native engine is `app/recorder`. It owns the Win32 tray/message loop,
hotkeys, capture, audio, encode, replay, manual recording, clip catalog writes,
settings reload, runtime status, updater, and IPC server.

The engine uses C++23, CMake, and vcpkg dependencies. Current major libraries:
`nlohmann-json`, `sqlite3`, `winsparkle`, and FFmpeg with hardware encoder
features.

## UI

The current desktop UI is Tauri v2/WebView2, not WinUI and not Deno Desktop.
Deno remains only as the frontend bundler for a Preact UI.

The Rust host serves embedded frontend assets and local API/media routes. It
uses `rusqlite` with bundled SQLite and talks to the engine over JSON-RPC.

## Storage

Settings store of record is `%LocalAppData%\Monolith\settings.db`.
`config/default-config.json` is the default seed/fallback. Legacy `config.json`
is imported once and renamed.

Clip catalogs are SQLite DBs beside media folders: `clips.db` and `recs.db`.
The engine is the single writer; UI mutations go through IPC.

## IPC

`libs/ipc` exposes newline-delimited JSON-RPC over TCP on `127.0.0.1:45991`.
It handles recorder commands, settings reload, status, and clip mutations.

## Stream Deck

`plugins/stream-deck` is implemented as a TypeScript plugin scaffold. It has
Save Replay, Toggle Recording, and Pause/Resume actions plus a reconnecting IPC
client.

## Documentation Drift

Stale areas found:

- WinUI settings sidecar references.
- Deno Desktop as shipped shell.
- `config.json` as live settings store.
- Stream Deck described as not implemented.
- Active Game 30s/configurable poll language after current 5s fixed cadence.
- Release docs mentioning .NET/WinAppSDK payload after Tauri replacement.
