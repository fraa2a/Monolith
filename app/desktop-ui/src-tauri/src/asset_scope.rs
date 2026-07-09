// Scopes the Tauri asset protocol (asset://, exposed to the frontend via
// convertFileSrc()) to the configured clip/recording folders, so <video>/<img>
// tags can stream straight from disk instead of round-tripping through a local
// HTTP server. Re-run on startup and whenever settings are saved, since the
// output folders are user-configurable. See docs/DECISIONS.md.

use tauri::{AppHandle, Manager};

pub fn refresh(app: &AppHandle) {
    let dirs = crate::settings_store::output_dirs();
    // recursive: true also covers each folder's .thumbs subdirectory.
    let scope = app.asset_protocol_scope();
    let _ = scope.allow_directory(&dirs.clips, true);
    let _ = scope.allow_directory(&dirs.recs, true);
}
