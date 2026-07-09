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

The Rust host starts a loopback HTTP server and opens the WebView at
`http://127.0.0.1:<port>/`. This keeps the frontend route contract simple:

- Static app assets.
- `/api/*` JSON routes.
- `/media/*` range streaming for video playback.
- `/thumb/*` thumbnail responses.
- `/api/events` Server-Sent Events for clip refresh.

Rust modules:

- `main.rs`: Tauri bootstrap and visible-window creation.
- `server.rs`: HTTP route handling.
- `media.rs`: video and thumbnail streaming.
- `settings_store.rs`: read/write `%LocalAppData%\Monolith\settings.db`.
- `clip_catalog.rs`: read-only clip/recs catalog queries.
- `engine_rpc.rs`: JSON-RPC client to `Monolith.exe`.
- `game_catalog.rs`: game metadata lookup.
- `paths.rs`: Monolith runtime paths.

The UI never writes `clips.db` or `recs.db` directly. Mutations go through engine
IPC so the recorder remains the single writer.

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
