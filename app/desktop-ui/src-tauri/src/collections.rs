// Collections ("albums"): user-curated groupings of clips that span both
// sources (replay + manual). Stored in a single global collections.db under
// %LocalAppData%\Monolith — deliberately not per-catalog, because a collection
// mixes clips from clips.db and recs.db. Rows referencing clips that have
// since been deleted are pruned on read.

use crate::clip_catalog::{self, ClipSource};
use crate::paths;
use rusqlite::{params, Connection};
use serde::Serialize;

const DDL: &str = "
CREATE TABLE IF NOT EXISTS collections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    color TEXT NOT NULL DEFAULT '',
    created_at_utc TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS collection_clips (
    collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
    source TEXT NOT NULL,
    clip_id INTEGER NOT NULL,
    added_at_utc TEXT NOT NULL,
    PRIMARY KEY (collection_id, source, clip_id)
);";

fn open() -> Result<Connection, String> {
    let dir = paths::monolith_data_dir();
    std::fs::create_dir_all(&dir).map_err(|err| err.to_string())?;
    let conn = Connection::open(dir.join("collections.db")).map_err(|err| err.to_string())?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    conn.execute_batch("PRAGMA foreign_keys = ON;").map_err(|err| err.to_string())?;
    conn.execute_batch(DDL).map_err(|err| err.to_string())?;
    Ok(conn)
}

// UTC ISO-8601 in the same format the engine's storage layer writes
// ("2026-08-11T12:34:56Z") so SQLite string ordering stays consistent.
fn now_iso8601_utc() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let (hh, mm, ss) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    // civil_from_days (Hinnant): days since 1970-01-01 -> (y, m, d).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!("{y:04}-{m:02}-{d:02}T{hh:02}:{mm:02}:{ss:02}Z")
}

#[derive(Serialize)]
pub struct CollectionSummary {
    pub id: i64,
    pub name: String,
    pub color: String,
    pub created_at_utc: String,
    pub clip_count: i64,
}

pub fn list_collections() -> Result<Vec<CollectionSummary>, String> {
    let conn = open()?;
    let mut stmt = conn
        .prepare(
            "SELECT c.id, c.name, c.color, c.created_at_utc,
                    (SELECT COUNT(*) FROM collection_clips cc
                      WHERE cc.collection_id = c.id) AS clip_count
             FROM collections c ORDER BY c.created_at_utc, c.id",
        )
        .map_err(|err| err.to_string())?;
    let rows = stmt
        .query_map([], |row| {
            Ok(CollectionSummary {
                id: row.get(0)?,
                name: row.get(1)?,
                color: row.get(2)?,
                created_at_utc: row.get(3)?,
                clip_count: row.get(4)?,
            })
        })
        .map_err(|err| err.to_string())?;
    rows.collect::<Result<Vec<_>, _>>().map_err(|err| err.to_string())
}

pub fn create_collection(name: &str, color: &str) -> Result<i64, String> {
    if name.trim().is_empty() {
        return Err("empty collection name".to_string());
    }
    let conn = open()?;
    conn.execute(
        "INSERT INTO collections (name, color, created_at_utc) VALUES (?1, ?2, ?3)",
        params![name.trim(), color, now_iso8601_utc()],
    )
    .map_err(|err| err.to_string())?;
    Ok(conn.last_insert_rowid())
}

pub fn rename_collection(id: i64, name: &str) -> Result<(), String> {
    if name.trim().is_empty() {
        return Err("empty collection name".to_string());
    }
    let conn = open()?;
    conn.execute("UPDATE collections SET name = ?1 WHERE id = ?2", params![name.trim(), id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("collection not found".to_string());
    }
    Ok(())
}

pub fn delete_collection(id: i64) -> Result<(), String> {
    let conn = open()?;
    // collection_clips rows go via the FK ON DELETE CASCADE now that the
    // pragma is enabled in open(); delete clips first anyway so a failed
    // cascade (older DB, pragma didn't stick) cannot leave orphans.
    conn.execute("DELETE FROM collection_clips WHERE collection_id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    conn.execute("DELETE FROM collections WHERE id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("collection not found".to_string());
    }
    Ok(())
}

pub fn add_clip_to_collection(collection_id: i64, source: ClipSource, clip_id: i64) -> Result<(), String> {
    let conn = open()?;
    // Only accept clips that exist in the catalog right now.
    if clip_catalog::clip_by_id(source, clip_id).is_none() {
        return Err("clip not found".to_string());
    }
    let exists: bool = conn
        .query_row(
            "SELECT 1 FROM collections WHERE id = ?1",
            params![collection_id],
            |_| Ok(()),
        )
        .is_ok();
    if !exists {
        return Err("collection not found".to_string());
    }
    conn.execute(
        "INSERT OR IGNORE INTO collection_clips (collection_id, source, clip_id, added_at_utc)
         VALUES (?1, ?2, ?3, ?4)",
        params![collection_id, source.as_str(), clip_id, now_iso8601_utc()],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

pub fn remove_clip_from_collection(collection_id: i64, source: ClipSource, clip_id: i64) -> Result<(), String> {
    let conn = open()?;
    conn.execute(
        "DELETE FROM collection_clips WHERE collection_id = ?1 AND source = ?2 AND clip_id = ?3",
        params![collection_id, source.as_str(), clip_id],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

// Clips in a collection, most recently added first. Stale rows (the clip was
// deleted from its catalog, or its video file is gone) are pruned here so the
// collection never shows ghosts.
pub fn collection_clips(collection_id: i64) -> Result<Vec<clip_catalog::Clip>, String> {
    let conn = open()?;
    let mut stmt = conn
        .prepare(
            "SELECT source, clip_id FROM collection_clips
             WHERE collection_id = ?1 ORDER BY datetime(added_at_utc) DESC, clip_id DESC",
        )
        .map_err(|err| err.to_string())?;
    let rows = stmt
        .query_map(params![collection_id], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
        })
        .map_err(|err| err.to_string())?;

    let mut clips = Vec::new();
    let mut stale = Vec::new();
    for entry in rows {
        let (source_str, clip_id) = entry.map_err(|err| err.to_string())?;
        let Some(source) = ClipSource::parse(&source_str) else {
            stale.push((collection_id, source_str, clip_id));
            continue;
        };
        match clip_catalog::clip_by_id(source, clip_id) {
            Some(clip) => clips.push(clip),
            None => stale.push((collection_id, source_str, clip_id)),
        }
    }
    for (cid, source, clip_id) in stale {
        let _ = conn.execute(
            "DELETE FROM collection_clips WHERE collection_id = ?1 AND source = ?2 AND clip_id = ?3",
            params![cid, source, clip_id],
        );
    }
    Ok(clips)
}
