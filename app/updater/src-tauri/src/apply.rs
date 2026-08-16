use std::fs;
use std::path::Path;
use windows::core::PCWSTR;
use windows::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING};

/// Extracts a component zip into dest, rejecting entries that escape dest
/// (zip-slip).
pub fn extract_zip(zip_path: &Path, dest: &Path) -> Result<(), String> {
    let file = fs::File::open(zip_path).map_err(|e| format!("open zip: {e}"))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("read zip: {e}"))?;
    fs::create_dir_all(dest).map_err(|e| format!("staging dir: {e}"))?;
    for i in 0..archive.len() {
        let mut entry = archive
            .by_index(i)
            .map_err(|e| format!("zip entry {i}: {e}"))?;
        let Some(rel) = entry.enclosed_name() else {
            return Err(format!("unsafe path in zip: {}", entry.name()));
        };
        let out = dest.join(rel);
        if entry.is_dir() {
            fs::create_dir_all(&out).map_err(|e| format!("mkdir {}: {e}", out.display()))?;
            continue;
        }
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("mkdir: {e}"))?;
        }
        let mut w =
            fs::File::create(&out).map_err(|e| format!("extract {}: {e}", out.display()))?;
        std::io::copy(&mut entry, &mut w)
            .map_err(|e| format!("extract {}: {e}", out.display()))?;
    }
    Ok(())
}

fn wide(path: &Path) -> Vec<u16> {
    path.as_os_str()
        .to_string_lossy()
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect()
}

fn to_old(path: &Path) -> Result<(), String> {
    // Renaming a running image or a loaded DLL is legal on Windows; deleting
    // is not. Park replaced files as *.old — swept on the next launch.
    let mut name = path.file_name().map(|n| n.to_os_string()).unwrap_or_default();
    name.push(".old");
    let old = path.with_file_name(name);
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

fn place_file(staged: &Path, target: &Path) -> Result<(), String> {
    if target.exists() {
        to_old(target)?;
    }
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("mkdir: {e}"))?;
    }
    if fs::rename(staged, target).is_err() {
        // Same-volume rename should always work (staging lives inside the
        // app dir); the copy+delete fallback covers exotic setups.
        fs::copy(staged, target)
            .and_then(|_| fs::remove_file(staged))
            .map_err(|e| format!("place {}: {e}", target.display()))?;
    }
    Ok(())
}

/// Moves every file of the extracted component tree into place under dest.
/// Files already present in dest but not part of the tree are left alone —
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
    to_old(&current)?;
    fs::copy(new_exe, &current)
        .map(|_| ())
        .map_err(|e| format!("self swap: {e}"))
}

/// Deletes *.old leftovers from previous applies (app root + ui\). A failure
/// (file still held by a process that has not fully exited yet) is silently
/// retried on the next launch. Also drops the legacy WinSparkle.dll from
/// installs migrated off the old updater.
pub fn sweep_old(app_dir: &Path) {
    let dirs = [app_dir.to_path_buf(), app_dir.join("ui")];
    for dir in dirs {
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let p = entry.path();
            if p.extension().map(|e| e == "old").unwrap_or(false) {
                let _ = fs::remove_file(&p);
            }
        }
    }
    let _ = fs::remove_file(app_dir.join("WinSparkle.dll"));
}
