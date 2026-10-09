// Component updater host; publish state to the frontend over update-state events.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod apply;
mod archive;
mod download;
mod engine_rpc;
mod http;
mod install;
mod manifest;
mod paths;
mod process;
mod state;
mod versions;

use state::{CompStatus, ComponentState, Core, Phase};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

static CORE: OnceLock<Core> = OnceLock::new();
static FORCE: AtomicBool = AtomicBool::new(false);

fn core() -> &'static Core {
    CORE.get_or_init(Core::new)
}

fn emit_state(app: &AppHandle) {
    let _ = app.emit("update-state", core().snapshot());
}

fn set_phase(app: &AppHandle, phase: Phase) {
    core().state.lock().unwrap().phase = phase;
    emit_state(app);
}

fn fail(app: &AppHandle, message: &str) {
    let mut s = core().state.lock().unwrap();
    s.phase = Phase::Failed;
    s.error = Some(message.to_string());
    drop(s);
    emit_state(app);
}

#[tauri::command]
fn updater_state() -> serde_json::Value {
    core().snapshot()
}

#[tauri::command]
fn updater_start(app: AppHandle) {
    if core().phase() != Phase::Available || core().busy.swap(true, Ordering::SeqCst) {
        return;
    }
    core().cancel.store(false, Ordering::SeqCst);
    std::thread::spawn(move || {
        run_pipeline(&app);
        core().busy.store(false, Ordering::SeqCst);
    });
}

#[tauri::command]
fn updater_cancel() {
    core().request_cancel();
}

#[tauri::command]
fn updater_retry(app: AppHandle) {
    if core().busy.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(move || {
        run_check(&app, false);
        core().busy.store(false, Ordering::SeqCst);
    });
}

/// Use the first three Windows FileVersion fields for semver comparison.
fn three_part(v: &str) -> String {
    v.split('.').take(3).collect::<Vec<_>>().join(".")
}

fn run_check(app: &AppHandle, auto: bool) {
    if let Err(e) = recover_update(&paths::app_dir()) {
        fail(app, &format!("recovering interrupted update: {e}"));
        if let Some(win) = app.get_webview_window("main") {
            let _ = win.show();
        }
        return;
    }
    apply::sweep_old(&paths::app_dir());
    *core().manifest.lock().unwrap() = None;
    set_phase(app, Phase::Checking);

    let m = match manifest::fetch() {
        Ok(m) => m,
        Err(e) => {
            // Silent startup checks suppress their error window.
            if auto {
                std::process::exit(0);
            }
            fail(app, &e);
            return;
        }
    };

    let installed = versions::installed();
    let force = FORCE.load(Ordering::Relaxed);
    let mut comps: Vec<ComponentState> = Vec::new();
    for key in ["engine", "ui", "updater"] {
        let Some(info) = m.components.get(key) else {
            continue;
        };
        let from = match key {
            "engine" => installed.engine.clone(),
            "ui" => installed.ui.clone(),
            _ => installed.updater.clone(),
        };
        let to = three_part(&info.version);
        let newer = match (
            semver::Version::parse(&three_part(&from)),
            semver::Version::parse(&to),
        ) {
            (Ok(a), Ok(b)) => b > a,
            // Compare unparseable versions as strings to allow replacement.
            _ => from != to,
        };
        if newer || (force && from == to) {
            comps.push(ComponentState {
                key: key.to_string(),
                from,
                to,
                size: info.size,
                downloaded: 0,
                status: CompStatus::Pending,
            });
        }
    }

    *core().manifest.lock().unwrap() = Some(m);

    if comps.is_empty() {
        if auto {
            std::process::exit(0);
        }
        {
            let mut s = core().state.lock().unwrap();
            s.phase = Phase::UpToDate;
            s.installed = Some(installed);
            s.components.clear();
            s.error = None;
        }
        emit_state(app);
        return;
    }

    {
        let guard = core().manifest.lock().unwrap();
        let m = guard.as_ref().unwrap();
        let mut s = core().state.lock().unwrap();
        s.phase = Phase::Available;
        s.installed = Some(installed);
        s.tag = m.release.tag.clone();
        s.notes_url = m.release.notes_url.clone();
        s.published_at = m.published_at.clone();
        s.components = comps;
        s.error = None;
    }
    if auto {
        if let Some(win) = app.get_webview_window("main") {
            let _ = win.show();
            let _ = win.set_focus();
        }
    }
    emit_state(app);
    spawn_recording_watch(app);
}

