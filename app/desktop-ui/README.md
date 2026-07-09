# Monolith Desktop UI

This is the current Monolith UI sidecar: Tauri v2/WebView2 host plus Preact
frontend. It replaces both the old WinUI settings sidecar and the experimental
Deno Desktop shell.

## Shape

- Host: Rust/Tauri v2 in `src-tauri/`.
- Frontend: Preact in `ui/`.
- Bundler: Vite (`vite.config.ts`).
- Runtime window: WebView2 via Tauri.
- Shipped exe: `Monolith.UI.exe`.

Node/npm is build-only. It is not shipped with Monolith.

## Architecture

The Rust host opens the WebView directly against the bundled frontend
(`WebviewUrl::App`), using native Tauri v2 IPC instead of a loopback HTTP
server. See ADR-0015 in `docs/DECISIONS.md`.

- Frontend calls Rust with `invoke()` (`@tauri-apps/api/core`), dispatched to
  `#[tauri::command]` functions in `commands.rs`.
- Live clip-list refresh uses a native event: the host emits a `clips` event
  when the engine's clip-generation counter advances, the frontend
  `listen()`s for it.
- Clip video/thumbnail playback uses the Tauri asset protocol
  (`convertFileSrc()` over each clip's absolute path) instead of HTTP range
  streaming. The asset-protocol scope is limited to the configured clip/
  recording folders and refreshed after every settings save.
- Window chrome (minimize/maximize/close/drag) is driven directly from the
  frontend via `@tauri-apps/api/window`; there is no Rust-side window
  command.

Rust modules:

- `main.rs`: Tauri bootstrap, command registration, visible-window creation,
  the clip-generation watch thread.
- `commands.rs`: all `#[tauri::command]` handlers invoked from the frontend.
- `asset_scope.rs`: scopes the asset protocol to the current output folders.
- `settings_store.rs`: read/write `%LocalAppData%\Monolith\settings.db`.
- `clip_catalog.rs`: clip/recs catalog queries and direct-write mutations
  (favorite/title/hashtags/rename/delete).
- `engine_rpc.rs`: JSON-RPC client to `Monolith.exe` (recorder control,
  `clip_regen_thumb`, `reload_settings`, status). Unchanged by the IPC
  migration — also used by `plugins/stream-deck`.
- `game_catalog.rs`: game metadata lookup.
- `exe_icon.rs`: native exe icon extraction.
- `paths.rs`: Monolith runtime paths.

The UI writes `clips.db`/`recs.db` directly for favorite/title/hashtag/
rename/delete (same WAL + `busy_timeout` discipline as the engine), so those
work without the recorder running. Only `clip_regen_thumb` still goes through
engine IPC, because thumbnail regeneration decodes a video frame via FFmpeg,
which only the recorder links against.

## Develop

Bundle frontend:

```bat
npm install
npm run build
```

Run host:

```bat
cargo run --manifest-path src-tauri\Cargo.toml
```

The window can open without `Monolith.exe` running. Engine-dependent routes
degrade gracefully.

## Ship

```bat
npm install
npm run build
cargo build --release --manifest-path src-tauri\Cargo.toml
```

The release binary is:

```text
src-tauri\target\release\monolith_ui.exe
```

CMake copies it to:

```text
<recorder-output>\ui\Monolith.UI.exe
```

## Folders

```text
src-tauri/       Rust host
ui/              Preact frontend
vite.config.ts   Vite bundle config
package.json     npm deps/scripts for bundling only
```

## UI Quality Notes

- Keep controls keyboard reachable.
- Keep overlay escape routes obvious.
- Keep icon-only controls labelled.
- Use semantic buttons/inputs over generic clickable containers.
- Keep destructive actions separated and confirmed.
- Respect reduced motion for nonessential animation.
