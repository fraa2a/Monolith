use crate::paths;
use rusqlite::{params, Connection, OpenFlags};
use serde_json::Value;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

// Discord is only ever used to enrich the display (name/icon/cover) of a game
// the C++ recorder's local heuristic already detected — never to decide which
// process is active. All reads used by the clip grid go through
// resolve_artwork_cached (below), which never makes a network call; artwork
// is populated/refreshed only by the scheduled background job in main.rs
// (see refresh_stale) and by the live "currently playing" titlebar lookup.

const DISCORD_FETCH_TIMEOUT: Duration = Duration::from_secs(3);

fn now_unix() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[derive(Clone, Serialize)]
pub struct CatalogEntry {
    pub process_name_lower: String,
    pub display_name: String,
    pub discord_app_id: Option<String>,
    pub icon_url: Option<String>,
    pub cover_url: Option<String>,
    pub last_updated: i64,
}

fn catalog_path() -> PathBuf {
    paths::monolith_data_dir().join("game_catalog.db")
}

fn open(readonly: bool) -> Option<Connection> {
    let path = catalog_path();
    if readonly && !path.is_file() {
        return None;
    }
    if !readonly {
        let _ = std::fs::create_dir_all(path.parent()?);
    }

    let flags = if readonly {
        OpenFlags::SQLITE_OPEN_READ_ONLY
    } else {
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE
    };
    let conn = Connection::open_with_flags(path, flags).ok()?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    Some(conn)
}

fn ensure_schema(conn: &Connection) {
    let _ = conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS game_catalog (
            process_name_lower TEXT PRIMARY KEY,
            display_name TEXT NOT NULL DEFAULT '',
            discord_app_id TEXT,
            icon_url TEXT,
            cover_url TEXT
        );",
    );
    for (name, ty) in [
        ("display_name", "TEXT NOT NULL DEFAULT ''"),
        ("discord_app_id", "TEXT"),
        ("icon_url", "TEXT"),
        ("cover_url", "TEXT"),
    ] {
        if !has_column(conn, "game_catalog", name) {
            let _ = conn.execute(&format!("ALTER TABLE game_catalog ADD COLUMN {name} {ty}"), []);
        }
    }
}

fn has_column(conn: &Connection, table: &str, column: &str) -> bool {
    let Ok(mut stmt) = conn.prepare(&format!("PRAGMA table_info({table})")) else {
        return false;
    };
    let Ok(rows) = stmt.query_map([], |row| row.get::<_, String>(1)) else {
        return false;
    };
    let found = rows.flatten().any(|name| name == column);
    found
}

fn cover_expr(conn: &Connection) -> &'static str {
    if has_column(conn, "game_catalog", "cover_url") {
        "cover_url"
    } else if has_column(conn, "game_catalog", "banner_url") {
        "banner_url"
    } else {
        "NULL"
    }
}

fn row_to_entry(row: &rusqlite::Row<'_>) -> rusqlite::Result<CatalogEntry> {
    Ok(CatalogEntry {
        process_name_lower: row.get(0)?,
        display_name: row.get(1)?,
        discord_app_id: row.get(2)?,
        icon_url: row.get(3)?,
        cover_url: row.get(4)?,
        last_updated: row.get::<_, Option<i64>>(5)?.unwrap_or(0),
    })
}

fn select_expr(conn: &Connection) -> String {
    let last_updated_expr = if has_column(conn, "game_catalog", "last_updated") {
        "last_updated"
    } else {
        "0"
    };
    format!(
        "process_name_lower, display_name, discord_app_id, icon_url, {}, {last_updated_expr}",
        cover_expr(conn),
    )
}

pub fn catalog_map() -> BTreeMap<String, CatalogEntry> {
    let Some(conn) = open(true) else {
        return BTreeMap::new();
    };
    let sql = format!("SELECT {} FROM game_catalog", select_expr(&conn));
    let Ok(mut stmt) = conn.prepare(&sql) else {
        return BTreeMap::new();
    };
    let Ok(rows) = stmt.query_map([], row_to_entry) else {
        return BTreeMap::new();
    };

    rows.flatten()
        .map(|entry| (entry.process_name_lower.clone(), entry))
        .collect()
}

pub fn entry_by_process(process_name: &str) -> Option<CatalogEntry> {
    let key = process_name.to_lowercase();
    let conn = open(true)?;
    let sql = format!(
        "SELECT {} FROM game_catalog WHERE process_name_lower = ?1",
        select_expr(&conn),
    );
    conn.query_row(&sql, params![key], row_to_entry).ok()
}

pub fn entry_by_app_id(app_id: &str) -> Option<CatalogEntry> {
    let conn = open(true)?;
    let sql = format!(
        "SELECT {} FROM game_catalog WHERE discord_app_id = ?1",
        select_expr(&conn),
    );
    conn.query_row(&sql, params![app_id], row_to_entry).ok()
}

pub fn resolve_icon(process_name: &str) -> Option<String> {
    entry_by_process(process_name).and_then(|entry| entry.icon_url)
}

// Local-cache-only lookup: never makes a network call. Used by the clip grid
// display path (list_clips), which must not block on/wait for Discord.
pub fn resolve_artwork_cached(app_id: Option<&str>, process_name: Option<&str>) -> CatalogEntry {
    if let Some(app_id) = app_id.filter(|id| !id.is_empty()) {
        if let Some(entry) = entry_by_app_id(app_id) {
            return entry;
        }
    }
    if let Some(process_name) = process_name.filter(|name| !name.is_empty()) {
        if let Some(entry) = entry_by_process(process_name) {
            return entry;
        }
    }
    CatalogEntry {
        process_name_lower: process_name.unwrap_or_default().to_lowercase(),
        display_name: String::new(),
        discord_app_id: app_id.map(str::to_string),
        icon_url: None,
        cover_url: None,
        last_updated: 0,
    }
}

