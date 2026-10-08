use std::path::PathBuf;

pub fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Resolve the app directory from MONOLITH_APP_DIR, the installed layout,
/// then the development CMake output.
pub fn app_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("MONOLITH_APP_DIR") {
        if !dir.is_empty() {
            return PathBuf::from(dir);
        }
    }
    let exe = exe_dir();
    if exe.join("Monolith.exe").is_file() {
        return exe;
    }
    let mut cur: Option<&std::path::Path> = Some(exe.as_path());
    while let Some(dir) = cur {
        if dir.join("app").join("desktop-ui").is_dir() && dir.join("app").join("updater").is_dir() {
            return dir
                .join("build")
                .join("app")
                .join("recorder")
                .join("Release");
        }
        cur = dir.parent();
    }
    exe
}

pub fn staging_dir() -> PathBuf {
    app_dir().join("update-staging")
}
