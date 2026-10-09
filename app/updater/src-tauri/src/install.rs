use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

const DIR: &str = ".update-transaction";
const MAX_JOURNAL: u64 = 4 * 1024 * 1024;

#[derive(Clone, Copy, Deserialize, Serialize, PartialEq)]
enum Phase {
    Prepared,
    Committed,
    RolledBack,
}

#[derive(Deserialize, Serialize)]
struct Entry {
    path: PathBuf,
    prior: Option<String>,
    next: String,
}

#[derive(Deserialize, Serialize)]
struct Journal {
    schema: u32,
    phase: Phase,
    files: Vec<Entry>,
}

pub struct Transaction {
    root: PathBuf,
    journal: Journal,
}

fn plain(path: &Path) -> Result<Option<fs::Metadata>, String> {
    let m = match fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("inspect {}: {e}", path.display())),
    };
    #[cfg(windows)]
    let link = {
        use std::os::windows::fs::MetadataExt;
        m.file_attributes() & 0x400 != 0
    };
    #[cfg(not(windows))]
    let link = m.file_type().is_symlink();
    if link || (!m.is_file() && !m.is_dir()) {
        return Err(format!("unsupported filesystem entry: {}", path.display()));
    }
    Ok(Some(m))
}

fn relative(path: &Path) -> Result<(), String> {
    if path.as_os_str().is_empty() || path.components().count() > 32 {
        return Err("invalid update path".into());
    }
    for part in path.components() {
        let Component::Normal(name) = part else {
            return Err("non-relative update path".into());
        };
        let name = name.to_string_lossy();
        let stem = name.split('.').next().unwrap_or("").to_ascii_uppercase();
        if name.contains([':', '\\', '*', '?', '"', '<', '>', '|'])
            || name.ends_with(['.', ' '])
            || name.eq_ignore_ascii_case(DIR)
            || ["CON", "PRN", "AUX", "NUL"].contains(&stem.as_str())
            || (stem.len() == 4
                && (stem.starts_with("COM") || stem.starts_with("LPT"))
                && matches!(stem.as_bytes()[3], b'1'..=b'9'))
        {
            return Err(format!("unsupported update path: {}", path.display()));
        }
    }
    Ok(())
}

fn target(root: &Path, rel: &Path) -> Result<PathBuf, String> {
    relative(rel)?;
    let mut path = root.to_path_buf();
    for part in rel.components() {
        path.push(part);
        plain(&path)?;
    }
    Ok(path)
}

fn digest(path: &Path) -> Result<String, String> {
    if !plain(path)?.is_some_and(|m| m.is_file()) {
        return Err(format!("expected regular file: {}", path.display()));
    }
    let mut f = File::open(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = [0; 64 * 1024];
    loop {
        let n = f.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}

fn verify(path: &Path, hash: &str) -> Result<(), String> {
    if digest(path)? != hash {
        return Err(format!("file changed during update: {}", path.display()));
    }
    Ok(())
}

fn sync_dir(path: &Path) -> Result<(), String> {
    #[cfg(not(windows))]
    File::open(path)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    #[cfg(windows)]
    let _ = path;
    Ok(())
}

fn mkdir(path: &Path) -> Result<(), String> {
    if let Some(m) = plain(path)? {
        return if m.is_dir() {
            Ok(())
        } else {
            Err(format!("not a directory: {}", path.display()))
        };
    }
    if let Some(parent) = path.parent() {
        mkdir(parent)?;
        fs::create_dir(path).map_err(|e| format!("mkdir {}: {e}", path.display()))?;
        sync_dir(parent)?;
    }
    Ok(())
}

fn move_file(src: &Path, dest: &Path, replace: bool) -> Result<(), String> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows::core::PCWSTR;
        use windows::Win32::Storage::FileSystem::{
            MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
        };
        let wide = |p: &Path| {
            p.as_os_str()
                .encode_wide()
                .chain(Some(0))
                .collect::<Vec<_>>()
        };
        let s = wide(src);
        let d = wide(dest);
        let flags = if replace {
            MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING
        } else {
            MOVEFILE_WRITE_THROUGH
        };
        unsafe { MoveFileExW(PCWSTR(s.as_ptr()), PCWSTR(d.as_ptr()), flags) }
            .map_err(|e| e.to_string())?;
    }
    #[cfg(not(windows))]
    {
        if !replace && plain(dest)?.is_some() {
            return Err(format!("destination exists: {}", dest.display()));
        }
        fs::rename(src, dest).map_err(|e| format!("move {}: {e}", src.display()))?;
        sync_dir(src.parent().ok_or("missing source parent")?)?;
        sync_dir(dest.parent().ok_or("missing destination parent")?)?;
    }
    Ok(())
}

