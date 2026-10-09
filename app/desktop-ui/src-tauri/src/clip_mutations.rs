use base64::{engine::general_purpose, Engine as _};
use serde_json::Value;
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

pub fn send(method: &str, params: Value) -> Result<(), String> {
    let bytes = serde_json::to_vec(&params).map_err(|err| err.to_string())?;
    if bytes.len() > 60 * 1024 {
        return Err("clip request too large".into());
    }
    let response = crate::engine_rpc::mutate_clip(method, params);
    if response.get("ok").and_then(Value::as_bool) == Some(true) {
        Ok(())
    } else {
        Err(response
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("engine error")
            .to_string())
    }
}

fn stage(root: &Path, data_url: &str) -> Result<(String, PathBuf), String> {
    if data_url.len() > 12 * 1024 * 1024 {
        return Err("thumbnail too large".into());
    }
    let encoded = data_url
        .strip_prefix("data:image/png;base64,")
        .ok_or_else(|| "thumbnail must be png".to_string())?;
    let png = general_purpose::STANDARD
        .decode(encoded)
        .map_err(|_| "bad thumbnail data".to_string())?;
    if png.len() > 8 * 1024 * 1024 || !png.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err("invalid png thumbnail".into());
    }
    fs::create_dir_all(root).map_err(|err| err.to_string())?;
    if let Ok(entries) = fs::read_dir(root) {
        for entry in entries.take(128).flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.len() == 36
                && name.ends_with(".png")
                && name[..32].bytes().all(|c| c.is_ascii_hexdigit())
            {
                if entry
                    .metadata()
                    .ok()
                    .and_then(|meta| meta.modified().ok())
                    .and_then(|time| time.elapsed().ok())
                    .is_some_and(|age| age.as_secs() > 86400)
                {
                    let _ = fs::remove_file(entry.path());
                }
            }
        }
    }
    let random = rusqlite::Connection::open_in_memory().map_err(|err| err.to_string())?;
    let token: String = random
        .query_row("SELECT lower(hex(randomblob(16)))", [], |row| row.get(0))
        .map_err(|err| err.to_string())?;
    let path = root.join(format!("{token}.png"));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .map_err(|err| err.to_string())?;
    if let Err(err) = file.write_all(&png).and_then(|_| file.sync_all()) {
        drop(file);
        let _ = fs::remove_file(&path);
        return Err(err.to_string());
    }
    Ok((token, path))
}

pub fn capture(
    source: &str,
    id: i64,
    catalog_uid: &str,
    clip_uid: &str,
    media_revision: i64,
    data_url: &str,
) -> Result<String, String> {
    let root = crate::paths::monolith_data_dir().join("thumb-upload");
    let (token, path) = stage(&root, data_url)?;
    let result = send(
        "clip_capture_thumb",
        serde_json::json!({"source":source,"id":id,"catalog_uid":catalog_uid,"clip_uid":clip_uid,"media_revision":media_revision,"upload_token":token}),
    );
    let _ = fs::remove_file(path);
    result?;
    crate::clip_catalog::clip_by_id(
        crate::clip_catalog::ClipSource::parse(source).ok_or_else(|| "bad source".to_string())?,
        id,
    )
    .and_then(|clip| clip.thumbnail_file)
    .ok_or_else(|| "thumbnail unavailable after capture".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn staging_bounds_validates_and_creates_unique_uploads() {
        let root = std::env::temp_dir().join(format!("monolith-upload-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let png = b"\x89PNG\r\n\x1a\nfixture";
        let encoded = format!(
            "data:image/png;base64,{}",
            general_purpose::STANDARD.encode(png)
        );
        let (token, first) = stage(&root, &encoded).unwrap();
        assert_eq!(token.len(), 32);
        assert!(token.bytes().all(|c| c.is_ascii_hexdigit()));
        assert_eq!(fs::read(&first).unwrap(), png);
        let (_, second) = stage(&root, &encoded).unwrap();
        assert_ne!(first, second);
        assert!(stage(&root, "data:image/png;base64,bm90LXBuZw==").is_err());
        assert!(stage(&root, &"a".repeat(12 * 1024 * 1024)).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
