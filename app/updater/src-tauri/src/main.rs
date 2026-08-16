// Monolith Updater (Updater.exe) — component self-updater.
//
// Owns the whole update flow: fetch update-manifest.json from the releases
// page, compare per-component versions (engine / ui / updater are versioned
// independently — a UI-only release never touches the engine), download ONLY
// the components whose version changed, verify sha256 + Ed25519 (same key
// pair WinSparkle used), stage and swap files on disk (rename-to-.old dance),
// and relaunch the engine. The recorder spawns this exe with --auto at
// startup (silent unless an update exists) and from the tray "Check for
// Updates…" (window immediately).
//
// The Preact frontend is a pure projection of "update-state" events plus a
// few commands (start / cancel / retry). Layout on disk (installed):
//   {app}\Monolith.exe  {app}\Updater.exe  {app}\ui\Monolith.UI.exe

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod apply;
mod download;
mod engine_rpc;
mod http;
mod manifest;
mod paths;
mod state;
mod versions;

use state::{CompStatus, ComponentState, Core, Phase};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

static CORE: OnceLock<Core> = OnceLock::new();
// --force: reinstall even when versions match (testing / repair path).
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

// ── Commands (frontend) ─────────────────────────────────────────────────────

#[tauri::command]
fn updater_state() -> serde_json::Value {
    core().snapshot()
}

#[tauri::command]
fn updater_start(app: AppHandle) {
    if core().phase() != Phase::Available || core().busy.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(move || {
        run_pipeline(&app);
        core().busy.store(false, Ordering::SeqCst);
    });
}

#[tauri::command]
fn updater_cancel() {
    core().cancel.store(true, Ordering::Relaxed);
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

// ── Check ───────────────────────────────────────────────────────────────────

/// Truncate to the first three numeric groups so semver can parse 4-part
/// Windows FileVersions ("1.2.3.4" → "1.2.3").
fn three_part(v: &str) -> String {
    v.split('.').take(3).collect::<Vec<_>>().join(".")
}

fn run_check(app: &AppHandle, auto: bool) {
    apply::sweep_old(&paths::app_dir());
    *core().manifest.lock().unwrap() = None;
    set_phase(app, Phase::Checking);

    let m = match manifest::fetch() {
        Ok(m) => m,
        Err(e) => {
            // A failed background check must never nag the user.
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
        let Some(info) = m.components.get(key) else { continue };
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
            // Unparseable version: fall back to inequality so the component
            // still heals itself through a reinstall.
            _ => from != to,
        };
        if force || newer {
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

/// While the user decides (Available), mirror the engine's recording state
/// so Update-now can be disabled during a recording (the engine restarts to
/// apply engine updates — that would kill an active clip).
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

// ── Download + apply pipeline ───────────────────────────────────────────────

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
    core().cancel.store(false, Ordering::Relaxed);
    set_phase(app, Phase::Downloading);

    let app_dir = paths::app_dir();
    let staging = paths::staging_dir();
    let _ = std::fs::remove_dir_all(&staging);
    if let Err(e) = std::fs::create_dir_all(&staging) {
        fail(app, &format!("cannot create staging dir: {e}"));
        return;
    }

    let manifest = core().manifest.lock().unwrap().clone();
    let Some(m) = manifest else {
        fail(app, "manifest missing — check for updates again");
        return;
    };

    let keys: Vec<String> = {
        let s = core().state.lock().unwrap();
        s.components.iter().map(|c| c.key.clone()).collect()
    };

    // 1. Download + verify each changed component into staging.
    for key in &keys {
        let Some(info) = m.components.get(key.as_str()) else { continue };
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
            &info.url,
            &zip_path,
            info.size,
            &info.sha256,
            &info.ed_signature,
            &core().cancel,
            &on_progress,
            &on_verify,
        ) {
            Err(e) if e == download::CANCELLED => {
                let _ = std::fs::remove_dir_all(&staging);
                {
                    let mut s = core().state.lock().unwrap();
                    s.speed_bps = 0;
                    for c in s.components.iter_mut() {
                        c.status = CompStatus::Pending;
                        c.downloaded = 0;
                    }
                    s.phase = Phase::Available;
                }
                emit_state(app);
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

    // 2. Apply. UI first (its process must close), engine next (its process
    //    must exit), then relaunch the engine, then self-update last.
    core().state.lock().unwrap().speed_bps = 0;
    if engine_rpc::recording() {
        fail(app, "a recording is in progress — stop it and retry");
        return;
    }
    set_phase(app, Phase::Applying);

    let has = |k: &str| keys.iter().any(|x| x == k);
    let engine_was_running = engine_rpc::engine_running();

    if has("ui") {
        if engine_was_running {
            if let Err(e) = engine_rpc::close_ui() {
                fail(app, &format!("closing the interface failed: {e}"));
                return;
            }
        }
        let src = staging.join("ui");
        if let Err(e) = apply::extract_zip(&staging.join("ui.zip"), &src)
            .and_then(|_| apply::place_tree(&src, &app_dir.join("ui")))
        {
            fail(app, &format!("applying interface: {e}"));
            return;
        }
    }

    if has("engine") {
        if engine_was_running {
            engine_rpc::request_engine_exit();
            if !engine_rpc::wait_engine_exit(Duration::from_secs(20)) {
                fail(app, "the engine did not exit in time — close Monolith and retry");
                return;
            }
        }
        let src = staging.join("engine");
        if let Err(e) = apply::extract_zip(&staging.join("engine.zip"), &src)
            .and_then(|_| apply::place_tree(&src, &app_dir))
        {
            fail(app, &format!("applying engine: {e}"));
            return;
        }
    }

    // Relaunch the engine before self-swapping so the new engine is already
    // running even if this process dies right after the swap.
    if has("engine") && engine_was_running {
        use std::process::{Command, Stdio};
        let _ = Command::new(app_dir.join("Monolith.exe"))
            .current_dir(&app_dir)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn();
    }

    // Record the applied versions (exe resources are the live source; this
    // file is the fallback) before anything else can go wrong.
    {
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
        versions::write_components_json(&v);
    }

    if has("updater") {
        let src = staging.join("updater");
        if let Err(e) = apply::extract_zip(&staging.join("updater.zip"), &src)
            .and_then(|_| apply::self_swap(&src.join("Updater.exe")))
        {
            fail(app, &format!("applying updater: {e}"));
            return;
        }
    }

    let _ = std::fs::remove_dir_all(&staging);
    set_phase(app, Phase::Done);
}

// ── Single instance ─────────────────────────────────────────────────────────

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