fn save(root: &Path, journal: &Journal) -> Result<(), String> {
    let bytes = serde_json::to_vec(journal).map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_JOURNAL {
        return Err("update journal is too large".into());
    }
    let dir = root.join(DIR);
    let tmp = dir.join("journal.tmp");
    if plain(&tmp)?.is_some() {
        fs::remove_file(&tmp).map_err(|e| e.to_string())?;
    }
    let mut f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .map_err(|e| e.to_string())?;
    f.write_all(&bytes)
        .and_then(|_| f.sync_all())
        .map_err(|e| e.to_string())?;
    drop(f);
    move_file(&tmp, &dir.join("journal.json"), true)
}

fn load(root: &Path) -> Result<Option<Journal>, String> {
    let path = root.join(DIR).join("journal.json");
    if plain(&path)?.is_none() {
        return Ok(None);
    }
    let mut bytes = Vec::new();
    File::open(&path)
        .map_err(|e| e.to_string())?
        .take(MAX_JOURNAL + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_JOURNAL {
        return Err("update journal is too large".into());
    }
    let journal: Journal =
        serde_json::from_slice(&bytes).map_err(|e| format!("update journal: {e}"))?;
    if journal.schema != 1 || journal.files.is_empty() || journal.files.len() > 12289 {
        return Err("invalid update journal".into());
    }
    let mut names = HashSet::new();
    for e in &journal.files {
        relative(&e.path)?;
        if !names.insert(e.path.to_string_lossy().to_lowercase()) {
            return Err("duplicate update path".into());
        }
        for hash in std::iter::once(&e.next).chain(e.prior.iter()) {
            if hash.len() != 64 || !hash.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err("invalid journal hash".into());
            }
        }
    }
    Ok(Some(journal))
}

fn cleanup(root: &Path) {
    // Loaded parked binaries can retain this directory until the next launch.
    let _ = fs::remove_dir_all(root.join(DIR));
}

pub fn pending(root: &Path) -> Result<bool, String> {
    if let Some(m) = plain(&root.join(DIR))? {
        if !m.is_dir() {
            return Err("invalid transaction directory".into());
        }
    } else {
        return Ok(false);
    }
    Ok(load(root)?.is_some_and(|j| j.phase == Phase::Prepared))
}

pub fn recover(root: &Path) -> Result<(), String> {
    match plain(&root.join(DIR))? {
        None => return Ok(()),
        Some(m) if m.is_dir() => {}
        _ => return Err("invalid transaction directory".into()),
    }
    let Some(mut j) = load(root)? else {
        cleanup(root);
        return Ok(());
    };
    if j.phase != Phase::Prepared {
        cleanup(root);
        return Ok(());
    }
    let dir = root.join(DIR);
    let discard = dir.join("discard").join(format!(
        "{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos()
    ));
    for e in j.files.iter().rev() {
        let installed = target(root, &e.path)?;
        let backup = target(&dir.join("backup"), &e.path)?;
        if let Some(prior) = &e.prior {
            if plain(&backup)?.is_some() {
                verify(&backup, prior)?;
                if plain(&installed)?.is_some() {
                    verify(&installed, &e.next)?;
                    let parked = discard.join(&e.path);
                    mkdir(parked.parent().ok_or("missing discard parent")?)?;
                    move_file(&installed, &parked, false)?;
                }
                move_file(&backup, &installed, false)?;
            } else {
                verify(&installed, prior)?;
            }
        } else if plain(&dir.join("new").join(&e.path))?.is_none() {
            if plain(&installed)?.is_some() {
                verify(&installed, &e.next)?;
                let parked = discard.join(&e.path);
                mkdir(parked.parent().ok_or("missing discard parent")?)?;
                move_file(&installed, &parked, false)?;
            }
        } else if plain(&installed)?.is_some() {
            return Err(format!(
                "unexpected recovery target: {}",
                installed.display()
            ));
        }
    }
    j.phase = Phase::RolledBack;
    save(root, &j)?;
    cleanup(root);
    Ok(())
}

