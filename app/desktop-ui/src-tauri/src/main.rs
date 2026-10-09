// Tauri hosts bundled frontend assets and the native command/event bridge.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod clip_catalog;
mod clip_mutations;
mod collections;
mod commands;
mod engine_rpc;
#[cfg(target_os = "windows")]
mod exe_icon;
mod game_catalog;
mod media_assets;
mod paths;
mod request_pool;
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
    let media_pool = request_pool::Pool::new(
        4,
        128,
        |(request, responder): (tauri::http::Request<Vec<u8>>, tauri::UriSchemeResponder)| {
            let dirs = settings_store::output_dirs();
            let range = request
                .headers()
                .get("Range")
                .and_then(|value| value.to_str().ok());
            let mut result = if request.method() == tauri::http::Method::HEAD {
                media_assets::serve_head(request.uri().path(), range, &[dirs.clips, dirs.recs])
            } else {
                media_assets::serve(request.uri().path(), range, &[dirs.clips, dirs.recs])
            };
            if request.method() != tauri::http::Method::GET
                && request.method() != tauri::http::Method::HEAD
            {
                result.status = 405;
                result.body.clear();
            }
            let mut response = tauri::http::Response::builder().status(result.status);
            for (name, value) in result.headers {
                response = response.header(name, value);
            }
            match response.body(result.body) {
                Ok(response) => responder.respond(response),
                Err(err) => {
                    eprintln!("media response failed: {err}");
                    responder.respond(
                        tauri::http::Response::builder()
                            .status(500)
                            .body(Vec::new())
                            .unwrap(),
                    );
                }
            }
        },
    )
    .expect("failed to create media request workers");
    tauri::Builder::default()
        .register_asynchronous_uri_scheme_protocol("media", move |_context, request, responder| {
            if let Err((_request, responder)) = media_pool.submit((request, responder)) {
                responder.respond(
                    tauri::http::Response::builder()
                        .status(503)
                        .header("Retry-After", "1")
                        .header("Access-Control-Allow-Origin", "*")
                        .body(Vec::new())
                        .unwrap(),
                );
            }
        })
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
            commands::collection_memberships,
            commands::clip_snapshot,
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
