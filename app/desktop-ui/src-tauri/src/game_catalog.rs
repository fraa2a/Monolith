use crate::paths;
use rusqlite::{params, Connection, OpenFlags};
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Serialize)]
pub struct CatalogEntry {
    pub process_name_lower: String,
    pub display_name: String,
    pub discord_app_id: Option<String>,
    pub icon_url: Option<String>,
}

fn catalog_path() -> PathBuf {
    paths::monolith_data_dir().join("game_catalog.db")
}

fn open(readonly: bool) -> Option<Connection> {
    let path = catalog_path();
    if !path.is_file() {
        return None;
    }

    let flags = if readonly {
        OpenFlags::SQLITE_OPEN_READ_ONLY
    } else {
        OpenFlags::SQLITE_OPEN_READ_WRITE
    };
    let conn = Connection::open_with_flags(path, flags).ok()?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    Some(conn)
}

pub fn catalog_map() -> BTreeMap<String, CatalogEntry> {
    let Some(conn) = open(true) else {
        return BTreeMap::new();
    };
    let Ok(mut stmt) = conn.prepare(
        "SELECT process_name_lower, display_name, discord_app_id, icon_url FROM game_catalog",
    ) else {
        return BTreeMap::new();
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok(CatalogEntry {
            process_name_lower: row.get(0)?,
            display_name: row.get(1)?,
            discord_app_id: row.get(2)?,
            icon_url: row.get(3)?,
        })
    }) else {
        return BTreeMap::new();
    };

    rows.flatten()
        .map(|entry| (entry.process_name_lower.clone(), entry))
        .collect()
}

pub fn resolve_icon(process_name: &str) -> Option<String> {
    let key = process_name.to_lowercase();
    let conn = open(true)?;
    let entry = conn
        .query_row(
            "SELECT icon_url FROM game_catalog WHERE process_name_lower = ?1",
            params![key],
            |row| row.get::<_, Option<String>>(0),
        )
        .ok()
        .flatten();
    entry
}
