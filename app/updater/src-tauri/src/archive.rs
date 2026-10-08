use std::fs;
use std::path::Path;

/// Extracts a component zip into dest, rejecting entries that escape dest
/// (zip-slip).
pub fn extract_zip(zip_path: &Path, dest: &Path) -> Result<(), String> {
    let file = fs::File::open(zip_path).map_err(|e| format!("open zip: {e}"))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("read zip: {e}"))?;
    if archive.len() > 4096 {
        return Err("too many archive entries".into());
    }
    let mut total = 0u64;
    fs::create_dir_all(dest).map_err(|e| format!("staging dir: {e}"))?;
    for i in 0..archive.len() {
        let mut entry = archive
            .by_index(i)
            .map_err(|e| format!("zip entry {i}: {e}"))?;
        let Some(rel) = entry.enclosed_name() else {
            return Err(format!("unsafe path in zip: {}", entry.name()));
        };
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("archive links are not supported".into());
        }
        for part in rel.components() {
            let name = part.as_os_str().to_string_lossy();
            let stem = name.split('.').next().unwrap_or("").to_ascii_uppercase();
            if name.contains([':', '\\'])
                || name.ends_with(['.', ' '])
                || [
                    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                    "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
                    "LPT8", "LPT9",
                ]
                .contains(&stem.as_str())
            {
                return Err(format!("unsupported archive path: {}", entry.name()));
            }
        }
        total = total
            .checked_add(entry.size())
            .ok_or("archive size overflow")?;
        if total > 2 * 1024 * 1024 * 1024 {
            return Err("archive exceeds 2 GiB".into());
        }
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
        std::io::copy(&mut entry, &mut w).map_err(|e| format!("extract {}: {e}", out.display()))?;
    }
    Ok(())
}
