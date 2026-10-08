// Tauri hosts bundled frontend assets and the native command/event bridge.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod asset_scope;
mod clip_catalog;
mod collections;
mod commands;
mod engine_rpc;
#[cfg(target_os = "windows")]
mod exe_icon;
mod game_catalog;
mod paths;
mod settings_store;

use std::thread;
use std::time::Duration;
use tauri::{Emitter, WebviewUrl, WebviewWindowBuilder};

// Emit clips events when the engine generation counter changes.
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

// Refresh artwork in the background; the grid reads only cached rows.
fn spawn_artwork_refresh() {
    thread::spawn(|| {
        const REFRESH_AGE: Duration = Duration::from_secs(72 * 3600);
        const CHECK_INTERVAL: Duration = Duration::from_secs(6 * 3600);
        loop {
            game_catalog::refresh_stale(REFRESH_AGE);
            thread::sleep(CHECK_INTERVAL);
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
            commands::set_selected_game,
            commands::clip_set_duration,
            commands::thumb_capture,
            commands::clip_set_favorite,
            commands::clip_set_title,
            commands::clip_add_hashtag,
            commands::clip_remove_hashtag,
            commands::clip_rename,
            commands::clip_delete,
            commands::clip_regen_thumb,
            commands::clip_trim,
            commands::recording_add_bookmark,
            commands::clip_list_bookmarks,
            commands::clip_add_bookmark,
            commands::clip_update_bookmark,
            commands::clip_delete_bookmark,
            commands::list_collections,
            commands::create_collection,
            commands::rename_collection,
            commands::delete_collection,
            commands::add_clip_to_collection,
            commands::remove_clip_from_collection,
            commands::collection_clips,
            commands::reveal_in_explorer,
            commands::open_updater,
            commands::get_settings,
            commands::save_settings,
            commands::runtime_status,
            commands::pick_folder,
            #[cfg(target_os = "windows")]
            commands::exe_icon,
            commands::game_catalog_map,
            commands::game_icon,
            commands::game_artwork,
        ])
        .setup(|app| {
            asset_scope::refresh(&app.handle());
            spawn_clip_watch(app.handle().clone());
            spawn_artwork_refresh();

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
