use crate::paths;
use rusqlite::{params, Connection, OpenFlags};
use serde_json::Value;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Clone, Serialize)]
pub struct CatalogEntry {
    pub process_name_lower: String,
    pub display_name: String,
    pub discord_app_id: Option<String>,
    pub icon_url: Option<String>,
    pub cover_url: Option<String>,
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
    })
}

pub fn catalog_map() -> BTreeMap<String, CatalogEntry> {
    let Some(conn) = open(true) else {
        return BTreeMap::new();
    };
    let sql = format!(
        "SELECT process_name_lower, display_name, discord_app_id, icon_url, {} FROM game_catalog",
        cover_expr(&conn),
    );
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
        "SELECT process_name_lower, display_name, discord_app_id, icon_url, {} FROM game_catalog WHERE process_name_lower = ?1",
        cover_expr(&conn),
    );
    conn.query_row(&sql, params![key], row_to_entry).ok()
}

pub fn entry_by_app_id(app_id: &str) -> Option<CatalogEntry> {
    let conn = open(true)?;
    let sql = format!(
        "SELECT process_name_lower, display_name, discord_app_id, icon_url, {} FROM game_catalog WHERE discord_app_id = ?1",
        cover_expr(&conn),
    );
    conn.query_row(&sql, params![app_id], row_to_entry).ok()
}

pub fn resolve_icon(process_name: &str) -> Option<String> {
    entry_by_process(process_name).and_then(|entry| entry.icon_url)
}

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
    if let Some(conn) = open(false) {
        ensure_schema(&conn);
        let _ = conn.execute(
            "INSERT INTO game_catalog
                (process_name_lower, display_name, discord_app_id, icon_url, cover_url)
             VALUES (?1, ?2, ?3, ?4, ?5)
             ON CONFLICT(process_name_lower) DO UPDATE SET
                display_name=excluded.display_name,
                discord_app_id=excluded.discord_app_id,
                icon_url=excluded.icon_url,
                cover_url=excluded.cover_url",
            params![&key, &name, app_id, &icon_url, &cover_url],
        );
    }

    Some(CatalogEntry {
        process_name_lower: key,
        display_name: name,
        discord_app_id: Some(app_id.to_string()),
        icon_url,
        cover_url,
    })
}
