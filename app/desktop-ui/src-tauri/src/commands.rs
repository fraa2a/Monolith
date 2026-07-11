// Tauri IPC commands invoked from the frontend via `invoke()`. Replaces the old
// loopback HTTP `/api/*` routes (server.rs, removed) now that the window loads
// bundled assets directly (see main.rs) and has the Tauri JS API available.
//
// Clip/settings mutations write straight to the on-disk SQLite catalogs
// (clip_catalog.rs / settings_store.rs) so they work without the recorder
// engine running. Only `clip_regen_thumb` and the recorder-control commands
// cross to the engine over the JSON-RPC channel in engine_rpc.rs, which is
// unchanged and still used by the Stream Deck plugin (127.0.0.1:45991). See
// docs/DECISIONS.md.
//
// IMPORTANT: every command here is `async fn`. Tauri v2 runs plain
// (non-async) commands inline on the main thread, which is also the WebView2
// message-pump thread — a synchronous SQLite query or engine_rpc TCP call
// there freezes the whole window. `async fn` commands are dispatched via
// `async_runtime::spawn`, and the blocking bodies below run on
// `spawn_blocking` so the UI thread never waits on disk/network I/O.

use crate::{clip_catalog, engine_rpc, exe_icon, game_catalog, settings_store};
use base64::{engine::general_purpose, Engine as _};
use clip_catalog::{Clip, ClipFilter, ClipSource};
use game_catalog::CatalogEntry;
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeMap;

fn parse_source(source: &str) -> Result<ClipSource, String> {
    ClipSource::parse(source).ok_or_else(|| "bad source".to_string())
}

// Runs a blocking closure off the main thread and returns its value.
// Panics inside `f` are not expected in normal operation (they'd indicate a
// bug in the catalog/engine code, same as today), so they're surfaced by
// unwrapping the join result rather than silently swallowed.
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> T {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .expect("blocking task panicked")
}

// Same as `blocking`, but for closures that already return Result<T, String>;
// a panic (join error) is folded into the same error channel instead of
// unwrapping, since these are surfaced straight to the frontend as failures.
async fn blocking_result<T: Send + 'static>(
    f: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|err| err.to_string())?
}

// ── Clip catalog (read) ───────────────────────────────────────────────────

#[tauri::command]
pub async fn list_clips(filter: ClipFilter) -> Vec<Clip> {
    blocking(move || clip_catalog::list_clips(&filter)).await
}

#[tauri::command]
pub async fn distinct_games() -> Vec<String> {
    blocking(clip_catalog::distinct_games).await
}

#[tauri::command]
pub async fn distinct_hashtags() -> Vec<String> {
    blocking(clip_catalog::distinct_hashtags).await
}

// ── Engine status / recorder control (JSON-RPC to 127.0.0.1:45991) ───────

#[tauri::command]
pub async fn engine_status() -> Value {
    blocking(engine_rpc::get_status).await
}

#[tauri::command]
pub async fn recorder_command(method: String) -> Result<(), String> {
    let allowed = ["recording_start", "recording_stop", "save_replay"];
    if !allowed.contains(&method.as_str()) {
        return Err("bad method".to_string());
    }
    let result = blocking(move || engine_rpc::command(&method)).await;
    if result.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        Ok(())
    } else {
        Err(result
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("engine error")
            .to_string())
    }
}

// ── Clip mutations (direct SQLite writes; work without the engine) ───────

#[tauri::command]
pub async fn clip_set_duration(source: String, id: i64, duration: f64) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::set_duration(src, id, duration)).await
}

#[tauri::command]
pub async fn thumb_capture(source: String, id: i64, data_url: String) -> Result<String, String> {
    let src = parse_source(&source)?;
    blocking_result(move || {
        let encoded = data_url.split_once(',').map(|(_, data)| data).unwrap_or(&data_url);
        let decoded = general_purpose::STANDARD
            .decode(encoded.as_bytes())
            .map_err(|_| "bad thumbnail data".to_string())?;
        clip_catalog::save_thumbnail_capture(src, id, &decoded)
    })
    .await
}

#[tauri::command]
pub async fn clip_set_favorite(source: String, id: i64, favorite: bool) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::set_favorite(src, id, favorite)).await
}

#[tauri::command]
pub async fn clip_set_title(source: String, id: i64, title: String) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::set_title(src, id, &title)).await
}

#[tauri::command]
pub async fn clip_add_hashtag(source: String, id: i64, tag: String) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::add_hashtag(src, id, &tag)).await
}

#[tauri::command]
pub async fn clip_remove_hashtag(source: String, id: i64, tag: String) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::remove_hashtag(src, id, &tag)).await
}

#[tauri::command]
pub async fn clip_rename(source: String, id: i64, new_name: String) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::rename_clip(src, id, &new_name)).await
}

#[tauri::command]
pub async fn clip_delete(source: String, id: i64) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::remove_clip(src, id)).await
}

