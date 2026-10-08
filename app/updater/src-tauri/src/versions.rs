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
        updater: file_version(&dir.join("Updater.exe"))
            .or_else(|| fallback.as_ref().map(|f| f.updater.clone()))
            .unwrap_or_else(|| env!("CARGO_PKG_VERSION").to_string()),
    }
}

/// Stage metadata for inclusion in the same transaction as the binaries.
pub fn stage_components_json(v: &Installed, path: &Path) -> Result<(), String> {
    use std::io::Write;
    let json = serde_json::to_vec_pretty(v).map_err(|e| e.to_string())?;
    let mut file = std::fs::File::create(path).map_err(|e| e.to_string())?;
    file.write_all(&json)
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string())
}

fn read_components_json() -> Option<Installed> {
    let text = std::fs::read_to_string(paths::app_dir().join("components.json")).ok()?;
    serde_json::from_str(&text).ok()
}

/// Read the translation-independent fixed VERSIONINFO block.
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
            || len < std::mem::size_of::<VS_FIXEDFILEINFO>() as u32
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
