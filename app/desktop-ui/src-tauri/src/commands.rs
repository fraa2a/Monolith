// Native Tauri commands. Run blocking disk, decode and network work through spawn_blocking.

#[cfg(target_os = "windows")]
use crate::exe_icon;
use crate::{clip_catalog, engine_rpc, game_catalog, settings_store};
use base64::{engine::general_purpose, Engine as _};
use clip_catalog::{Clip, ClipFilter, ClipSource};
use game_catalog::CatalogEntry;
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeMap;

fn parse_source(source: &str) -> Result<ClipSource, String> {
    ClipSource::parse(source).ok_or_else(|| "bad source".to_string())
}

// The closure runs on a blocking worker, outside the Tauri message loop.
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

// Selects which detected game the engine should record/clip when several are
// running. `exe` is the executable basename ("" or "auto" clears the selection).
#[tauri::command]
pub async fn set_selected_game(exe: String, pid: Option<u32>) -> Result<(), String> {
    let params = serde_json::json!({ "exe": exe, "pid": pid.unwrap_or(0) });
    let result = blocking(move || engine_rpc::mutate_clip("set_selected_game", params)).await;
    if result.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        Ok(())
    } else {
        let err = result
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("engine error")
            .to_string();
        eprintln!("[set_selected_game] failed: {err}");
        Err(err)
    }
}

async fn clip_mutation(
    method: &'static str,
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    mut params: Value,
) -> Result<(), String> {
    let src = parse_source(&source)?;
    params["source"] = serde_json::json!(src.as_str());
    params["id"] = serde_json::json!(id);
    params["catalog_uid"] = serde_json::json!(catalog_uid);
    params["clip_uid"] = serde_json::json!(clip_uid);
    blocking_result(move || crate::clip_mutations::send(method, params)).await
}
#[tauri::command]
pub async fn clip_set_duration(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    media_revision: i64,
    duration: f64,
) -> Result<(), String> {
    clip_mutation(
        "clip_set_duration",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"duration":duration,"media_revision":media_revision}),
    )
    .await
}
#[tauri::command]
pub async fn thumb_capture(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    media_revision: i64,
    data_url: String,
) -> Result<String, String> {
    let src = parse_source(&source)?;
    blocking_result(move || {
        crate::clip_mutations::capture(
            src.as_str(),
            id,
            &catalog_uid,
            &clip_uid,
            media_revision,
            &data_url,
        )
    })
    .await
}
#[tauri::command]
pub async fn clip_set_favorite(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    favorite: bool,
) -> Result<(), String> {
    clip_mutation(
        "clip_set_favorite",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"favorite":favorite}),
    )
    .await
}
#[tauri::command]
pub async fn clip_set_title(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    title: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_set_title",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"title":title}),
    )
    .await
}
#[tauri::command]
pub async fn clip_add_hashtag(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    tag: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_add_hashtag",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"tag":tag}),
    )
    .await
}
#[tauri::command]
pub async fn clip_remove_hashtag(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    tag: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_remove_hashtag",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"tag":tag}),
    )
    .await
}
#[tauri::command]
pub async fn clip_rename(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    new_name: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_rename",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"new_name":new_name}),
    )
    .await
}
#[tauri::command]
pub async fn clip_delete(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_delete",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({}),
    )
    .await
}

// Explorer requires /select, and the path in separate arguments.
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

// Resolve Updater.exe beside the recorder or in the development build tree.
#[tauri::command]
pub async fn open_updater() -> Result<(), String> {
    blocking_result(move || {
        let exe = std::env::current_exe().map_err(|err| err.to_string())?;
        let base = exe
            .parent()
            .ok_or_else(|| "no exe directory".to_string())?
            .to_path_buf();
        let candidates = [
            // Installed: <app>\ui\Monolith.UI.exe → <app>\Updater.exe.
            base.join("..").join("Updater.exe"),
            // Dev: cargo tree under the repo → CMake output copy.
            base.join("../../../../../../build/app/recorder/Release/Updater.exe"),
            // Dev: the updater's own cargo tree.
            base.join("../../../../app/updater/src-tauri/target/release/updater.exe"),
            base.join("../../../../app/updater/src-tauri/target/debug/updater.exe"),
        ];
        for candidate in candidates {
            let path = candidate.canonicalize().unwrap_or(candidate);
            if path.is_file() {
                let dir = path.parent().map(|p| p.to_path_buf()).unwrap_or_default();
                return std::process::Command::new(&path)
                    .current_dir(dir)
                    .spawn()
                    .map(|_| ())
                    .map_err(|err| err.to_string());
            }
        }
        Err("Updater.exe not found (built by CMake when Rust and Node are installed)".to_string())
    })
    .await
}

#[tauri::command]
pub async fn clip_regen_thumb(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_regen_thumb",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({}),
    )
    .await
}
#[tauri::command]
pub async fn clip_trim(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    start: f64,
    end: f64,
) -> Result<(), String> {
    clip_mutation(
        "clip_trim",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"start":start,"end":end}),
    )
    .await
}