// Opens Explorer with the clip's video file pre-selected. `/select,` is an
// Explorer-native flag (no shell interpretation of the path), so this is safe
// against injection as long as `path` is a real filesystem path from the
// catalog rather than arbitrary user text.
#[tauri::command]
pub async fn reveal_in_explorer(path: String) -> Result<(), String> {
    blocking_result(move || {
        std::process::Command::new("explorer")
            .arg("/select,")
            .arg(&path)
            .spawn()
            .map(|_| ())
            .map_err(|err| err.to_string())
    })
    .await
}

// Only mutation that still needs the engine: thumbnail regeneration decodes a
// video frame via FFmpeg, which only the recorder process links against.
#[tauri::command]
pub async fn clip_regen_thumb(source: String, id: i64) -> Result<(), String> {
    let src = parse_source(&source)?;
    let result = blocking(move || {
        let params = serde_json::json!({ "source": src.as_str(), "id": id });
        engine_rpc::mutate_clip("clip_regen_thumb", params)
    })
    .await;
    if result.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        Ok(())
    } else {
        Err(result
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("engine error")
            .to_string())
    }
}

// ── Settings ───────────────────────────────────────────────────────────

#[tauri::command]
pub async fn get_settings() -> Value {
    blocking(|| settings_store::read_config().unwrap_or(Value::Null)).await
}

// Rejects a config document that assigns the same normalized hotkey chord to
// more than one action. Defense-in-depth mirror of the check the engine's
// `settings::save()` also runs before persisting — this one catches it before
// the write even happens. "NONE" (hotkey disabled) never collides with itself.
fn find_hotkey_collision(config: &Value) -> Option<String> {
    let hotkeys = config.get("hotkeys")?.as_object()?;
    let entries: [(&str, &str); 4] = [
        ("Save Replay", "save_replay"),
        ("Start Recording", "recording_start"),
        ("Stop Recording", "recording_stop"),
        ("Pause/Resume", "pause_resume"),
    ];
    let values: Vec<(&str, String)> = entries
        .iter()
        .filter_map(|(label, key)| {
            hotkeys
                .get(*key)
                .and_then(|v| v.as_str())
                .map(|s| (*label, s.to_lowercase()))
        })
        .collect();
    for i in 0..values.len() {
        if values[i].1.is_empty() || values[i].1 == "none" {
            continue;
        }
        for j in (i + 1)..values.len() {
            if values[i].1 == values[j].1 {
                return Some(format!(
                    "hotkey conflict: \"{}\" is assigned to both \"{}\" and \"{}\"",
                    values[i].1, values[i].0, values[j].0
                ));
            }
        }
    }
    None
}

#[tauri::command]
pub async fn save_settings(app: tauri::AppHandle, config: Value) -> Result<(), String> {
    if !config.is_object() {
        return Err("bad config".to_string());
    }
    if let Some(err) = find_hotkey_collision(&config) {
        return Err(err);
    }
    blocking_result(move || settings_store::write_config(&config).map_err(|err| err.to_string())).await?;
    blocking(engine_rpc::reload_settings).await;
    // Output folders may have changed: re-scope the asset protocol so
    // convertFileSrc() keeps working for the (possibly new) clip/recording
    // dirs. Cheap in-memory scope update, safe to run on the main thread.
    crate::asset_scope::refresh(&app);
    Ok(())
}

#[tauri::command]
pub async fn runtime_status() -> Value {
    blocking(settings_store::read_runtime_status).await
}

#[tauri::command]
pub async fn pick_folder(current: Option<String>) -> Option<String> {
    blocking(move || {
        let mut dialog = rfd::FileDialog::new();
        if let Some(start) = current.as_deref() {
            if !start.is_empty() && std::path::Path::new(start).is_dir() {
                dialog = dialog.set_directory(start);
            }
        }
        dialog.pick_folder().map(|p| p.to_string_lossy().to_string())
    })
    .await
}

// ── Native app icon (PNG extracted from an exe, base64 data URL) ─────────

#[tauri::command]
pub async fn exe_icon(path: String, process: String) -> Option<String> {
    blocking(move || {
        exe_icon::icon_png(&path, &process)
            .map(|bytes| format!("data:image/png;base64,{}", general_purpose::STANDARD.encode(bytes)))
    })
    .await
}

// ── Game catalog / artwork ────────────────────────────────────────────────

#[tauri::command]
pub async fn game_catalog_map() -> BTreeMap<String, CatalogEntry> {
    blocking(game_catalog::catalog_map).await
}

#[tauri::command]
pub async fn game_icon(process: String) -> Option<String> {
    blocking(move || game_catalog::resolve_icon(&process)).await
}

#[derive(Serialize)]
pub struct GameArtwork {
    icon: Option<String>,
    cover: Option<String>,
    display_name: Option<String>,
    discord_app_id: Option<String>,
}

#[tauri::command]
pub async fn game_artwork(app_id: Option<String>, process: Option<String>) -> GameArtwork {
    blocking(move || {
        let entry = game_catalog::resolve_artwork(app_id.as_deref(), process.as_deref());
        GameArtwork {
            icon: entry.icon_url,
            cover: entry.cover_url,
            display_name: if entry.display_name.is_empty() {
                None
            } else {
                Some(entry.display_name)
            },
            discord_app_id: entry.discord_app_id,
        }
    })
    .await
}
