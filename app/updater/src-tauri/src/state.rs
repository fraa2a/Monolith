use crate::manifest::Manifest;
use crate::versions::Installed;
use serde_json::{json, Value};
use std::sync::atomic::AtomicBool;
use std::sync::Mutex;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    Checking,
    UpToDate,
    Available,
    Downloading,
    Applying,
    Done,
    Failed,
}

impl Phase {
    pub fn as_str(self) -> &'static str {
        match self {
            Phase::Checking => "checking",
            Phase::UpToDate => "upToDate",
            Phase::Available => "available",
            Phase::Downloading => "downloading",
            Phase::Applying => "applying",
            Phase::Done => "done",
            Phase::Failed => "failed",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CompStatus {
    Pending,
    Downloading,
    Verifying,
    Ready,
    Failed,
}

impl CompStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            CompStatus::Pending => "pending",
            CompStatus::Downloading => "downloading",
            CompStatus::Verifying => "verifying",
            CompStatus::Ready => "ready",
            CompStatus::Failed => "failed",
        }
    }
}

#[derive(Clone)]
pub struct ComponentState {
    pub key: String, // "engine" | "ui" | "updater"
    pub from: String,
    pub to: String,
    pub size: u64,
    pub downloaded: u64,
    pub status: CompStatus,
}

pub struct SharedState {
    pub phase: Phase,
    pub tag: String,
    pub notes_url: String,
    pub published_at: String,
    pub components: Vec<ComponentState>,
    pub installed: Option<Installed>,
    pub error: Option<String>,
    pub recording: bool,
    pub engine_running: bool,
    pub speed_bps: u64,
}

/// Shared between the tauri commands (any thread), the check/pipeline
/// threads and the recording watcher. Every mutation is followed by an
/// "update-state" emit; the frontend re-renders from snapshots only.
pub struct Core {
    pub state: Mutex<SharedState>,
    pub manifest: Mutex<Option<Manifest>>,
    pub cancel: AtomicBool,
    pub busy: AtomicBool,
}

impl Core {
    pub fn new() -> Self {
        Core {
            state: Mutex::new(SharedState {
                phase: Phase::Checking,
                tag: String::new(),
                notes_url: String::new(),
                published_at: String::new(),
                components: Vec::new(),
                installed: None,
                error: None,
                recording: false,
                engine_running: false,
                speed_bps: 0,
            }),
            manifest: Mutex::new(None),
            cancel: AtomicBool::new(false),
            busy: AtomicBool::new(false),
        }
    }

    pub fn snapshot(&self) -> Value {
        let s = self.state.lock().unwrap();
        json!({
            "phase": s.phase.as_str(),
            "tag": s.tag,
            "notesUrl": s.notes_url,
            "publishedAt": s.published_at,
            "error": s.error,
            "recording": s.recording,
            "engineRunning": s.engine_running,
            "speedBps": s.speed_bps,
            "installed": s.installed,
            "components": s.components
                .iter()
                .map(|c| {
                    json!({
                        "key": c.key,
                        "from": c.from,
                        "to": c.to,
                        "size": c.size,
                        "downloaded": c.downloaded,
                        "status": c.status.as_str(),
                    })
                })
                .collect::<Vec<_>>(),
        })
    }

    pub fn phase(&self) -> Phase {
        self.state.lock().unwrap().phase
    }
}
