use std::path::PathBuf;

pub fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Directory where the installed app lives (Monolith.exe, Updater.exe, ui\).
/// Resolution order:
/// 1. `MONOLITH_APP_DIR` env override (tests / portable layouts);
/// 2. the exe's own dir when it contains Monolith.exe (installed layout, or
///    the CMake output dir where the build copies both exes);
/// 3. dev fallback: walking up from a cargo target dir finds the repo root
///    (recognized by app/desktop-ui + app/updater) and uses the CMake
///    release output dir.
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
        if dir.join("app").join("desktop-ui").is_dir()
            && dir.join("app").join("updater").is_dir()
        {
            return dir.join("build").join("app").join("recorder").join("Release");
        }
        cur = dir.parent();
    }
    exe
}

pub fn staging_dir() -> PathBuf {
    app_dir().join("update-staging")
}
