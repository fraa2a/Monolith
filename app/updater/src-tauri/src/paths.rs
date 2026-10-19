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
    resolve_app_dir(&exe_dir())
}

fn resolve_app_dir(exe: &std::path::Path) -> PathBuf {
    for dir in exe.ancestors() {
        if [".update-recovery", ".update-transaction"]
            .iter()
            .any(|name| dir.file_name().is_some_and(|n| n == *name))
        {
            if let Some(root) = dir.parent() {
                if root.join(".update-transaction/journal.json").is_file() {
                    return root.to_path_buf();
                }
            }
        }
    }
    if exe.join("Monolith.exe").is_file() {
        return exe.to_path_buf();
    }
    let mut cur: Option<&std::path::Path> = Some(exe);
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
    exe.to_path_buf()
}

pub fn staging_dir() -> PathBuf {
    app_dir().join("update-staging")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn recovery_and_backup_launches_find_installation_root() {
        let root = std::env::temp_dir().join(format!("monolith-root-{}", std::process::id()));
        std::fs::create_dir_all(root.join(".update-transaction/backup")).unwrap();
        std::fs::write(root.join(".update-transaction/journal.json"), b"{}").unwrap();
        assert_eq!(resolve_app_dir(&root.join(".update-recovery")), root);
        assert_eq!(
            resolve_app_dir(&root.join(".update-transaction/backup")),
            root
        );
        std::fs::remove_dir_all(root).unwrap();
    }
}
