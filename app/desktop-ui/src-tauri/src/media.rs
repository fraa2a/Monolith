// Streams clip videos and thumbnails from disk to the webview with HTTP Range
// support (needed for <video> seeking). Paths are constrained to the configured
// output folders and file params are reduced to a basename to block traversal.
// Mirrors the Deno server/media.ts behaviour.

use crate::settings_store;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::windows::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use tiny_http::{Header, Request, Response, StatusCode};

// Windows share-mode flags (winnt.h). Rust's File::open omits FILE_SHARE_DELETE,
// so a streaming <video> read would hold an undeletable handle and the engine's
// DeleteFileW on that clip fails with a sharing violation. Opening media with
// all three share bits lets the engine delete/rename a clip while the webview is
// still reading it — the delete is honoured and the handle is invalidated.
const FILE_SHARE_READ: u32 = 0x0000_0001;
const FILE_SHARE_WRITE: u32 = 0x0000_0002;
const FILE_SHARE_DELETE: u32 = 0x0000_0004;

fn open_shared(path: &Path) -> std::io::Result<File> {
    File::options()
        .read(true)
        .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
        .open(path)
}

pub enum Source {
    Replay,
    Manual,
}

impl Source {
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "replay" => Some(Self::Replay),
            "manual" => Some(Self::Manual),
            _ => None,
        }
    }
}

fn folder_for(source: &Source) -> PathBuf {
    let dirs = settings_store::output_dirs();
    match source {
        Source::Replay => dirs.clips,
        Source::Manual => dirs.recs,
    }
}

fn header(name: &str, value: &str) -> Header {
    Header::from_bytes(name.as_bytes(), value.as_bytes()).expect("valid header")
}

fn respond_status(request: Request, code: u16, body: &str) {
    let _ = request.respond(Response::from_string(body).with_status_code(StatusCode(code)));
}

// Reduce a caller-supplied file name to a bare basename to block path traversal.
fn basename(file: &str) -> String {
    Path::new(file)
        .file_name()
        .map(|name| name.to_string_lossy().to_string())
        .unwrap_or_default()
}

fn ext_lower(name: &str) -> String {
    Path::new(name)
        .extension()
        .map(|ext| ext.to_string_lossy().to_lowercase())
        .unwrap_or_default()
}

// Parses a single `bytes=start-end` range against the file size.
// Returns (start, end_inclusive) or None when the header is absent/unparseable.
fn parse_range(range: &str, size: u64) -> Option<(u64, u64)> {
    let spec = range.strip_prefix("bytes=")?;
    let (start_s, end_s) = spec.split_once('-')?;
    let start = if start_s.is_empty() { 0 } else { start_s.parse().ok()? };
    let end = if end_s.is_empty() { size.saturating_sub(1) } else { end_s.parse().ok()? };
    Some((start, end))
}

fn serve_file(request: Request, path: &Path, mime: &str) {
    let mut file = match open_shared(path) {
        Ok(file) => file,
        Err(_) => return respond_status(request, 404, "Not found"),
    };
    let size = match file.metadata() {
        Ok(meta) => meta.len(),
        Err(_) => return respond_status(request, 404, "Not found"),
    };

    let range = request
        .headers()
        .iter()
        .find(|h| h.field.equiv("Range"))
        .map(|h| h.value.as_str().to_string());

    if let Some(range) = range {
        if let Some((start, end)) = parse_range(&range, size) {
            if size == 0 || start >= size || end >= size || start > end {
                let resp = Response::empty(StatusCode(416))
                    .with_header(header("Content-Range", &format!("bytes */{size}")));
                let _ = request.respond(resp);
                return;
            }
            let length = end - start + 1;
            if file.seek(SeekFrom::Start(start)).is_err() {
                return respond_status(request, 500, "seek failed");
            }
            let reader = file.take(length);
            let resp = Response::new(
                StatusCode(206),
                vec![
                    header("Content-Type", mime),
                    header("Accept-Ranges", "bytes"),
                    header("Content-Range", &format!("bytes {start}-{end}/{size}")),
                ],
                reader,
                Some(length as usize),
                None,
            );
            let _ = request.respond(resp);
            return;
        }
    }

    let resp = Response::new(
        StatusCode(200),
        vec![
            header("Content-Type", mime),
            header("Accept-Ranges", "bytes"),
        ],
        file,
        Some(size as usize),
        None,
    );
    let _ = request.respond(resp);
}

pub fn serve_media(request: Request, source: &Source, file: &str) {
    let name = basename(file);
    let mime = match ext_lower(&name).as_str() {
        "mp4" => "video/mp4",
        "mkv" => "video/x-matroska",
        _ => return respond_status(request, 415, "Unsupported"),
    };
    let path = folder_for(source).join(&name);
    serve_file(request, &path, mime);
}

pub fn serve_thumb(request: Request, source: &Source, file: &str) {
    let name = basename(file);
    if ext_lower(&name) != "png" {
        return respond_status(request, 415, "Unsupported");
    }
    let path = folder_for(source).join(".thumbs").join(&name);
    serve_file(request, &path, "image/png");
}
