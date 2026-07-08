// Extracts the embedded icon of a local executable and returns it as PNG bytes.
// Used by /api/exe-icon so the UI can render the *real* application icon (title
// bar capture status pattern) instead of remote artwork or a raw process name.
// Windows-only by definition (SHDefExtractIconW); results are cached per path
// because the shell extraction + PNG encode is a few milliseconds each.

use std::collections::HashMap;
use std::path::Path;
use std::sync::{Mutex, OnceLock};

use windows::core::PCWSTR;
use windows::Win32::Foundation::S_OK;
use windows::Win32::Graphics::Gdi::{
    DeleteObject, GetDC, GetDIBits, GetObjectW, ReleaseDC, BITMAP, BITMAPINFO, BITMAPINFOHEADER,
    BI_RGB, DIB_RGB_COLORS, HBITMAP,
};
use windows::Win32::UI::Shell::SHDefExtractIconW;
use windows::Win32::UI::WindowsAndMessaging::{DestroyIcon, GetIconInfo, HICON, ICONINFO};

const ICON_SIZE: u32 = 64;

static CACHE: OnceLock<Mutex<HashMap<String, Option<Vec<u8>>>>> = OnceLock::new();

/// PNG bytes of the file's icon, or None when the path is not a readable
/// executable or carries no extractable icon. Negative results are cached too.
pub fn icon_png(path: &str) -> Option<Vec<u8>> {
    let key = path.to_lowercase();
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some(hit) = cache.lock().ok()?.get(&key) {
        return hit.clone();
    }

    let result = extract(path);
    if let Ok(mut map) = cache.lock() {
        map.insert(key, result.clone());
    }
    result
}

fn extract(path: &str) -> Option<Vec<u8>> {
    let p = Path::new(path);
    let ext_ok = p
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| e.eq_ignore_ascii_case("exe") || e.eq_ignore_ascii_case("dll"))
        .unwrap_or(false);
    if !ext_ok || !p.is_file() {
        return None;
    }

    let wide: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    unsafe {
        let mut hicon = HICON::default();
        // LOWORD of nIconSize is the large-icon size; we only request the large one.
        let hr = SHDefExtractIconW(
            PCWSTR(wide.as_ptr()),
            0,
            0,
            Some(&mut hicon),
            None,
            ICON_SIZE,
        );
        if hr != S_OK || hicon.is_invalid() {
            return None;
        }
        let png = hicon_to_png(hicon);
        let _ = DestroyIcon(hicon);
        png
    }
}

// Reads the icon's color + mask bitmaps into RGBA and encodes a PNG. Modern
// icons ship a 32bpp color plane with a real alpha channel; legacy ones have
// an empty alpha plane, in which case alpha is rebuilt from the AND mask.
unsafe fn hicon_to_png(hicon: HICON) -> Option<Vec<u8>> {
    let mut info = ICONINFO::default();
    GetIconInfo(hicon, &mut info).ok()?;
    let color = info.hbmColor;
    let mask = info.hbmMask;

    let result = (|| {
        if color.is_invalid() {
            return None; // monochrome-only icon; not worth rendering as art
        }
        let mut bm = BITMAP::default();
        if GetObjectW(
            color,
            std::mem::size_of::<BITMAP>() as i32,
            Some(&mut bm as *mut _ as *mut _),
        ) == 0
        {
            return None;
        }
        let (w, h) = (bm.bmWidth, bm.bmHeight);
        if w <= 0 || h <= 0 || w > 512 || h > 512 {
            return None;
        }

        let mut pixels = dib_bgra(color, w, h)?;
        // Legacy icons: alpha plane all zero -> derive it from the AND mask
        // (mask pixel non-black = transparent).
        if pixels.chunks_exact(4).all(|px| px[3] == 0) {
            let mask_px = dib_bgra(mask, w, h)?;
            for (px, m) in pixels.chunks_exact_mut(4).zip(mask_px.chunks_exact(4)) {
                px[3] = if m[0] == 0 { 255 } else { 0 };
            }
        }
        // BGRA -> RGBA in place.
        for px in pixels.chunks_exact_mut(4) {
            px.swap(0, 2);
        }

        let mut out = Vec::new();
        {
            let mut enc = png::Encoder::new(&mut out, w as u32, h as u32);
            enc.set_color(png::ColorType::Rgba);
            enc.set_depth(png::BitDepth::Eight);
            let mut writer = enc.write_header().ok()?;
            writer.write_image_data(&pixels).ok()?;
        }
        Some(out)
    })();

    let _ = DeleteObject(color);
    let _ = DeleteObject(mask);
    result
}

// Copies a bitmap into a top-down 32bpp BGRA buffer via GetDIBits (which
// converts from whatever depth the source bitmap has).
unsafe fn dib_bgra(bitmap: HBITMAP, w: i32, h: i32) -> Option<Vec<u8>> {
    let hdc = GetDC(None);
    if hdc.is_invalid() {
        return None;
    }
    let mut bi = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: w,
            biHeight: -h, // negative = top-down rows
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            ..Default::default()
        },
        ..Default::default()
    };
    let mut buf = vec![0u8; (w as usize) * (h as usize) * 4];
    let rows = GetDIBits(
        hdc,
        bitmap,
        0,
        h as u32,
        Some(buf.as_mut_ptr() as *mut _),
        &mut bi,
        DIB_RGB_COLORS,
    );
    ReleaseDC(None, hdc);
    if rows == 0 {
        return None;
    }
    Some(buf)
}