/// Refresh recording state while awaiting user action; engine updates require recording to stop.
fn spawn_recording_watch(app: &AppHandle) {
    let app = app.clone();
    std::thread::spawn(move || loop {
        let phase = core().phase();
        if phase != Phase::Available && phase != Phase::Failed {
            return;
        }
        let rec = engine_rpc::recording();
        let run = engine_rpc::engine_running();
        {
            let mut s = core().state.lock().unwrap();
            if s.recording != rec || s.engine_running != run {
                s.recording = rec;
                s.engine_running = run;
                drop(s);
                emit_state(&app);
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    });
}

fn set_comp(key: &str, mutate: impl FnOnce(&mut ComponentState)) {
    let mut s = core().state.lock().unwrap();
    for c in s.components.iter_mut() {
        if c.key == key {
            mutate(c);
            break;
        }
    }
}

fn run_pipeline(app: &AppHandle) {
    set_phase(app, Phase::Downloading);

    let app_dir = paths::app_dir();
    let staging = paths::staging_dir();
    if staging.exists() {
        if let Err(e) = std::fs::remove_dir_all(&staging) {
            fail(app, &format!("cannot clear staging dir: {e}"));
            return;
        }
    }
    if let Err(e) = std::fs::create_dir_all(&staging) {
        fail(app, &format!("cannot create staging dir: {e}"));
        return;
    }

    let manifest = core().manifest.lock().unwrap().clone();
    let Some(m) = manifest else {
        fail(app, "manifest missing - check for updates again");
        return;
    };

    let keys: Vec<String> = {
        let s = core().state.lock().unwrap();
        s.components.iter().map(|c| c.key.clone()).collect()
    };

    // 1. Download + verify each changed component into staging.
    for key in &keys {
        let Some(info) = m.components.get(key.as_str()) else {
            continue;
        };
        set_comp(key, |c| {
            c.status = CompStatus::Downloading;
            c.downloaded = 0;
        });
        emit_state(app);

        let zip_path = staging.join(format!("{key}.zip"));
        let start = std::time::Instant::now();
        let key_for_cb = key.clone();
        let app_for_cb = app.clone();
        let on_progress = move |bytes: u64| {
            let elapsed = start.elapsed().as_secs_f64();
            let speed = if elapsed > 0.25 {
                (bytes as f64 / elapsed) as u64
            } else {
                0
            };
            {
                let mut s = core().state.lock().unwrap();
                s.speed_bps = speed;
                for c in s.components.iter_mut() {
                    if c.key == key_for_cb {
                        c.downloaded = bytes;
                    }
                }
            }
            emit_state(&app_for_cb);
        };
        let key_for_verify = key.clone();
        let app_for_verify = app.clone();
        let on_verify = move || {
            set_comp(&key_for_verify, |c| c.status = CompStatus::Verifying);
            emit_state(&app_for_verify);
        };

        match download::download(
            key,
            info,
            &zip_path,
            &core().cancel,
            &on_progress,
            &on_verify,
        ) {
            Err(e) if e == download::CANCELLED => {
                cancelled(app, &staging);
                return;
            }
            Err(e) => {
                set_comp(key, |c| c.status = CompStatus::Failed);
                fail(app, &format!("{key}: {e}"));
                return;
            }
            Ok(()) => {}
        }
        set_comp(key, |c| c.status = CompStatus::Ready);
        emit_state(app);
    }

    core().state.lock().unwrap().speed_bps = 0;
    if engine_rpc::recording() {
        fail(app, "a recording is in progress - stop it and retry");
        return;
    }
    if !core().begin_apply() {
        cancelled(app, &staging);
        return;
    }
    emit_state(app);
    let has = |k: &str| keys.iter().any(|x| x == k);
    let engine_processes = match process::EngineProcesses::capture(&app_dir.join("Monolith.exe")) {
        Ok(processes) => processes,
        Err(error) => {
            fail(app, &error);
            return;
        }
    };
    let engine_was_running = engine_processes.running();
    let installed = {
        let s = core().state.lock().unwrap();
        let mut v = s.installed.clone().unwrap_or(versions::Installed {
            engine: "0.0.0".into(),
            ui: "0.0.0".into(),
            updater: "0.0.0".into(),
        });
        for c in &s.components {
            match c.key.as_str() {
                "engine" => v.engine = c.to.clone(),
                "ui" => v.ui = c.to.clone(),
                "updater" => v.updater = c.to.clone(),
                _ => {}
            }
        }
        v
    };
    let tx = match prepare_update(&keys, &staging, &app_dir, &installed) {
        Ok(tx) => tx,
        Err(e) => {
            fail(app, &format!("preparing update: {e}"));
            return;
        }
    };
    if engine_rpc::recording() {
        let error = tx
            .abort()
            .err()
            .map(|e| format!("; recovery: {e}"))
            .unwrap_or_default();
        fail(
            app,
            &format!("a recording started while preparing the update{error}"),
        );
        return;
    }
    if has("ui") && engine_was_running {
        if let Err(e) = engine_rpc::close_ui() {
            let restore = tx
                .abort()
                .err()
                .map(|e| format!("; recovery: {e}"))
                .unwrap_or_default();
            fail(app, &format!("closing the interface: {e}{restore}"));
            return;
        }
    }
    if has("engine") && engine_was_running {
        engine_rpc::request_engine_exit();
        if engine_processes.wait_exit(Duration::from_secs(20)).is_err() {
            let restore = tx
                .abort()
                .err()
                .map(|e| format!("; recovery: {e}"))
                .unwrap_or_default();
            fail(
                app,
                &format!("the engine did not exit in time - close Monolith and retry{restore}"),
            );
            return;
        }
    }
    if let Err(e) = tx.install() {
        let restart = if has("engine")
            && engine_was_running
            && matches!(install::pending(&app_dir), Ok(false))
        {
            restart_engine(&app_dir)
                .err()
                .map(|e| format!("; restarting restored engine: {e}"))
                .unwrap_or_default()
        } else {
            String::new()
        };
        fail(app, &format!("applying update: {e}{restart}"));
        return;
    }
    core().state.lock().unwrap().installed = Some(installed);
    if has("engine") && engine_was_running {
        if let Err(e) = restart_engine(&app_dir) {
            fail(
                app,
                &format!("update applied, but the engine could not restart: {e}"),
            );
            return;
        }
    }

    let _ = std::fs::remove_dir_all(&staging);
    set_phase(app, Phase::Done);
}

fn cancelled(app: &AppHandle, staging: &std::path::Path) {
    let _ = std::fs::remove_dir_all(staging);
    let mut s = core().state.lock().unwrap();
    s.speed_bps = 0;
    for c in &mut s.components {
        c.status = CompStatus::Pending;
        c.downloaded = 0;
    }
    s.phase = Phase::Available;
    drop(s);
    emit_state(app);
}

fn recover_update(app_dir: &std::path::Path) -> Result<(), String> {
    let interrupted = install::pending(app_dir)?;
    let engine_processes = if interrupted {
        Some(process::EngineProcesses::capture(
            &app_dir.join("Monolith.exe"),
        )?)
    } else {
        None
    };
    let running = engine_processes
        .as_ref()
        .is_some_and(|processes| processes.running());
    if running {
        if engine_rpc::recording() {
            return Err("stop the recording before recovering the update".into());
        }
        engine_rpc::close_ui()?;
        engine_rpc::request_engine_exit();
        if engine_processes
            .as_ref()
            .unwrap()
            .wait_exit(Duration::from_secs(20))
            .is_err()
        {
            return Err("close Monolith before recovering the update".into());
        }
    }
    install::recover(app_dir)?;
    if running {
        restart_engine(app_dir)?;
    }
    Ok(())
}

fn prepare_update(
    keys: &[String],
    staging: &std::path::Path,
    app_dir: &std::path::Path,
    installed: &versions::Installed,
) -> Result<install::Transaction, String> {
    let mut files = Vec::new();
    for key in keys {
        let src = staging.join(key);
        archive::extract_zip(&staging.join(format!("{key}.zip")), &src)?;
        let (exe, expected) = match key.as_str() {
            "engine" => ("Monolith.exe", &installed.engine),
            "ui" => ("Monolith.UI.exe", &installed.ui),
            "updater" => ("Updater.exe", &installed.updater),
            _ => return Err("unknown component".into()),
        };
        let payload_version = versions::file_version(&src.join(exe))
            .ok_or("component executable has no version metadata")?;
        if three_part(&payload_version) != *expected {
            return Err(format!(
                "{key} payload version does not match signed metadata"
            ));
        }
        let prefix = if key == "ui" {
            std::path::Path::new("ui")
        } else {
            std::path::Path::new("")
        };
        let component = install::files(&src, prefix)?;
        if key == "updater"
            && (component.len() != 1 || component[0].1 != std::path::Path::new("Updater.exe"))
        {
            return Err("updater payload must contain only Updater.exe".into());
        }
        files.extend(component);
    }
    let metadata = staging.join("components.json");
    versions::stage_components_json(installed, &metadata)?;
    files.push((metadata, "components.json".into()));
    install::Transaction::prepare(app_dir, &files)
}

fn restart_engine(app_dir: &std::path::Path) -> Result<(), String> {
    use std::process::{Command, Stdio};
    let expected = versions::installed().engine;
    let mut child = Command::new(app_dir.join("Monolith.exe"))
        .current_dir(app_dir)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| e.to_string())?;
    process::wait_ready(&mut child, Duration::from_secs(30), || {
        engine_rpc::ready_version(&expected)
    })
}

fn focus_existing_instance() -> bool {
    use windows::core::HSTRING;
    use windows::Win32::Foundation::{GetLastError, ERROR_ALREADY_EXISTS};
    use windows::Win32::System::Threading::CreateMutexW;
    use windows::Win32::UI::WindowsAndMessaging::{FindWindowW, SetForegroundWindow};

    let name = HSTRING::from("Monolith_Updater_SingleInstance");
    unsafe {
        // Intentionally leaked: the mutex must live for the process lifetime.
        let _ = CreateMutexW(None, false, &name);
        if GetLastError() == ERROR_ALREADY_EXISTS {
            let title = HSTRING::from("Monolith Updater");
            if let Ok(hwnd) = FindWindowW(None, &title) {
                let _ = SetForegroundWindow(hwnd);
            }
            return true;
        }
    }
    false
}

fn main() {
    if focus_existing_instance() {
        return;
    }
    let auto = std::env::args().any(|a| a == "--auto");
    if std::env::args().any(|a| a == "--force") {
        FORCE.store(true, Ordering::Relaxed);
    }

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            updater_state,
            updater_start,
            updater_cancel,
            updater_retry
        ])
        .setup(move |app| {
            // Hidden in --auto mode: only shown when an update is actually
            // available (run_check decides).
            WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                .title("Monolith Updater")
                .inner_size(460.0, 560.0)
                .min_inner_size(460.0, 520.0)
                .resizable(false)
                .decorations(false)
                .shadow(true)
                .center()
                .visible(!auto)
                .build()?;

            let handle = app.handle().clone();
            std::thread::spawn(move || run_check(&handle, auto));
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run Monolith Updater");
}
