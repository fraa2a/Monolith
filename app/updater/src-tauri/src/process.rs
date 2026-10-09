use std::path::Path;
use std::process::Child;
use std::time::{Duration, Instant};

pub fn wait_ready(
    child: &mut Child,
    timeout: Duration,
    ready: impl Fn() -> bool,
) -> Result<(), String> {
    let start = Instant::now();
    let mut ready_since = None;
    loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            return Err(format!("engine exited during startup: {status}"));
        }
        if ready() {
            let since = ready_since.get_or_insert_with(Instant::now);
            if since.elapsed() >= Duration::from_millis(500) {
                return Ok(());
            }
        } else {
            ready_since = None;
        }
        if start.elapsed() >= timeout {
            return Err("engine readiness timed out".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn image_path(path: &Path) -> Result<String, String> {
    let parent = std::fs::canonicalize(path.parent().ok_or("engine path has no parent")?)
        .map_err(|e| format!("engine directory: {e}"))?;
    let path = parent.join(path.file_name().ok_or("engine path has no filename")?);
    Ok(path
        .to_string_lossy()
        .trim_start_matches("\\\\?\\")
        .to_lowercase())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(not(windows))]
    #[test]
    fn image_identity_resolves_parent_aliases_when_executable_is_missing() {
        let root = std::env::temp_dir().join(format!("monolith-image-path-{}", std::process::id()));
        std::fs::create_dir_all(root.join("installation")).unwrap();
        std::os::unix::fs::symlink(root.join("installation"), root.join("alias")).unwrap();
        let expected = image_path(&root.join("installation/Monolith.exe")).unwrap();
        let actual = image_path(&root.join("alias/Monolith.exe")).unwrap();
        std::fs::remove_dir_all(root).unwrap();
        assert_eq!(actual, expected);
    }
    #[cfg(windows)]
    #[test]
    fn image_identity_expands_short_directory_names_when_executable_is_missing() {
        use std::os::windows::ffi::OsStrExt;
        use windows::core::PCWSTR;
        use windows::Win32::Storage::FileSystem::GetShortPathNameW;
        let root =
            std::env::temp_dir().join(format!("monolith image directory {}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let wide: Vec<u16> = root
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let mut short = vec![0u16; 32768];
        let length = unsafe { GetShortPathNameW(PCWSTR(wide.as_ptr()), Some(&mut short)) } as usize;
        assert!(length > 0 && length < short.len());
        let alias = std::path::PathBuf::from(String::from_utf16_lossy(&short[..length]));
        let expected =
            image_path(&std::fs::canonicalize(&root).unwrap().join("Monolith.exe")).unwrap();
        let actual = image_path(&alias.join("Monolith.exe")).unwrap();
        std::fs::remove_dir_all(root).unwrap();
        assert_eq!(actual, expected);
    }
    #[test]
    #[ignore]
    fn short_lived_child() {
        std::thread::sleep(Duration::from_millis(100));
    }

    #[test]
    fn child_must_remain_alive_during_ready_handshake() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--ignored", "--exact", "process::tests::short_lived_child"])
            .spawn()
            .unwrap();
        assert!(wait_ready(&mut child, Duration::from_secs(3), || true).is_err());
        child.wait().unwrap();
    }

    #[cfg(windows)]
    #[test]
    #[ignore]
    fn slow_exit_child() {
        let root = std::path::PathBuf::from(std::env::var_os("MONOLITH_PROCESS_TEST").unwrap());
        std::fs::write(root.join("ready"), b"ready").unwrap();
        while !root.join("exit").exists() {
            std::thread::sleep(Duration::from_millis(10));
        }
        std::thread::sleep(Duration::from_millis(400));
    }

    #[cfg(windows)]
    #[test]
    fn closed_control_socket_does_not_mean_process_exited() {
        let root = std::env::temp_dir().join(format!("monolith-process-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let exe = root.join("Monolith.exe");
        std::fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
        let mut child = std::process::Command::new(&exe)
            .args(["--ignored", "--exact", "process::tests::slow_exit_child"])
            .env("MONOLITH_PROCESS_TEST", &root)
            .spawn()
            .unwrap();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            for _ in 0..500 {
                if root.join("ready").exists() {
                    break;
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            assert!(root.join("ready").exists());
            assert!(
                child.try_wait().unwrap().is_none(),
                "fixture exited before discovery"
            );
            let processes = EngineProcesses::capture(&exe).unwrap();
            assert!(
                processes.running(),
                "live child {} at {:?} was not discovered",
                child.id(),
                exe
            );
            std::fs::write(root.join("exit"), b"exit").unwrap();
            assert!(processes.wait_exit(Duration::from_millis(50)).is_err());
            processes.wait_exit(Duration::from_secs(5)).unwrap();
        }));
        let _ = child.kill();
        let _ = child.wait();
        std::fs::remove_dir_all(root).unwrap();
        result.unwrap();
    }
    #[test]
    fn engine_that_exits_immediately_cannot_be_reported_ready() {
        let mut command = if cfg!(windows) {
            let mut command = std::process::Command::new("cmd");
            command.args(["/c", "exit", "1"]);
            command
        } else {
            let mut command = std::process::Command::new("sh");
            command.args(["-c", "exit 1"]);
            command
        };
        let mut child = command.spawn().unwrap();
        child.wait().unwrap();
        assert!(wait_ready(&mut child, Duration::from_secs(1), || true).is_err());
    }
}

#[cfg(windows)]
pub struct EngineProcesses(Vec<windows::Win32::Foundation::HANDLE>);

#[cfg(windows)]
impl Drop for EngineProcesses {
    fn drop(&mut self) {
        for handle in &self.0 {
            unsafe {
                let _ = windows::Win32::Foundation::CloseHandle(*handle);
            }
        }
    }
}

#[cfg(windows)]
impl EngineProcesses {
    pub fn capture(exe: &Path) -> Result<Self, String> {
        use windows::core::PWSTR;
        use windows::Win32::Foundation::CloseHandle;
        use windows::Win32::System::Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
            TH32CS_SNAPPROCESS,
        };
        use windows::Win32::System::Threading::{
            OpenProcess, QueryFullProcessImageNameW, PROCESS_ACCESS_RIGHTS, PROCESS_NAME_WIN32,
            PROCESS_QUERY_LIMITED_INFORMATION,
        };
        let expected = image_path(exe)?;
        let mut processes = Self(Vec::new());
        unsafe {
            let snapshot =
                CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0).map_err(|e| e.to_string())?;
            let result = (|| {
                let mut entry = PROCESSENTRY32W {
                    dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
                    ..Default::default()
                };
                if Process32FirstW(snapshot, &mut entry).is_err() {
                    return Ok(());
                }
                loop {
                    let len = entry
                        .szExeFile
                        .iter()
                        .position(|c| *c == 0)
                        .unwrap_or(entry.szExeFile.len());
                    let name = String::from_utf16_lossy(&entry.szExeFile[..len]);
                    if exe
                        .file_name()
                        .is_some_and(|file| file.to_string_lossy().eq_ignore_ascii_case(&name))
                    {
                        let handle = OpenProcess(
                            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_ACCESS_RIGHTS(0x00100000),
                            false,
                            entry.th32ProcessID,
                        )
                        .map_err(|e| {
                            format!("opening engine process {}: {e}", entry.th32ProcessID)
                        })?;
                        let mut path = vec![0u16; 32768];
                        let mut size = path.len() as u32;
                        let query = QueryFullProcessImageNameW(
                            handle,
                            PROCESS_NAME_WIN32,
                            PWSTR(path.as_mut_ptr()),
                            &mut size,
                        );
                        if let Err(e) = query {
                            let _ = CloseHandle(handle);
                            return Err(format!("engine process path: {e}"));
                        }
                        let actual = String::from_utf16_lossy(&path[..size as usize]);
                        let actual = image_path(Path::new(&actual)).map_err(|error| {
                            let _ = CloseHandle(handle);
                            error
                        })?;
                        if actual == expected {
                            processes.0.push(handle);
                        } else {
                            let _ = CloseHandle(handle);
                        }
                    }
                    if Process32NextW(snapshot, &mut entry).is_err() {
                        break;
                    }
                }
                Ok(())
            })();
            let _ = CloseHandle(snapshot);
            result?;
        }
        Ok(processes)
    }

    pub fn running(&self) -> bool {
        !self.0.is_empty()
    }

    pub fn wait_exit(&self, timeout: Duration) -> Result<(), String> {
        use windows::Win32::Foundation::WAIT_OBJECT_0;
        use windows::Win32::System::Threading::WaitForSingleObject;
        let start = Instant::now();
        for handle in &self.0 {
            let remaining = timeout
                .saturating_sub(start.elapsed())
                .as_millis()
                .min(u32::MAX as u128 - 1) as u32;
            if unsafe { WaitForSingleObject(*handle, remaining) } != WAIT_OBJECT_0 {
                return Err("engine process did not exit in time".into());
            }
        }
        Ok(())
    }
}
