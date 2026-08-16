use crate::paths;
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Installed {
    pub engine: String,
    pub ui: String,
    pub updater: String,
}

pub fn installed() -> Installed {
    let fallback = read_components_json();
    let dir = paths::app_dir();
    Installed {
        engine: file_version(&dir.join("Monolith.exe"))
            .or_else(|| fallback.as_ref().map(|f| f.engine.clone()))
            .unwrap_or_else(|| "0.0.0".into()),
        ui: file_version(&dir.join("ui").join("Monolith.UI.exe"))
            .or_else(|| fallback.as_ref().map(|f| f.ui.clone()))
            .unwrap_or_else(|| "0.0.0".into()),
        // Own build: exact, no resource-file read needed.
        updater: env!("CARGO_PKG_VERSION").to_string(),
    }
}

/// Persists the last-applied per-component versions. The exe FileVersion is
/// the primary source; this file catches exes whose resource block is
/// missing or unreadable (and lets the updater's own version survive
/// self-swaps even if the new exe ever ships without a resource).
pub fn write_components_json(v: &Installed) {
    let path = paths::app_dir().join("components.json");
    if let Ok(json) = serde_json::to_string_pretty(v) {
        let _ = std::fs::write(path, json);
    }
}

fn read_components_json() -> Option<Installed> {
    let text = std::fs::read_to_string(paths::app_dir().join("components.json")).ok()?;
    serde_json::from_str(&text).ok()
}

/// Reads the Win32 VERSIONINFO FileVersion ("major.minor.patch[.build]") via
/// the fixed, translation-independent block — no StringFileInfo lookup
/// needed. Returns None when the file is missing or carries no version
/// resource.
fn file_version(path: &Path) -> Option<String> {
    if !path.is_file() {
        return None;
    }
    use windows::core::PCWSTR;
    use windows::Win32::Storage::FileSystem::{
        GetFileVersionInfoSizeW, GetFileVersionInfoW, VerQueryValueW, VS_FIXEDFILEINFO,
    };
    let wide: Vec<u16> = path
        .as_os_str()
        .to_string_lossy()
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let mut handle = 0u32;
        let size = GetFileVersionInfoSizeW(PCWSTR(wide.as_ptr()), Some(&mut handle));
        if size == 0 {
            return None;
        }
        let mut data = vec![0u8; size as usize];
        if GetFileVersionInfoW(
            PCWSTR(wide.as_ptr()),
            handle,
            size,
            data.as_mut_ptr() as *mut core::ffi::c_void,
        )
        .is_err()
        {
            return None;
        }
        let root: Vec<u16> = "\\".encode_utf16().chain(std::iter::once(0)).collect();
        let mut block: *mut core::ffi::c_void = std::ptr::null_mut();
        let mut len = 0u32;
        if !VerQueryValueW(
            data.as_ptr() as *const core::ffi::c_void,
            PCWSTR(root.as_ptr()),
            &mut block,
            &mut len,
        )
        .as_bool()
            || block.is_null()
        {
            return None;
        }
        let info = &*(block as *const VS_FIXEDFILEINFO);
        if info.dwSignature != 0xFEEF04BD {
            return None;
        }
        let major = (info.dwFileVersionMS >> 16) & 0xffff;
        let minor = info.dwFileVersionMS & 0xffff;
        let patch = (info.dwFileVersionLS >> 16) & 0xffff;
        let build = info.dwFileVersionLS & 0xffff;
        Some(if build == 0 {
            format!("{major}.{minor}.{patch}")
        } else {
            format!("{major}.{minor}.{patch}.{build}")
        })
    }
}
