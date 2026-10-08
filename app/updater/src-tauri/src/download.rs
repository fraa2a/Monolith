use base64::Engine;
use ed25519_dalek::{Signature, VerifyingKey};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::Path;
use std::sync::atomic::AtomicBool;

// Release components are signed with the CI Ed25519 key; this is its public half.
const PUBLIC_KEY_B64: &str = "GgyaSRupUFn5Omaa90w0H2xDTrqff2DdzRDtbeplvKA=";

/// Sentinel error string signalling a user cancel (not a failure).
pub const CANCELLED: &str = "cancelled";

pub fn download(
    url: &str,
    dest: &Path,
    expected_size: u64,
    expected_sha256: &str,
    ed_signature_b64: &str,
    cancel: &AtomicBool,
    on_progress: &dyn Fn(u64),
    on_verify: &dyn Fn(),
) -> Result<(), String> {
    crate::manifest::ComponentInfo {
        version: "0.0.0".into(),
        url: url.into(),
        size: expected_size,
        sha256: expected_sha256.into(),
        ed_signature: ed_signature_b64.into(),
    }
    .validate()?;
    let downloaded = crate::http::get_to_file(url, dest, expected_size, cancel, on_progress)
        .map_err(|e| {
            if e == "__cancelled" {
                CANCELLED.to_string()
            } else {
                e
            }
        })?;

    on_verify();
    if expected_size > 0 && downloaded != expected_size {
        let _ = std::fs::remove_file(dest);
        return Err(format!(
            "size mismatch - expected {expected_size} bytes, got {downloaded}"
        ));
    }
    if !expected_sha256.is_empty() {
        let digest = sha256_file(dest)?;
        if !digest.eq_ignore_ascii_case(expected_sha256) {
            let _ = std::fs::remove_file(dest);
            return Err("checksum mismatch - the download is corrupt".to_string());
        }
    }
    if let Err(e) = verify_signature(dest, ed_signature_b64) {
        let _ = std::fs::remove_file(dest);
        return Err(e);
    }
    Ok(())
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = std::fs::File::open(path).map_err(|e| format!("re-read for verify: {e}"))?;
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = file
            .read(&mut buf)
            .map_err(|e| format!("re-read for verify: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

fn verify_signature(path: &Path, sig_b64: &str) -> Result<(), String> {
    let key_bytes: [u8; 32] = base64::engine::general_purpose::STANDARD
        .decode(PUBLIC_KEY_B64)
        .map_err(|_| "bad public key encoding".to_string())?
        .try_into()
        .map_err(|_: Vec<u8>| "bad public key length".to_string())?;
    let sig_bytes: [u8; 64] = base64::engine::general_purpose::STANDARD
        .decode(sig_b64.trim())
        .map_err(|_| "bad signature encoding".to_string())?
        .try_into()
        .map_err(|_: Vec<u8>| "bad signature length".to_string())?;
    let key = VerifyingKey::from_bytes(&key_bytes).map_err(|_| "bad public key".to_string())?;
    let sig = Signature::from_bytes(&sig_bytes);
    let msg = std::fs::read(path).map_err(|e| format!("read for verify: {e}"))?;
    key.verify_strict(&msg, &sig)
        .map_err(|_| "signature verification failed".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_and_forged_signatures_are_rejected() {
        let path = std::env::temp_dir().join(format!("monolith-signature-{}", std::process::id()));
        std::fs::write(&path, b"untrusted component").unwrap();
        assert!(verify_signature(&path, "").is_err());
        let forged = base64::engine::general_purpose::STANDARD.encode([0u8; 64]);
        assert!(verify_signature(&path, &forged).is_err());
        std::fs::remove_file(path).unwrap();
    }
}
