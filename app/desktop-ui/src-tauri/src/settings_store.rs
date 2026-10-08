use crate::paths;
use rusqlite::{params, Connection, OpenFlags};
use serde_json::{Map, Value};
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::SystemTime;

pub fn settings_db_path() -> PathBuf {
    paths::monolith_data_dir().join("settings.db")
}

pub fn read_config() -> Option<Value> {
    let path = settings_db_path();
    if !path.is_file() {
        return None;
    }

    let conn = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    let mut stmt = conn.prepare("SELECT key, value FROM settings").ok()?;
    let rows = stmt
        .query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })
        .ok()?;

    let mut config = Map::new();
    for row in rows.flatten() {
        if let Ok(value) = serde_json::from_str::<Value>(&row.1) {
            config.insert(row.0, value);
        }
    }

    if config.is_empty() {
        return None;
    }
    Some(Value::Object(config))
}

pub fn write_config(config: &Value) -> rusqlite::Result<()> {
    let Some(sections) = config.as_object() else {
        return Ok(());
    };

    let path = settings_db_path();
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }

    let mut conn = Connection::open(path)?;
    conn.busy_timeout(std::time::Duration::from_millis(4000))?;
    conn.execute(
        "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        [],
    )?;

    let tx = conn.transaction()?;
    tx.execute("DELETE FROM settings", [])?;
    {
        let mut stmt = tx.prepare("INSERT INTO settings (key, value) VALUES (?1, ?2)")?;
        for (key, value) in sections {
            stmt.execute(params![key, value.to_string()])?;
        }
    }
    tx.commit()?;
    if let Ok(mut cache) = DIRS_CACHE.lock() {
        *cache = None;
    }
    Ok(())
}

pub fn read_runtime_status() -> Value {
    let path = paths::monolith_data_dir().join("runtime-status.json");
    fs::read_to_string(path)
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .unwrap_or_else(|| Value::Object(Map::new()))
}

// Fingerprint both SQLite files because WAL writes may leave the database timestamp unchanged.
struct DirCache {
    path: PathBuf,
    stamps: [(Option<SystemTime>, u64); 2],
    dirs: paths::OutputDirs,
}

static DIRS_CACHE: Mutex<Option<DirCache>> = Mutex::new(None);

pub fn output_dirs() -> paths::OutputDirs {
    let path = settings_db_path();
    let wal = path.with_file_name("settings.db-wal");
    let stamps = [path.as_path(), wal.as_path()].map(|p| {
        let meta = fs::metadata(p).ok();
        (
            meta.as_ref().and_then(|m| m.modified().ok()),
            meta.map(|m| m.len()).unwrap_or(0),
        )
    });
    let mut cache = DIRS_CACHE.lock().unwrap_or_else(|err| err.into_inner());
    if let Some(hit) = cache.as_ref() {
        if hit.path == path && hit.stamps == stamps {
            return hit.dirs.clone();
        }
    }
    let dirs = compute_output_dirs(paths::default_output_dirs());
    *cache = Some(DirCache {
        path,
        stamps,
        dirs: dirs.clone(),
    });
    dirs
}

fn compute_output_dirs(defaults: paths::OutputDirs) -> paths::OutputDirs {
    let Some(config) = read_config() else {
        return defaults;
    };
    let Some(output) = config.get("output").and_then(Value::as_object) else {
        return defaults;
    };

    let clips = output
        .get("clips_directory")
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or(defaults.clips);

    let recs = output
        .get("recordings_directory")
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or(defaults.recs);

    paths::OutputDirs { clips, recs }
}