// Network-capable lookup: fetches from Discord (with an explicit timeout) when
// the cache is missing or incomplete. Used only by explicit/live contexts
// (the titlebar's "currently playing" indicator), never by the clip grid.
pub fn resolve_artwork(app_id: Option<&str>, process_name: Option<&str>) -> CatalogEntry {
    if let Some(app_id) = app_id.filter(|id| !id.is_empty()) {
        if let Some(entry) = entry_by_app_id(app_id) {
            if entry.icon_url.is_none() || entry.cover_url.is_none() {
                if let Some(fresh) = fetch_and_cache_discord_app(app_id, process_name) {
                    return fresh;
                }
            }
            return entry;
        }
        if let Some(entry) = fetch_and_cache_discord_app(app_id, process_name) {
            return entry;
        }
    }
    if let Some(process_name) = process_name.filter(|name| !name.is_empty()) {
        if let Some(entry) = entry_by_process(process_name) {
            if entry.icon_url.is_none() || entry.cover_url.is_none() {
                if let Some(app_id) = entry.discord_app_id.as_deref() {
                    if let Some(fresh) = fetch_and_cache_discord_app(app_id, Some(process_name)) {
                        return fresh;
                    }
                }
            }
            return entry;
        }
    }
    CatalogEntry {
        process_name_lower: process_name.unwrap_or_default().to_lowercase(),
        display_name: String::new(),
        discord_app_id: app_id.map(str::to_string),
        icon_url: None,
        cover_url: None,
    }
}

fn discord_cdn_icon(app_id: &str, hash: &str) -> String {
    format!("https://cdn.discordapp.com/app-icons/{app_id}/{hash}.png?size=64")
}

fn discord_cdn_cover(app_id: &str, hash: &str) -> String {
    format!("https://cdn.discordapp.com/app-icons/{app_id}/{hash}.png?size=512")
}

fn fetch_and_cache_discord_app(app_id: &str, process_name: Option<&str>) -> Option<CatalogEntry> {
    let url = format!("https://discord.com/api/v10/applications/{app_id}/rpc");
    let response = ureq::get(&url)
        .set("User-Agent", "Monolith/1.0")
        .timeout(DISCORD_FETCH_TIMEOUT)
        .call()
        .ok()?;
    let value: Value = response.into_json().ok()?;
    let name = value
        .get("name")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string();
    let icon_url = value
        .get("icon")
        .and_then(Value::as_str)
        .filter(|hash| !hash.is_empty())
        .map(|hash| discord_cdn_icon(app_id, hash));
    let cover_url = value
        .get("cover_image")
        .and_then(Value::as_str)
        .filter(|hash| !hash.is_empty())
        .map(|hash| discord_cdn_cover(app_id, hash));
    if icon_url.is_none() && cover_url.is_none() && name.is_empty() {
        return None;
    }

    let key = process_name
        .filter(|name| !name.is_empty())
        .map(str::to_lowercase)
        .unwrap_or_else(|| format!("discord:{app_id}"));
    let last_updated = now_unix();
    if let Some(conn) = open(false) {
        ensure_schema(&conn);
        let _ = conn.execute(
            "INSERT INTO game_catalog
                (process_name_lower, display_name, discord_app_id, icon_url, cover_url, last_updated)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)
             ON CONFLICT(process_name_lower) DO UPDATE SET
                display_name=excluded.display_name,
                discord_app_id=excluded.discord_app_id,
                icon_url=excluded.icon_url,
                cover_url=excluded.cover_url,
                last_updated=excluded.last_updated",
            params![&key, &name, app_id, &icon_url, &cover_url, last_updated],
        );
    }

    Some(CatalogEntry {
        process_name_lower: key,
        display_name: name,
        discord_app_id: Some(app_id.to_string()),
        icon_url,
        cover_url,
        last_updated,
    })
}

// Background refresh: re-fetches any catalog row with a known discord_app_id
// whose last_updated is older than max_age (or never set), so the clip grid's
// cache-only reads (resolve_artwork_cached) stay reasonably fresh without ever
// blocking a display path on the network. Call from a background thread.
pub fn refresh_stale(max_age: Duration) {
    let Some(conn) = open(true) else { return };
    ensure_schema(&conn);
    let cutoff = now_unix() - max_age.as_secs() as i64;
    let sql = format!(
        "SELECT {} FROM game_catalog WHERE discord_app_id IS NOT NULL AND last_updated < ?1",
        select_expr(&conn),
    );
    let Ok(mut stmt) = conn.prepare(&sql) else { return };
    let Ok(rows) = stmt.query_map(params![cutoff], row_to_entry) else { return };
    let stale: Vec<CatalogEntry> = rows.flatten().collect();
    drop(stmt);
    drop(conn);

    for entry in stale {
        let Some(app_id) = entry.discord_app_id.as_deref() else { continue };
        let process_name = if entry.process_name_lower.starts_with("discord:") {
            None
        } else {
            Some(entry.process_name_lower.as_str())
        };
        fetch_and_cache_discord_app(app_id, process_name);
    }
}
