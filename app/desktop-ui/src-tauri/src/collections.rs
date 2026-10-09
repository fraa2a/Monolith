// Catalog and clip identities survive row ID reuse and folder changes.

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
    catalog_uid TEXT NOT NULL,
    clip_uid TEXT NOT NULL,
    PRIMARY KEY (collection_id, source, catalog_uid, clip_uid)
);
CREATE INDEX IF NOT EXISTS collection_member_identity ON collection_clips(source,catalog_uid,clip_uid);";

fn open() -> Result<Connection, String> {
    let dir = paths::monolith_data_dir();
    std::fs::create_dir_all(&dir).map_err(|err| err.to_string())?;
    let conn = Connection::open(dir.join("collections.db")).map_err(|err| err.to_string())?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    conn.execute_batch("PRAGMA foreign_keys = ON;")
        .map_err(|err| err.to_string())?;
    let legacy: bool = conn
        .query_row(
            "SELECT COUNT(*) FROM pragma_table_info('collection_clips') WHERE name='catalog_uid'",
            [],
            |row| row.get::<_, i64>(0),
        )
        .map_err(|err| err.to_string())?
        == 0;
    let exists: bool = conn
        .query_row(
            "SELECT COUNT(*) FROM sqlite_master WHERE name='collection_clips'",
            [],
            |row| row.get::<_, i64>(0),
        )
        .map_err(|err| err.to_string())?
        != 0;
    if exists && legacy {
        conn.execute_batch("ALTER TABLE collection_clips RENAME TO collection_clips_legacy;")
            .map_err(|err| err.to_string())?;
    }
    conn.execute_batch("CREATE TABLE IF NOT EXISTS collection_clips_legacy(collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE, source TEXT NOT NULL, clip_id INTEGER NOT NULL, added_at_utc TEXT NOT NULL, PRIMARY KEY(collection_id,source,clip_id));").map_err(|err| err.to_string())?;
    conn.execute_batch(DDL).map_err(|err| err.to_string())?;
    Ok(conn)
}

// Use UTC ISO-8601 to match the engine catalog timestamps.
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
    pub unresolved_count: i64,
}

pub fn list_collections() -> Result<Vec<CollectionSummary>, String> {
    let conn = open()?;
    let mut stmt = conn
        .prepare(
            "SELECT c.id, c.name, c.color, c.created_at_utc,
                    (SELECT COUNT(*) FROM collection_clips cc
                      WHERE cc.collection_id = c.id) +
                    (SELECT COUNT(*) FROM collection_clips_legacy l WHERE l.collection_id=c.id) AS clip_count,
                    (SELECT COUNT(*) FROM collection_clips_legacy l WHERE l.collection_id=c.id)
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
                unresolved_count: row.get(5)?,
            })
        })
        .map_err(|err| err.to_string())?;
    rows.collect::<Result<Vec<_>, _>>()
        .map_err(|err| err.to_string())
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
    conn.execute(
        "UPDATE collections SET name = ?1 WHERE id = ?2",
        params![name.trim(), id],
    )
    .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("collection not found".to_string());
    }
    Ok(())
}

pub fn delete_collection(id: i64) -> Result<(), String> {
    let conn = open()?;
    conn.execute("DELETE FROM collections WHERE id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("collection not found".to_string());
    }
    Ok(())
}

pub fn add_clip_to_collection(
    collection_id: i64,
    source: ClipSource,
    clip_id: i64,
) -> Result<(), String> {
    let identity = clip_catalog::identity(source, clip_id)?;
    add_clip_with_identity(collection_id, source, clip_id, identity)
}

pub fn add_clip_with_identity(
    collection_id: i64,
    source: ClipSource,
    clip_id: i64,
    identity: clip_catalog::ClipIdentity,
) -> Result<(), String> {
    let current = clip_catalog::identity(source, clip_id)?;
    if current.catalog_uid != identity.catalog_uid || current.clip_uid != identity.clip_uid {
        return Err("clip identity changed; refresh the library".into());
    }
    let conn = open()?;
    let exists: bool = conn
        .query_row(
            "SELECT 1 FROM collections WHERE id=?1",
            params![collection_id],
            |_| Ok(()),
        )
        .is_ok();
    if !exists {
        return Err("collection not found".into());
    }
    conn.execute("INSERT OR IGNORE INTO collection_clips(collection_id,source,clip_id,added_at_utc,catalog_uid,clip_uid) VALUES(?1,?2,?3,?4,?5,?6)",params![collection_id,source.as_str(),clip_id,now_iso8601_utc(),identity.catalog_uid,identity.clip_uid]).map_err(|err|err.to_string())?;
    conn.execute(
        "DELETE FROM collection_clips_legacy WHERE collection_id=?1 AND source=?2 AND clip_id=?3",
        params![collection_id, source.as_str(), clip_id],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

pub fn remove_clip_from_collection(
    collection_id: i64,
    source: ClipSource,
    clip_id: i64,
) -> Result<(), String> {
    let identity = clip_catalog::identity(source, clip_id)?;
    remove_clip_with_identity(collection_id, source, identity)
}

pub fn remove_clip_with_identity(
    collection_id: i64,
    source: ClipSource,
    identity: clip_catalog::ClipIdentity,
) -> Result<(), String> {
    let conn = open()?;
    conn.execute("DELETE FROM collection_clips WHERE collection_id=?1 AND source=?2 AND catalog_uid=?3 AND clip_uid=?4",params![collection_id,source.as_str(),identity.catalog_uid,identity.clip_uid]).map_err(|err|err.to_string())?;
    Ok(())
}

pub fn collection_memberships(source: ClipSource, id: i64) -> Result<Vec<i64>, String> {
    let identity = clip_catalog::identity(source, id)?;
    let conn = open()?;
    let mut stmt=conn.prepare("SELECT collection_id FROM collection_clips WHERE source=?1 AND catalog_uid=?2 AND clip_uid=?3 ORDER BY collection_id").map_err(|err|err.to_string())?;
    let rows = stmt
        .query_map(
            params![source.as_str(), identity.catalog_uid, identity.clip_uid],
            |row| row.get(0),
        )
        .map_err(|err| err.to_string())?;
    rows.collect::<Result<Vec<_>, _>>()
        .map_err(|err| err.to_string())
}

pub fn collection_clips(collection_id: i64) -> Result<Vec<clip_catalog::Clip>, String> {
    let conn = open()?;
    let mut stmt=conn.prepare("SELECT source,clip_id,catalog_uid,clip_uid FROM collection_clips WHERE collection_id=?1 ORDER BY added_at_utc DESC,clip_id DESC").map_err(|err|err.to_string())?;
    let entries = stmt
        .query_map(params![collection_id], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, String>(3)?,
            ))
        })
        .map_err(|err| err.to_string())?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|err| err.to_string())?;
    let mut clips = Vec::new();
    for source in [ClipSource::Replay, ClipSource::Manual] {
        let members: Vec<_> = entries
            .iter()
            .filter(|entry| entry.0 == source.as_str())
            .collect();
        if members.is_empty() {
            continue;
        }
        let ids: Vec<_> = members.iter().map(|entry| entry.1).collect();
        let available = clip_catalog::clips_by_ids(source, &ids)?;
        for member in members {
            if let Some(clip) = available.get(&member.1) {
                if clip.catalog_uid == member.2 && clip.clip_uid == member.3 {
                    clips.push(clip.clone());
                }
            }
        }
    }
    Ok(clips)
}