impl Transaction {
    pub fn prepare(root: &Path, files: &[(PathBuf, PathBuf)]) -> Result<Self, String> {
        if !plain(root)?.is_some_and(|m| m.is_dir()) {
            return Err("app directory is unavailable".into());
        }
        recover(root)?;
        let dir = root.join(DIR);
        if plain(&dir)?.is_some() {
            return Err(
                "parked update files are still in use; close Monolith and restart the updater"
                    .into(),
            );
        }
        if files.is_empty() || files.len() > 12289 {
            return Err("invalid update file count".into());
        }
        let mut names = HashSet::new();
        let mut entries = Vec::new();
        for (src, rel) in files {
            let dest = target(root, rel)?;
            if !names.insert(rel.to_string_lossy().to_lowercase()) {
                return Err("duplicate update path".into());
            }
            let prior = if plain(&dest)?.is_some() {
                Some(digest(&dest)?)
            } else {
                None
            };
            entries.push(Entry {
                path: rel.clone(),
                prior,
                next: digest(src)?,
            });
        }
        if entries
            .iter()
            .any(|entry| entry.path == Path::new("Updater.exe") && entry.prior.is_some())
        {
            let recovery = root.join(".update-recovery");
            mkdir(&recovery)?;
            let launcher = recovery.join("Updater.exe");
            if plain(&launcher)?.is_none() {
                let temporary = recovery.join("Updater.tmp");
                let mut input = File::open(root.join("Updater.exe")).map_err(|e| e.to_string())?;
                let mut output = File::create(&temporary).map_err(|e| e.to_string())?;
                std::io::copy(&mut input, &mut output)
                    .and_then(|_| output.sync_all())
                    .map_err(|e| e.to_string())?;
                drop(output);
                verify(&temporary, &digest(&root.join("Updater.exe"))?)?;
                move_file(&temporary, &launcher, false)?;
            }
        }
        mkdir(&dir)?;
        for ((src, rel), e) in files.iter().zip(&entries) {
            let new = dir.join("new").join(rel);
            mkdir(new.parent().ok_or("missing staged parent")?)?;
            let mut input = File::open(src).map_err(|e| e.to_string())?;
            let mut out = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&new)
                .map_err(|e| e.to_string())?;
            std::io::copy(&mut input, &mut out)
                .and_then(|_| out.sync_all())
                .map_err(|e| e.to_string())?;
            drop(out);
            verify(&new, &e.next)?;
        }
        let j = Journal {
            schema: 1,
            phase: Phase::Prepared,
            files: entries,
        };
        save(root, &j)?;
        Ok(Self {
            root: root.into(),
            journal: j,
        })
    }

    pub fn abort(self) -> Result<(), String> {
        recover(&self.root)
    }

    pub fn install(mut self) -> Result<(), String> {
        if let Err(error) = self.place(&mut |_| Ok(())) {
            return match recover(&self.root) {
                Ok(()) => Err(error),
                Err(restore) => Err(format!("{error}; recovery required: {restore}")),
            };
        }
        self.journal.phase = Phase::Committed;
        if let Err(e) = save(&self.root, &self.journal) {
            recover(&self.root).map_err(|restore| format!("{e}; recovery required: {restore}"))?;
            return Err(e);
        }
        cleanup(&self.root);
        Ok(())
    }

    fn place(
        &self,
        checkpoint: &mut impl FnMut(usize) -> Result<(), String>,
    ) -> Result<(), String> {
        let dir = self.root.join(DIR);
        let mut step = 0;
        for e in &self.journal.files {
            let dest = target(&self.root, &e.path)?;
            let new = target(&dir.join("new"), &e.path)?;
            verify(&new, &e.next)?;
            mkdir(dest.parent().ok_or("missing destination parent")?)?;
            if let Some(prior) = &e.prior {
                verify(&dest, prior)?;
                let backup = dir.join("backup").join(&e.path);
                mkdir(backup.parent().ok_or("missing backup parent")?)?;
                move_file(&dest, &backup, false)?;
                step += 1;
                checkpoint(step)?;
            }
            move_file(&new, &dest, false)?;
            step += 1;
            checkpoint(step)?;
        }
        Ok(())
    }
}

