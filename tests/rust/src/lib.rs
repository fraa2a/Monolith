#![allow(dead_code)]

#[path = "../../../app/updater/src-tauri/src/install.rs"]
mod install;

#[path = "../../../app/updater/src-tauri/src/archive.rs"]
mod archive;
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/clip_catalog.rs"]
mod clip_catalog;
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/collections.rs"]
mod collections;
#[path = "../../../app/updater/src-tauri/src/download.rs"]
mod download;
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/game_catalog.rs"]
mod game_catalog;
#[path = "../../../app/updater/src-tauri/src/manifest.rs"]
mod manifest;
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/paths.rs"]
mod paths;
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/settings_store.rs"]
mod settings_store;

#[cfg(windows)]
#[path = "../../../app/updater/src-tauri/src/apply.rs"]
mod apply;
#[cfg(windows)]
#[path = "../../../app/updater/src-tauri/src/http.rs"]
mod http;
#[cfg(not(feature = "catalogs"))]
#[path = "../../../app/updater/src-tauri/src/versions.rs"]
mod versions;

#[cfg(not(windows))]
mod http {
    use std::{path::Path, sync::atomic::AtomicBool};
    pub fn get_to_string(_: &str) -> Result<String, String> {
        Err("WinHTTP requires Windows".into())
    }
    pub fn get_to_file(
        _: &str,
        _: &Path,
        _: u64,
        _: &AtomicBool,
        _: &dyn Fn(u64),
    ) -> Result<u64, String> {
        Err("WinHTTP requires Windows".into())
    }
}

#[cfg(not(feature = "catalogs"))]
#[path = "../../../app/updater/src-tauri/src/paths.rs"]
mod paths;

#[cfg(all(test, feature = "catalogs"))]
mod tests;

#[path = "../../../app/desktop-ui/src-tauri/src/engine_rpc.rs"]
mod ui_rpc;
#[path = "../../../app/updater/src-tauri/src/engine_rpc.rs"]
mod updater_rpc;

#[cfg(all(test, feature = "catalogs"))]
mod host_tests;
#[cfg(feature = "catalogs")]
mod engine_rpc {
    pub use crate::ui_rpc::*;
}
#[cfg(feature = "catalogs")]
#[path = "../../../app/desktop-ui/src-tauri/src/clip_mutations.rs"]
mod clip_mutations;
#[path = "../../../app/desktop-ui/src-tauri/src/media_assets.rs"]
mod media_assets;
#[cfg(all(test, feature = "catalogs"))]
static CATALOG_ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
#[path = "../../../app/desktop-ui/src-tauri/src/request_pool.rs"]
mod request_pool;
#[path = "../../../app/updater/src-tauri/src/process.rs"]
mod process;

#[cfg(not(feature = "catalogs"))]
#[path = "../../../app/updater/src-tauri/src/state.rs"]
mod updater_state;