// Adds a bookmark at the current position of the running manual recording.
// Handled by the engine's IPC thread (timestamp accuracy matters). Fails with
// an engine error string when nothing is recording (or it is paused).
#[tauri::command]
pub async fn recording_add_bookmark() -> Result<(), String> {
    let result =
        blocking(|| engine_rpc::mutate_clip("recording_add_bookmark", serde_json::json!({}))).await;
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

#[tauri::command]
pub async fn clip_list_bookmarks(
    source: String,
    id: i64,
) -> Result<Vec<clip_catalog::BookmarkRow>, String> {
    let src = parse_source(&source)?;
    blocking_result(move || clip_catalog::list_bookmarks(src, id)).await
}

#[tauri::command]
pub async fn clip_add_bookmark(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    time_seconds: f64,
    label: String,
    color: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_add_bookmark",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"time_seconds":time_seconds,"label":label,"color":color}),
    )
    .await
}
#[tauri::command]
pub async fn clip_update_bookmark(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    seq: i64,
    label: String,
    color: String,
) -> Result<(), String> {
    clip_mutation(
        "clip_update_bookmark",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"seq":seq,"label":label,"color":color}),
    )
    .await
}
#[tauri::command]
pub async fn clip_delete_bookmark(
    source: String,
    id: i64,
    catalog_uid: String,
    clip_uid: String,
    seq: i64,
) -> Result<(), String> {
    clip_mutation(
        "clip_delete_bookmark",
        source,
        id,
        catalog_uid,
        clip_uid,
        serde_json::json!({"seq":seq}),
    )
    .await
}
#[tauri::command]
pub async fn clip_snapshot(source: String, id: i64) -> Result<Clip, String> {
    let src = parse_source(&source)?;
    blocking_result(move || {
        clip_catalog::clips_by_ids(src, &[id])?
            .remove(&id)
            .ok_or_else(|| "clip unavailable".into())
    })
    .await
}
#[tauri::command]
pub async fn collection_memberships(source: String, id: i64) -> Result<Vec<i64>, String> {
    let src = parse_source(&source)?;
    blocking_result(move || crate::collections::collection_memberships(src, id)).await
}

#[tauri::command]
pub async fn list_collections() -> Result<Vec<crate::collections::CollectionSummary>, String> {
    blocking_result(crate::collections::list_collections).await
}

#[tauri::command]
pub async fn create_collection(name: String, color: String) -> Result<i64, String> {
    blocking_result(move || crate::collections::create_collection(&name, &color)).await
}

#[tauri::command]
pub async fn rename_collection(id: i64, name: String) -> Result<(), String> {
    blocking_result(move || crate::collections::rename_collection(id, &name)).await
}

#[tauri::command]
pub async fn delete_collection(id: i64) -> Result<(), String> {
    blocking_result(move || crate::collections::delete_collection(id)).await
}

#[tauri::command]
pub async fn add_clip_to_collection(
    collection_id: i64,
    source: String,
    clip_id: i64,
    catalog_uid: String,
    clip_uid: String,
) -> Result<(), String> {
    let src = parse_source(&source)?;
    blocking_result(move || {
        crate::collections::add_clip_with_identity(
            collection_id,
            src,
            clip_id,
            clip_catalog::ClipIdentity {
                catalog_uid,
                clip_uid,
            },
        )
    })
    .await
}

#[tauri::command]
pub async fn remove_clip_from_collection(
    collection_id: i64,
    source: String,
    clip_id: i64,
    catalog_uid: String,
    clip_uid: String,
) -> Result<(), String> {
    let src = parse_source(&source)?;
    let _ = clip_id;
    blocking_result(move || {
        crate::collections::remove_clip_with_identity(
            collection_id,
            src,
            clip_catalog::ClipIdentity {
                catalog_uid,
                clip_uid,
            },
        )
    })
    .await
}

#[tauri::command]
pub async fn collection_clips(collection_id: i64) -> Result<Vec<Clip>, String> {
    blocking_result(move || crate::collections::collection_clips(collection_id)).await
}

#[tauri::command]
pub async fn get_settings() -> Value {
    blocking(|| settings_store::read_config().unwrap_or(Value::Null)).await
}

// Ignore disabled NONE bindings when checking normalized hotkey conflicts.
fn find_hotkey_collision(config: &Value) -> Option<String> {
    let hotkeys = config.get("hotkeys")?.as_object()?;
    let entries: [(&str, &str); 5] = [
        ("Save Replay", "save_replay"),
        ("Start Recording", "recording_start"),
        ("Stop Recording", "recording_stop"),
        ("Pause/Resume", "pause_resume"),
        ("Add Bookmark", "add_bookmark"),
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
pub async fn save_settings(config: Value) -> Result<(), String> {
    if !config.is_object() {
        return Err("bad config".to_string());
    }
    if let Some(err) = find_hotkey_collision(&config) {
        return Err(err);
    }
    blocking_result(move || settings_store::write_config(&config).map_err(|err| err.to_string()))
        .await?;
    if let Err(err) = blocking(engine_rpc::reload_settings).await {
        // Settings are saved; the engine just couldn't reload them right now
        // (it may not be running - it reads the new values at next start).
        eprintln!("engine settings reload failed: {err}");
    }
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
        dialog
            .pick_folder()
            .map(|p| p.to_string_lossy().to_string())
    })
    .await
}

#[cfg(target_os = "windows")]
#[tauri::command]
pub async fn exe_icon(path: String, process: String) -> Option<String> {
    blocking(move || {
        exe_icon::icon_png(&path, &process).map(|bytes| {
            format!(
                "data:image/png;base64,{}",
                general_purpose::STANDARD.encode(bytes)
            )
        })
    })
    .await
}

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
