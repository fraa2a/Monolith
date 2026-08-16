// Minimal WinHTTP client: manifest fetch + streaming download with progress.
//
// WinHTTP (not a Rust TLS stack) on purpose: the updater inherits the system
// proxy configuration — the same behavior the WinSparkle era had — and stays
// free of C dependencies, which also allows `cargo check --target
// x86_64-pc-windows-msvc` from any host. Redirects (GitHub's
// releases/latest/download → objects.githubusercontent.com) are followed by
// WinHTTP's default policy.

use std::io::Write;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use windows::core::PCWSTR;
use windows::Win32::Networking::WinHttp::{
    WinHttpCloseHandle, WinHttpConnect, WinHttpCrackUrl, WinHttpOpen, WinHttpOpenRequest,
    WinHttpQueryHeaders, WinHttpReadData, WinHttpReceiveResponse, WinHttpSendRequest,
    WinHttpSetTimeouts, URL_COMPONENTS, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
    WINHTTP_FLAG_SECURE, WINHTTP_INTERNET_SCHEME_HTTPS, WINHTTP_QUERY_FLAG_NUMBER,
    WINHTTP_QUERY_STATUS_CODE,
};

const USER_AGENT: &str = "Monolith-Updater";

struct HttpHandle(*mut core::ffi::c_void);
impl Drop for HttpHandle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe {
                let _ = WinHttpCloseHandle(self.0);
            }
        }
    }
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

struct Parts {
    host: Vec<u16>, // nul-terminated
    path: Vec<u16>, // nul-terminated, path + query
    port: u16,
    secure: bool,
}

fn crack_url(url: &str) -> Result<Parts, String> {
    let wide_url: Vec<u16> = url.encode_utf16().collect();
    let mut comps = URL_COMPONENTS::default();
    comps.dwStructSize = std::mem::size_of::<URL_COMPONENTS>() as u32;
    // 0xFFFFFFFF = return pointers/lengths of substrings inside the input
    // URL — no side buffers needed.
    comps.dwHostNameLength = 0xFFFF_FFFF;
    comps.dwUrlPathLength = 0xFFFF_FFFF;
    comps.dwExtraInfoLength = 0xFFFF_FFFF;
    unsafe {
        WinHttpCrackUrl(&wide_url, 0, &mut comps)
            .map_err(|_| format!("invalid URL: {url}"))?;
    }
    let host = unsafe {
        std::slice::from_raw_parts(comps.lpszHostName.0, comps.dwHostNameLength as usize)
    }
    .to_vec();
    let mut path = unsafe {
        std::slice::from_raw_parts(comps.lpszUrlPath.0, comps.dwUrlPathLength as usize)
    }
    .to_vec();
    if !comps.lpszExtraInfo.0.is_null() && comps.dwExtraInfoLength > 0 {
        path.extend_from_slice(unsafe {
            std::slice::from_raw_parts(
                comps.lpszExtraInfo.0,
                comps.dwExtraInfoLength as usize,
            )
        });
    }
    Ok(Parts {
        host,
        path,
        port: comps.nPort,
        secure: comps.nScheme == WINHTTP_INTERNET_SCHEME_HTTPS,
    })
}

// Drives a GET to completion and hands the response body to `sink` chunk by
// chunk.
fn get<F>(url: &str, mut sink: F) -> Result<(), String>
where
    F: FnMut(&[u8]) -> Result<(), String>,
{
    let parts = crack_url(url)?;
    let agent = wide(USER_AGENT);
    unsafe {
        let session_raw = WinHttpOpen(
            PCWSTR(agent.as_ptr()),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            PCWSTR::null(),
            PCWSTR::null(),
            0,
        );
        if session_raw.is_null() {
            return Err("WinHttpOpen failed".to_string());
        }
        let session = HttpHandle(session_raw);
        WinHttpSetTimeouts(session.0, 5_000, 15_000, 30_000, 60_000)
            .map_err(|e| format!("WinHttpSetTimeouts failed: {e}"))?;

        let port = if parts.port != 0 { parts.port } else { 443 };
        let connect_raw =
            WinHttpConnect(session.0, PCWSTR(parts.host.as_ptr()), port, 0);
        if connect_raw.is_null() {
            return Err("WinHttpConnect failed".to_string());
        }
        let connect = HttpHandle(connect_raw);

        let verb = wide("GET");
        let flags = if parts.secure {
            WINHTTP_FLAG_SECURE
        } else {
            windows::Win32::Networking::WinHttp::WINHTTP_OPEN_REQUEST_FLAGS(0)
        };
        let request_raw = WinHttpOpenRequest(
            connect.0,
            PCWSTR(verb.as_ptr()),
            PCWSTR(parts.path.as_ptr()),
            None,
            None,
            std::ptr::null(),
            flags,
        );
        if request_raw.is_null() {
            return Err("WinHttpOpenRequest failed".to_string());
        }
        let request = HttpHandle(request_raw);

        WinHttpSendRequest(request.0, None, None, 0, 0, 0)
            .map_err(|e| format!("request failed: {e}"))?;
        WinHttpReceiveResponse(request.0, std::ptr::null_mut())
            .map_err(|e| format!("no response: {e}"))?;

        let mut status: u32 = 0;
        let mut size: u32 = std::mem::size_of::<u32>() as u32;
        let mut index: u32 = 0;
        WinHttpQueryHeaders(
            request.0,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            None,
            Some(&mut status as *mut u32 as *mut core::ffi::c_void),
            &mut size,
            &mut index,
        )
        .map_err(|e| format!("status query failed: {e}"))?;
        if status != 200 {
            return Err(format!("HTTP {status} for {url}"));
        }

        let mut buf = vec![0u8; 64 * 1024];
        loop {
            let mut read: u32 = 0;
            WinHttpReadData(
                request.0,
                buf.as_mut_ptr() as *mut core::ffi::c_void,
                buf.len() as u32,
                &mut read,
            )
            .map_err(|e| format!("read failed: {e}"))?;
            if read == 0 {
                return Ok(());
            }
            sink(&buf[..read as usize])?;
        }
    }
}

/// GET a small document (the manifest) into a String.
pub fn get_to_string(url: &str) -> Result<String, String> {
    let mut body = Vec::new();
    get(url, |chunk| {
        body.extend_from_slice(chunk);
        Ok(())
    })?;
    String::from_utf8(body).map_err(|_| "manifest is not valid UTF-8".to_string())
}

/// GET a large file (a component zip) into `dest`, reporting the running
/// byte count via `on_progress`. Returns the total size. Checks `cancel`
/// between chunks and aborts.
pub fn get_to_file(
    url: &str,
    dest: &Path,
    cancel: &AtomicBool,
    on_progress: &dyn Fn(u64),
) -> Result<u64, String> {
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("staging dir: {e}"))?;
    }
    let mut file = std::fs::File::create(dest).map_err(|e| format!("staging file: {e}"))?;
    let mut total: u64 = 0;
    let result = get(url, |chunk| {
        if cancel.load(Ordering::Relaxed) {
            return Err("__cancelled".to_string());
        }
        file.write_all(chunk).map_err(|e| format!("staging write failed: {e}"))?;
        total += chunk.len() as u64;
        on_progress(total);
        Ok(())
    });
    match result {
        Ok(()) => Ok(total),
        Err(e) => {
            let _ = std::fs::remove_file(dest);
            Err(e)
        }
    }
}
