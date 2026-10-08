// Refresh asset permissions after output-folder changes; include .thumbs.

use tauri::{AppHandle, Manager};

pub fn refresh(app: &AppHandle) {
    let dirs = crate::settings_store::output_dirs();
    // recursive: true also covers each folder's .thumbs subdirectory.
    let scope = app.asset_protocol_scope();
    let _ = scope.allow_directory(&dirs.clips, true);
    let _ = scope.allow_directory(&dirs.recs, true);
}
