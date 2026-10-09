use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    path::PathBuf,
};

pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

fn error(status: u16) -> Response {
    Response {
        status,
        headers: vec![("Access-Control-Allow-Origin".into(), "*".into())],
        body: Vec::new(),
    }
}

fn decode(path: &str) -> Option<PathBuf> {
    let bytes = path.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            let pair = std::str::from_utf8(bytes.get(i + 1..i + 3)?).ok()?;
            decoded.push(u8::from_str_radix(pair, 16).ok()?);
            i += 3;
        } else {
            decoded.push(bytes[i]);
            i += 1;
        }
    }
    if decoded.contains(&0) {
        return None;
    }
    let decoded = String::from_utf8(decoded).ok()?;
    #[cfg(windows)]
    let decoded = decoded.strip_prefix('/').unwrap_or(&decoded).to_string();
    Some(PathBuf::from(decoded))
}

fn serve_request(path: &str, range: Option<&str>, roots: &[PathBuf], head: bool) -> Response {
    let Some(path) = decode(path) else {
        return error(400);
    };
    let Ok(path) = path.canonicalize() else {
        return error(404);
    };
    if !roots
        .iter()
        .filter_map(|root| root.canonicalize().ok())
        .any(|root| path.starts_with(root))
    {
        return error(403);
    }
    let mime = match path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "mp4" => "video/mp4",
        "mkv" => "video/x-matroska",
        "webm" => "video/webm",
        "mov" => "video/quicktime",
        _ => return error(403),
    };
    let Ok(mut file) = fs::File::open(&path) else {
        return error(404);
    };
    let Ok(meta) = file.metadata() else {
        return error(500);
    };
    if !meta.is_file() {
        return error(404);
    }
    let size = meta.len();
    let (status, start, end) = if let Some(range) = range {
        let invalid = || {
            let mut response = error(416);
            response
                .headers
                .push(("Content-Range".into(), format!("bytes */{size}")));
            response
        };
        let Some(range) = range.strip_prefix("bytes=") else {
            return invalid();
        };
        let Some((first, last)) = range.split_once('-') else {
            return invalid();
        };
        if range.contains(',') || size == 0 {
            return invalid();
        }
        let (start, end) = if first.is_empty() {
            let Ok(suffix) = last.parse::<u64>() else {
                return invalid();
            };
            if suffix == 0 {
                return invalid();
            }
            (size.saturating_sub(suffix), size - 1)
        } else {
            let Ok(start) = first.parse::<u64>() else {
                return invalid();
            };
            let end = if last.is_empty() {
                size - 1
            } else {
                let Ok(end) = last.parse::<u64>() else {
                    return invalid();
                };
                end.min(size - 1)
            };
            (start, end)
        };
        if start >= size || start > end {
            return invalid();
        }
        (206, start, end.min(start.saturating_add(1024 * 1024 - 1)))
    } else {
        if !head && size > 8 * 1024 * 1024 {
            return error(413);
        }
        (200, 0, size.saturating_sub(1))
    };
    let len = if size == 0 { 0 } else { end - start + 1 };
    let mut body = Vec::new();
    if !head {
        body.reserve(len as usize);
        if file.seek(SeekFrom::Start(start)).is_err()
            || file.take(len).read_to_end(&mut body).is_err()
            || body.len() != len as usize
        {
            return error(500);
        }
    }
    let mut headers = vec![
        ("Content-Type".into(), mime.into()),
        ("Content-Length".into(), len.to_string()),
        ("Accept-Ranges".into(), "bytes".into()),
        ("Access-Control-Allow-Origin".into(), "*".into()),
        ("Cache-Control".into(), "no-store".into()),
    ];
    if status == 206 {
        headers.push((
            "Content-Range".into(),
            format!("bytes {start}-{end}/{size}"),
        ));
    }
    Response {
        status,
        headers,
        body,
    }
}

pub fn serve(path: &str, range: Option<&str>, roots: &[PathBuf]) -> Response {
    serve_request(path, range, roots, false)
}
pub fn serve_head(path: &str, range: Option<&str>, roots: &[PathBuf]) -> Response {
    serve_request(path, range, roots, true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_current_canonical_roots_are_readable_and_ranges_are_bounded() {
        let base = std::env::temp_dir().join(format!("monolith-media-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let a = base.join("a");
        let b = base.join("b");
        fs::create_dir_all(&a).unwrap();
        fs::create_dir_all(&b).unwrap();
        fs::write(a.join("one.png"), b"thumbnail").unwrap();
        fs::write(b.join("two.mp4"), b"0123456789").unwrap();
        let url = a.join("one.png").to_string_lossy().into_owned();
        assert_eq!(serve(&url, None, &[a.clone()]).body, b"thumbnail");
        assert_eq!(serve(&url, None, &[b.clone()]).status, 403);
        assert_eq!(serve(&url, None, &[a.clone()]).status, 200);
        assert_eq!(
            serve(
                &a.join("../b/two.mp4").to_string_lossy(),
                None,
                &[a.clone()]
            )
            .status,
            403
        );
        let video = b.join("two.mp4").to_string_lossy().into_owned();
        let response = serve(&video, Some("bytes=2-5"), &[b.clone()]);
        assert_eq!(response.status, 206);
        assert_eq!(response.body, b"2345");
        assert_eq!(serve(&video, Some("bytes=-3"), &[b.clone()]).body, b"789");
        for invalid in ["bytes=20-30", "bytes=4-2", "bytes=0-1,3-4", "items=1-2"] {
            assert_eq!(serve(&video, Some(invalid), &[b.clone()]).status, 416);
        }
        let large = b.join("large.mp4");
        fs::File::create(&large)
            .unwrap()
            .set_len(32 * 1024 * 1024)
            .unwrap();
        assert_eq!(
            serve(&large.to_string_lossy(), None, &[b.clone()]).status,
            413
        );
        let head = serve_head(&large.to_string_lossy(), None, &[b.clone()]);
        assert_eq!(head.status, 200);
        assert!(head.body.is_empty());
        assert!(head
            .headers
            .contains(&("Content-Length".into(), (32 * 1024 * 1024).to_string())));
        assert_eq!(
            serve(&large.to_string_lossy(), Some("bytes=0-"), &[b.clone()])
                .body
                .len(),
            1024 * 1024
        );
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(b.join("two.mp4"), a.join("escape.mp4")).unwrap();
            assert_eq!(
                serve(&a.join("escape.mp4").to_string_lossy(), None, &[a]).status,
                403
            );
        }
        fs::remove_dir_all(base).unwrap();
    }
}
