# Monolith Desktop UI (Tauri)

New UI layer — Home = clip grid, Settings = popup. Tauri v2 / WebView2 shell.
Replaces both the experimental Deno Desktop host (which couldn't reliably show a
window) and the old WinUI 3 settings sidecar, both now removed. See ADR-0011 /
ADR-0012.

## Architecture

- **Rust host** (`src-tauri/`): opens a WebView2 window with Tauri and runs a
  loopback HTTP server (`server.rs`) that serves the built frontend, a JSON API
  over the **read-only** clip catalogs (`clip_catalog.rs` via `rusqlite`,
  bundled SQLite), and range-streamed media/thumbnails (`media.rs`). The window
  navigates to `http://127.0.0.1:<port>/`, so the frontend's `fetch()`/`<video>`
  route contract is unchanged — no `invoke()` rewrite.
- **Engine IPC** (`engine_rpc.rs`): clip mutations (favorite/hashtag/rename/
  delete) and `reload_settings` go to `Monolith.exe` over JSON-RPC on
  `127.0.0.1:45991` — the engine is the single writer of `clips.db`/`recs.db`.
- **Frontend** (`ui/`): Preact + esbuild (Deno loader), bundled by `build.ts`
  into `dist/`, embedded into the exe at compile time via `include_dir!`. Home
  grid, rounded clip cards with hover preview, context menu, favorite/hashtag/
  rename/delete, filters, fullscreen, settings popup.

## Develop

Two toolchains: `deno` (frontend bundle only) and the Rust toolchain (host).

```bat
deno run -A build.ts            :: bundle the frontend into dist/ (needed before cargo)
cargo run --manifest-path src-tauri\Cargo.toml   :: build + run the UI (engine optional)
```

The window opens even when `Monolith.exe` is not running; engine-dependent
routes degrade gracefully (empty clips, `config: null`, no crash when
`127.0.0.1:45991` is down).

## Ship

`cargo build --release` produces a single self-contained
`src-tauri/target/release/monolith_ui.exe` (SQLite is statically linked; the
frontend is embedded — nothing else to copy). The tauri-cli is not required.

```bat
deno run -A build.ts
cargo build --release --manifest-path src-tauri\Cargo.toml
```

CMake builds this and copies it to `<recorder-output>/ui/Monolith.UI.exe` when
both `deno` and `cargo` are on PATH (see `CMakeLists.txt`). The recorder tray
"Settings…" entry launches it (`app/recorder/src/settings_window.cpp`).

## Folders

```
src-tauri/          Rust host: main.rs, server.rs, media.rs, clip_catalog.rs,
                    settings_store.rs, engine_rpc.rs, game_catalog.rs, paths.rs
build.ts            esbuild bundle of ui/ -> dist/ (run before cargo build)
ui/                 index.html, styles.css, src/ (Preact frontend)
deno.json           frontend bundler config (Deno is build-only, never shipped)
```

Deno is used *only* to bundle the Preact frontend into `dist/` at build time; it
is not part of the shipped app (the exe embeds `dist/` via `include_dir!`).
