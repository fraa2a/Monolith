// Monolith Desktop UI host (Tauri v2 / WebView2). The window loads the bundled
// Preact frontend directly (native Tauri IPC: invoke/commands/events/asset
// protocol) instead of navigating to a local loopback HTTP server. The engine
// (Monolith.exe) is a separate process, reached only over JSON-RPC on
// 127.0.0.1:45991 for clip mutations / settings reload / recorder control
// (engine_rpc.rs) — that transport is unchanged and also used by the Stream
// Deck plugin. See docs/DECISIONS.md (ADR superseding ADR-0011/0012).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod asset_scope;
mod clip_catalog;
mod commands;
mod engine_rpc;
mod exe_icon;
mod game_catalog;
mod paths;
mod settings_store;

use std::thread;
use std::time::Duration;
use tauri::{Emitter, WebviewUrl, WebviewWindowBuilder};

// Polls the engine's clip-generation counter and emits a `clips` event to the
// frontend whenever it changes (a clip was saved/stopped). Mirrors the old SSE
// stream but over a native Tauri event; cheap for a single local user.
fn spawn_clip_watch(app: tauri::AppHandle) {
    thread::spawn(move || {
        let mut last = engine_rpc::clip_generation();
        loop {
            thread::sleep(Duration::from_millis(1000));
            if let Some(gen) = engine_rpc::clip_generation() {
                if last != Some(gen) {
                    last = Some(gen);
                    let _ = app.emit("clips", ());
                }
            }
        }
    });
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            commands::list_clips,
            commands::distinct_games,
            commands::distinct_hashtags,
            commands::engine_status,
            commands::recorder_command,
            commands::clip_set_duration,
            commands::thumb_capture,
            commands::clip_set_favorite,
            commands::clip_set_title,
            commands::clip_add_hashtag,
            commands::clip_remove_hashtag,
            commands::clip_rename,
            commands::clip_delete,
            commands::clip_regen_thumb,
            commands::get_settings,
            commands::save_settings,
            commands::runtime_status,
            commands::pick_folder,
            commands::exe_icon,
            commands::game_catalog_map,
            commands::game_icon,
            commands::game_artwork,
        ])
        .setup(|app| {
            asset_scope::refresh(&app.handle());
            spawn_clip_watch(app.handle().clone());

            // decorations(false): the frontend draws its own title bar; window
            // controls + dragging use the native @tauri-apps/api/window API.
            WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                .title("Monolith")
                .inner_size(1240.0, 820.0)
                .min_inner_size(940.0, 620.0)
                .decorations(false)
                .shadow(true)
                .resizable(true)
                .center()
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run Monolith UI");
}