pub fn files(src: &Path, prefix: &Path) -> Result<Vec<(PathBuf, PathBuf)>, String> {
    fn walk(
        root: &Path,
        src: &Path,
        prefix: &Path,
        out: &mut Vec<(PathBuf, PathBuf)>,
    ) -> Result<(), String> {
        if !plain(src)?.is_some_and(|m| m.is_dir()) {
            return Err("component directory is unavailable".into());
        }
        for entry in fs::read_dir(src).map_err(|e| e.to_string())? {
            let p = entry.map_err(|e| e.to_string())?.path();
            let m = plain(&p)?.ok_or("component entry disappeared")?;
            if m.is_dir() {
                walk(root, &p, prefix, out)?;
            } else {
                let rel = prefix.join(p.strip_prefix(root).map_err(|e| e.to_string())?);
                relative(&rel)?;
                out.push((p, rel));
            }
        }
        Ok(())
    }
    let mut out = Vec::new();
    walk(src, src, prefix, &mut out)?;
    out.sort_by(|a, b| a.1.cmp(&b.1));
    if out.is_empty() {
        return Err("component is empty".into());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);

    struct Fixture {
        root: PathBuf,
        stage: PathBuf,
        plan: Vec<(PathBuf, PathBuf)>,
    }
    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!(
                "monolith-install-{}-{}",
                std::process::id(),
                SEQ.fetch_add(1, Ordering::Relaxed)
            ));
            let _ = fs::remove_dir_all(&root);
            mkdir(&root).unwrap();
            let stage = root.join("stage");
            mkdir(&stage).unwrap();
            let mut plan = Vec::new();
            for name in [
                "Monolith.exe",
                "ui/UI.exe",
                "Updater.exe",
                "new.dll",
                "components.json",
            ] {
                let rel = PathBuf::from(name);
                mkdir(stage.join(&rel).parent().unwrap()).unwrap();
                fs::write(stage.join(&rel), format!("new-{name}")).unwrap();
                if name != "new.dll" {
                    mkdir(root.join(&rel).parent().unwrap()).unwrap();
                    fs::write(root.join(&rel), format!("old-{name}")).unwrap();
                }
                plan.push((stage.join(&rel), rel));
            }
            fs::write(root.join("unrelated.json"), b"user settings").unwrap();
            Self { root, stage, plan }
        }
        fn original(&self) {
            for (_, rel) in &self.plan {
                if rel == Path::new("new.dll") {
                    assert!(!self.root.join(rel).exists());
                } else {
                    assert_eq!(
                        fs::read_to_string(self.root.join(rel)).unwrap(),
                        format!("old-{}", rel.to_str().unwrap().replace('\\', "/"))
                    );
                }
            }
            assert_eq!(
                fs::read(self.root.join("unrelated.json")).unwrap(),
                b"user settings"
            );
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    #[test]
    fn updater_recovery_entry_survives_missing_normal_executable() {
        let f = Fixture::new();
        let tx = Transaction::prepare(
            &f.root,
            &[(f.stage.join("Updater.exe"), "Updater.exe".into())],
        )
        .unwrap();
        tx.place(&mut |step| {
            if step == 1 {
                Err("interrupted".into())
            } else {
                Ok(())
            }
        })
        .unwrap_err();
        assert!(!f.root.join("Updater.exe").exists());
        assert_eq!(
            fs::read(f.root.join(".update-recovery/Updater.exe")).unwrap(),
            b"old-Updater.exe"
        );
        recover(&f.root).unwrap();
        f.original();
    }
    #[test]
    fn every_interrupted_move_restores_all_components_and_versions() {
        for fail_at in 1..=9 {
            let f = Fixture::new();
            let tx = Transaction::prepare(&f.root, &f.plan).unwrap();
            assert!(tx
                .place(&mut |step| if step == fail_at {
                    Err("interrupted".into())
                } else {
                    Ok(())
                })
                .is_err());
            drop(tx);
            recover(&f.root).unwrap();
            recover(&f.root).unwrap();
            f.original();
            assert!(f.stage.join("Monolith.exe").is_file());
        }
    }

    #[test]
    fn late_placement_failure_rolls_back_the_whole_install() {
        let f = Fixture::new();
        let tx = Transaction::prepare(&f.root, &f.plan).unwrap();
        fs::remove_file(f.root.join(DIR).join("new/components.json")).unwrap();
        assert!(tx.install().is_err());
        f.original();
    }

    #[test]
    fn interrupted_recovery_can_resume_without_overwriting_foreign_data() {
        let f = Fixture::new();
        let tx = Transaction::prepare(&f.root, &f.plan).unwrap();
        tx.place(&mut |_| Ok(())).unwrap();
        fs::write(f.root.join("Monolith.exe"), b"external replacement").unwrap();
        assert!(recover(&f.root).is_err());
        assert_eq!(
            fs::read(f.root.join("Monolith.exe")).unwrap(),
            b"external replacement"
        );
        fs::write(f.root.join("Monolith.exe"), b"new-Monolith.exe").unwrap();
        recover(&f.root).unwrap();
        f.original();
    }

    #[test]
    fn committed_install_and_abort_have_truthful_metadata() {
        let f = Fixture::new();
        Transaction::prepare(&f.root, &f.plan)
            .unwrap()
            .abort()
            .unwrap();
        f.original();
        Transaction::prepare(&f.root, &f.plan)
            .unwrap()
            .install()
            .unwrap();
        recover(&f.root).unwrap();
        for (_, rel) in &f.plan {
            assert_eq!(
                fs::read_to_string(f.root.join(rel)).unwrap(),
                format!("new-{}", rel.to_str().unwrap().replace('\\', "/"))
            );
        }
        assert_eq!(
            fs::read(f.root.join("unrelated.json")).unwrap(),
            b"user settings"
        );
    }

    #[test]
    fn committed_journal_survives_restart_without_rollback() {
        let f = Fixture::new();
        let mut tx = Transaction::prepare(&f.root, &f.plan).unwrap();
        tx.place(&mut |_| Ok(())).unwrap();
        tx.journal.phase = Phase::Committed;
        save(&f.root, &tx.journal).unwrap();
        drop(tx);
        assert!(!pending(&f.root).unwrap());
        recover(&f.root).unwrap();
        for (_, rel) in &f.plan {
            assert_eq!(
                fs::read_to_string(f.root.join(rel)).unwrap(),
                format!("new-{}", rel.to_str().unwrap().replace('\\', "/"))
            );
        }
    }

    #[test]
    fn unfinished_preparation_does_not_change_installed_files() {
        let f = Fixture::new();
        mkdir(&f.root.join(DIR).join("new")).unwrap();
        fs::write(f.root.join(DIR).join("new/orphan"), b"partial copy").unwrap();
        assert!(!pending(&f.root).unwrap());
        recover(&f.root).unwrap();
        f.original();
        Transaction::prepare(&f.root, &f.plan)
            .unwrap()
            .abort()
            .unwrap();
        f.original();
    }

    #[test]
    fn malformed_journal_and_case_aliases_are_rejected() {
        let f = Fixture::new();
        let mut plan = f.plan.clone();
        plan.push((f.stage.join("Monolith.exe"), "MONOLITH.EXE".into()));
        assert!(Transaction::prepare(&f.root, &plan).is_err());
        for path in [
            "../outside",
            "/outside",
            "a:stream",
            "CON.txt",
            "a.",
            ".update-transaction/a",
        ] {
            assert!(
                Transaction::prepare(&f.root, &[(f.stage.join("Monolith.exe"), path.into())])
                    .is_err()
            );
        }
        mkdir(&f.root.join(DIR)).unwrap();
        fs::write(f.root.join(DIR).join("journal.json"), br#"{"schema":1,"phase":"Prepared","files":[{"path":"../outside","prior":null,"next":"00"}]}"#).unwrap();
        assert!(recover(&f.root).is_err());
        f.original();
    }

    #[cfg(unix)]
    #[test]
    fn linked_destination_is_rejected_before_any_replacement() {
        let f = Fixture::new();
        let other = f.root.join("other");
        mkdir(&other).unwrap();
        fs::remove_dir_all(f.root.join("ui")).unwrap();
        std::os::unix::fs::symlink(&other, f.root.join("ui")).unwrap();
        assert!(Transaction::prepare(&f.root, &f.plan).is_err());
        assert!(!other.join("UI.exe").exists());
    }

    #[cfg(windows)]
    #[test]
    #[ignore]
    fn recovery_launcher_child() {
        let root = crate::paths::app_dir();
        assert_eq!(
            root,
            PathBuf::from(std::env::var_os("MONOLITH_RECOVERY_ROOT").unwrap())
        );
        recover(&root).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn interrupted_self_update_can_launch_recovery_from_installation() {
        for stop in [1, 2] {
            let f = Fixture::new();
            let exe = f.root.join("Updater.exe");
            fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
            let original = digest(&exe).unwrap();
            let tx = Transaction::prepare(
                &f.root,
                &[(f.stage.join("Updater.exe"), "Updater.exe".into())],
            )
            .unwrap();
            tx.place(&mut |step| {
                if step == stop {
                    Err("interrupted".into())
                } else {
                    Ok(())
                }
            })
            .unwrap_err();
            let status = std::process::Command::new(f.root.join(".update-recovery/Updater.exe"))
                .args([
                    "--ignored",
                    "--exact",
                    "install::tests::recovery_launcher_child",
                ])
                .env("MONOLITH_RECOVERY_ROOT", &f.root)
                .env_remove("MONOLITH_APP_DIR")
                .status()
                .unwrap();
            assert!(status.success());
            assert_eq!(digest(&exe).unwrap(), original);
        }
    }

    #[cfg(windows)]
    #[test]
    #[ignore]
    fn loaded_child() {
        let ready = std::env::var_os("MONOLITH_INSTALL_READY").unwrap();
        fs::write(ready, b"ready").unwrap();
        std::thread::sleep(std::time::Duration::from_secs(300));
    }

    #[cfg(windows)]
    #[test]
    fn loaded_updater_image_can_be_replaced_and_recovered() {
        let f = Fixture::new();
        let exe = f.root.join("Updater.exe");
        fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
        let original = digest(&exe).unwrap();
        let ready = f.root.join("ready");
        let mut child = std::process::Command::new(&exe)
            .args(["--ignored", "--exact", "install::tests::loaded_child"])
            .env("MONOLITH_INSTALL_READY", &ready)
            .spawn()
            .unwrap();
        let result = std::panic::catch_unwind(|| {
            for _ in 0..500 {
                if ready.exists() {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            assert!(ready.exists());
            let tx = Transaction::prepare(
                &f.root,
                &[(f.stage.join("Updater.exe"), "Updater.exe".into())],
            )
            .unwrap();
            tx.place(&mut |_| Ok(())).unwrap();
            recover(&f.root).unwrap();
            assert_eq!(digest(&exe).unwrap(), original);
        });
        let _ = child.kill();
        let _ = child.wait();
        result.unwrap();
    }
}
