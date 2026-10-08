use std::fs;
use std::path::Path;

/// Retry parked-file cleanup on later launches if files remain locked.
pub fn sweep_old(app_dir: &Path) {
    let dirs = [app_dir.to_path_buf(), app_dir.join("ui")];
    for dir in dirs {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let p = entry.path();
            if p.extension().is_some_and(|e| e == "old") {
                let _ = fs::remove_file(&p);
            }
        }
    }
    let _ = fs::remove_file(app_dir.join("WinSparkle.dll"));
}
