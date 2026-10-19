use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

pub const CANCELLED: &str = "cancelled";

pub fn check_cancel(cancel: &AtomicBool) -> Result<(), String> {
    if cancel.load(Ordering::SeqCst) {
        Err(CANCELLED.into())
    } else {
        Ok(())
    }
}

pub fn download(
    component: &str,
    info: &crate::manifest::ComponentInfo,
    dest: &Path,
    cancel: &AtomicBool,
    on_progress: &dyn Fn(u64),
    on_verify: &dyn Fn(),
) -> Result<(), String> {
    info.authenticate(component)?;
    check_cancel(cancel)?;
    let result = (|| {
        let downloaded = crate::http::get_to_file(&info.url, dest, info.size, cancel, on_progress)
            .map_err(|e| {
                if e == "__cancelled" {
                    CANCELLED.into()
                } else {
                    e
                }
            })?;
        on_verify();
        check_cancel(cancel)?;
        if downloaded != info.size {
            return Err("component size mismatch".into());
        }
        let digest = sha256_file_cancel(dest, cancel)?;
        if !digest.eq_ignore_ascii_case(&info.sha256) {
            return Err("component checksum mismatch".into());
        }
        check_cancel(cancel)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(dest);
    }
    result
}

fn sha256_file_cancel(path: &Path, cancel: &AtomicBool) -> Result<String, String> {
    check_cancel(cancel)?;
    let mut file = std::fs::File::open(path).map_err(|e| format!("re-read for verify: {e}"))?;
    hash_reader(&mut file, cancel)
}

fn hash_reader(file: &mut impl Read, cancel: &AtomicBool) -> Result<String, String> {
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        check_cancel(cancel)?;
        let n = file
            .read(&mut buf)
            .map_err(|e| format!("re-read for verify: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    check_cancel(cancel)?;
    Ok(format!("{:x}", hasher.finalize()))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cancelled_verification_does_not_accept_completed_download() {
        let path = std::env::temp_dir().join(format!("monolith-cancel-{}", std::process::id()));
        std::fs::write(&path, b"downloaded").unwrap();
        let cancel = AtomicBool::new(true);
        assert_eq!(sha256_file_cancel(&path, &cancel), Err(CANCELLED.into()));
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn hashing_bounds_reads_and_observes_cancellation_between_chunks() {
        struct Reader<'a> {
            cancel: &'a AtomicBool,
            reads: usize,
        }
        impl Read for Reader<'_> {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                assert!(buf.len() <= 64 * 1024);
                self.reads += 1;
                buf.fill(0);
                self.cancel.store(true, Ordering::SeqCst);
                Ok(buf.len())
            }
        }
        let cancel = AtomicBool::new(false);
        let mut reader = Reader {
            cancel: &cancel,
            reads: 0,
        };
        assert_eq!(hash_reader(&mut reader, &cancel), Err(CANCELLED.into()));
        assert_eq!(reader.reads, 1);
        cancel.store(false, Ordering::SeqCst);
        assert_eq!(
            hash_reader(&mut std::io::Cursor::new(b"abc"), &cancel).unwrap(),
            format!("{:x}", Sha256::digest(b"abc"))
        );
    }
}
