use std::fs;
use std::path::Path;
use windows::core::PCWSTR;
use windows::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING};

fn wide(path: &Path) -> Vec<u16> {
    path.as_os_str()
        .to_string_lossy()
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect()
}

fn to_old(path: &Path) -> Result<(), String> {
    // Renaming a running image or a loaded DLL is legal on Windows; deleting
    // is not. Park replaced files as *.old - swept on the next launch.
    let old = old_path(path);
    unsafe {
        MoveFileExW(
            PCWSTR(wide(path).as_ptr()),
            PCWSTR(wide(&old).as_ptr()),
            MOVEFILE_REPLACE_EXISTING,
        )
        .map_err(|e| format!("park {}: {e}", path.display()))?;
    }
    Ok(())
}

fn old_path(path: &Path) -> std::path::PathBuf {
    let mut name = path
        .file_name()
        .map(|n| n.to_os_string())
        .unwrap_or_default();
    name.push(".old");
    path.with_file_name(name)
}

fn place_file(staged: &Path, target: &Path) -> Result<(), String> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("mkdir: {e}"))?;
    }
    let replaced = target.exists();
    if replaced {
        to_old(target)?;
    }
    if fs::rename(staged, target).is_err() {
        // Same-volume rename should always work (staging lives inside the
        // app dir); the copy+delete fallback covers exotic setups.
        if let Err(error) = fs::copy(staged, target) {
            if replaced {
                unsafe {
                    MoveFileExW(
                        PCWSTR(wide(&old_path(target)).as_ptr()),
                        PCWSTR(wide(target).as_ptr()),
                        MOVEFILE_REPLACE_EXISTING,
                    )
                    .map_err(|restore| {
                        format!(
                            "place {}: {error}; restore failed: {restore}",
                            target.display()
                        )
                    })?;
                }
            } else {
                let _ = fs::remove_file(target);
            }
            return Err(format!("place {}: {error}", target.display()));
        }
        let _ = fs::remove_file(staged);
    }
    Ok(())
}

/// Moves every file of the extracted component tree into place under dest.
/// Files already present in dest but not part of the tree are left alone -
/// the app root also hosts Updater.exe and user-adjacent files that are not
/// part of the engine zip.
pub fn place_tree(src: &Path, dest: &Path) -> Result<(), String> {
    place_tree_rec(src, src, dest)
}

fn place_tree_rec(root: &Path, src: &Path, dest: &Path) -> Result<(), String> {
    for entry in fs::read_dir(src).map_err(|e| format!("read {}: {e}", src.display()))? {
        let entry = entry.map_err(|e| format!("read {}: {e}", src.display()))?;
        let path = entry.path();
        if path.is_dir() {
            place_tree_rec(root, &path, dest)?;
        } else {
            let rel = path.strip_prefix(root).map_err(|e| e.to_string())?;
            place_file(&path, &dest.join(rel))?;
        }
    }
    Ok(())
}

/// Replaces Updater.exe itself: park the running image as .old, copy the new
/// one in. The running process keeps executing from memory, so the swap is
/// safe mid-flight; the parked image is swept on the next launch.
pub fn self_swap(new_exe: &Path) -> Result<(), String> {
    let current = std::env::current_exe().map_err(|e| format!("self path: {e}"))?;
    place_file(new_exe, &current)
}

/// Deletes *.old leftovers from previous applies (app root + ui\). A failure
/// (file still held by a process that has not fully exited yet) is silently
/// retried on the next launch. Also drops the legacy WinSparkle.dll from
/// installs migrated off the old updater.
pub fn sweep_old(app_dir: &Path) {
    let dirs = [app_dir.to_path_buf(), app_dir.join("ui")];
    for dir in dirs {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let p = entry.path();
            if p.extension().map(|e| e == "old").unwrap_or(false) {
                let _ = fs::remove_file(&p);
            }
        }
    }
    let _ = fs::remove_file(app_dir.join("WinSparkle.dll"));
}
