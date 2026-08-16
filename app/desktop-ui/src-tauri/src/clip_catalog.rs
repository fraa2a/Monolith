use crate::{game_catalog, settings_store};
use rusqlite::{params, Connection, OpenFlags};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy)]
pub enum ClipSource {
    Replay,
    Manual,
}

impl ClipSource {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Replay => "replay",
            Self::Manual => "manual",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "replay" => Some(Self::Replay),
            "manual" => Some(Self::Manual),
            _ => None,
        }
    }
}

#[derive(Default, Deserialize)]
pub struct ClipFilter {
    pub game: Option<String>,
    pub hashtag: Option<String>,
    pub favorite: Option<bool>,
    pub search: Option<String>,
}

#[derive(Serialize)]
pub struct Clip {
    pub id: i64,
    pub source: String,
    pub video_file: String,
    pub title: String,
    pub thumbnail_file: Option<String>,
    pub created_at_utc: String,
    pub duration_seconds: Option<f64>,
    pub game_process_name: Option<String>,
    pub game_display_name: Option<String>,
    pub game_executable_path: Option<String>,
    pub discord_app_id: Option<String>,
    pub game_icon_url: Option<String>,
    pub game_cover_url: Option<String>,
    pub favorite: bool,
    pub hashtags: Vec<String>,
    pub size_bytes: u64,
    pub video_path: String,
    pub thumbnail_path: Option<String>,
}

fn db_path(source: ClipSource) -> PathBuf {
    let dirs = settings_store::output_dirs();
    match source {
        ClipSource::Replay => dirs.clips.join("clips.db"),
        ClipSource::Manual => dirs.recs.join("recs.db"),
    }
}

fn media_folder(source: ClipSource) -> PathBuf {
    let dirs = settings_store::output_dirs();
    match source {
        ClipSource::Replay => dirs.clips,
        ClipSource::Manual => dirs.recs,
    }
}

// ── Bookmarks ─────────────────────────────────────────────────────────────
// The engine inserts bookmarks at recording-save time (timestamped live from
// its own clock); the UI reads/edits them here. Table DDL mirrors
// engine's storage.cpp so either side can create it (IF NOT EXISTS).

const BOOKMARK_DDL: &str = "CREATE TABLE IF NOT EXISTS clip_bookmarks (
    clip_id INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    time_seconds REAL NOT NULL,
    label TEXT NOT NULL,
    color TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (clip_id, seq))";

fn open(source: ClipSource, readonly: bool) -> Option<Connection> {
    let path = db_path(source);
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
    if !readonly {
        // Ensure the bookmarks table exists once per write connection instead
        // of re-running the DDL on every add_bookmark insert.
        let _ = conn.execute(BOOKMARK_DDL, []);
    }
    Some(conn)
}

fn file_size(path: &Path) -> u64 {
    path.metadata().map(|m| m.len()).unwrap_or(0)
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

// clip_id -> tags, built once per catalog pass (a linear filter per row made
// listing O(clips x tags)).
fn clip_hashtags(conn: &Connection) -> HashMap<i64, Vec<String>> {
    let mut map: HashMap<i64, Vec<String>> = HashMap::new();
    let Ok(mut stmt) = conn.prepare("SELECT clip_id, tag FROM clip_hashtags") else {
        return map;
    };
    let Ok(rows) = stmt.query_map([], |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))) else {
        return map;
    };
    for (id, tag) in rows.flatten() {
        map.entry(id).or_default().push(tag);
    }
    map
}

fn clip_select_sql(conn: &Connection) -> String {
    let discord_expr = if has_column(conn, "clips", "discord_app_id") {
        "discord_app_id"
    } else {
        "NULL"
    };
    let exe_path_expr = if has_column(conn, "clips", "game_executable_path") {
        "game_executable_path"
    } else {
        "NULL"
    };
    format!(
        "SELECT id, video_file, thumbnail_file, created_at_utc, duration_seconds,
                game_process_name, game_display_name, favorite,
                COALESCE(NULLIF(title, ''), 'Untitled'), {discord_expr}, {exe_path_expr}
         FROM clips",
    )
}

fn map_clip_row(
    row: &rusqlite::Row<'_>,
    source: ClipSource,
    folder: &Path,
    tags: &HashMap<i64, Vec<String>>,
    artwork: &game_catalog::ArtworkCache,
) -> rusqlite::Result<Clip> {
    let id = row.get::<_, i64>(0)?;
    let video_file = row.get::<_, String>(1)?;
    let thumbnail_file = row.get::<_, Option<String>>(2)?;
    let game_process_name = row.get::<_, Option<String>>(5)?;
    let game_display_name = row.get::<_, Option<String>>(6)?;
    let discord_app_id = row.get::<_, Option<String>>(9)?;
    let game_executable_path = row.get::<_, Option<String>>(10)?;
    // Cache-only read: the clip grid must never make a synchronous network
    // call. Artwork not yet cached shows up once the scheduled refresh
    // (see game_catalog::refresh_stale) has run.
    let art = artwork.resolve(discord_app_id.as_deref(), game_process_name.as_deref());
    let video_path = folder.join(&video_file);
    let thumbnail_path = thumbnail_file
        .as_ref()
        .map(|file| folder.join(".thumbs").join(file).to_string_lossy().to_string());

    Ok(Clip {
        id,
        source: source.as_str().to_string(),
        video_file,
        title: row.get::<_, String>(8).unwrap_or_else(|_| "Untitled".to_string()),
        thumbnail_file,
        created_at_utc: row.get(3)?,
        duration_seconds: row.get(4)?,
        game_process_name,
        game_display_name: game_display_name.or_else(|| if art.display_name.is_empty() { None } else { Some(art.display_name.clone()) }),
        game_executable_path,
        discord_app_id: discord_app_id.or(art.discord_app_id.clone()),
        game_icon_url: art.icon_url,
        game_cover_url: art.cover_url,
        favorite: row.get::<_, i64>(7).unwrap_or(0) != 0,
        hashtags: tags.get(&id).cloned().unwrap_or_default(),
        size_bytes: file_size(&video_path),
        video_path: video_path.to_string_lossy().to_string(),
        thumbnail_path,
    })
}

fn read_source(source: ClipSource, filter: &ClipFilter) -> Vec<Clip> {
    let Some(conn) = open(source, true) else {
        return Vec::new();
    };
    let folder = media_folder(source);
    let tags = clip_hashtags(&conn);
    let artwork = game_catalog::ArtworkCache::load();
    // ISO-8601 strings sort lexicographically; list_clips re-sorts the merged
    // sources anyway, and datetime() here would defeat any index.
    let sql = format!("{} ORDER BY created_at_utc DESC", clip_select_sql(&conn));

    let Ok(mut stmt) = conn.prepare(&sql) else {
        return Vec::new();
    };

    let Ok(rows) = stmt.query_map([], |row| map_clip_row(row, source, &folder, &tags, &artwork)) else {
        return Vec::new();
    };

    rows.flatten().filter(|clip| matches_filter(clip, filter)).collect()
}

// Single-clip lookup (used by collections: prune + materialize the clips a
// collection references). Returns None when the catalog is missing, the row
// is gone, or the file no longer exists.
pub fn clip_by_id(source: ClipSource, id: i64) -> Option<Clip> {
    let conn = open(source, true)?;
    let folder = media_folder(source);
    let tags = clip_hashtags(&conn);
    let artwork = game_catalog::ArtworkCache::load();
    let sql = format!("{} WHERE id = ?1", clip_select_sql(&conn));
    let clip = conn
        .query_row(&sql, params![id], |row| map_clip_row(row, source, &folder, &tags, &artwork))
        .ok()?;
    if !clip.video_path.is_empty() && std::path::Path::new(&clip.video_path).is_file() {
        Some(clip)
    } else {
        None
    }
}

fn matches_filter(clip: &Clip, filter: &ClipFilter) -> bool {
    if filter.game.as_deref().is_some_and(|game| clip.game_display_name.as_deref() != Some(game)) {
        return false;
    }
    if filter.favorite == Some(true) && !clip.favorite {
        return false;
    }
    if filter.hashtag.as_ref().is_some_and(|tag| !clip.hashtags.contains(tag)) {
        return false;
    }
    if let Some(search) = &filter.search {
        let query = search.to_lowercase();
        let in_title = clip.title.to_lowercase().contains(&query);
        let in_file = clip.video_file.to_lowercase().contains(&query);
        let in_game = clip
            .game_display_name
            .as_ref()
            .is_some_and(|game| game.to_lowercase().contains(&query));
        if !in_title && !in_file && !in_game {
            return false;
        }
    }
    true
}

pub fn list_clips(filter: &ClipFilter) -> Vec<Clip> {
    let mut clips = read_source(ClipSource::Replay, filter);
    clips.extend(read_source(ClipSource::Manual, filter));
    clips.sort_by(|a, b| b.created_at_utc.cmp(&a.created_at_utc));
    clips
}

pub fn distinct_games() -> Vec<String> {
    let mut names = BTreeSet::new();
    for clip in list_clips(&ClipFilter::default()) {
        if let Some(name) = clip.game_display_name {
            names.insert(name);
        }
    }
    names.into_iter().collect()
}

pub fn distinct_hashtags() -> Vec<String> {
    let mut tags = BTreeSet::new();
    for clip in list_clips(&ClipFilter::default()) {
        for tag in clip.hashtags {
            tags.insert(tag);
        }
    }
    tags.into_iter().collect()
}

fn video_file_for(conn: &Connection, id: i64) -> Result<String, String> {
    conn.query_row("SELECT video_file FROM clips WHERE id = ?1", params![id], |row| row.get(0))
        .map_err(|_| "clip not found".to_string())
}

pub fn set_duration(source: ClipSource, id: i64, duration: f64) -> Result<(), String> {
    if !duration.is_finite() || duration <= 0.0 {
        return Err("invalid duration".to_string());
    }
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute("UPDATE clips SET duration_seconds = ?1 WHERE id = ?2", params![duration, id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("clip not found".to_string());
    }
    Ok(())
}

fn clip_exists(conn: &Connection, id: i64) -> bool {
    conn.query_row("SELECT 1 FROM clips WHERE id = ?1", params![id], |_| Ok(()))
        .is_ok()
}

fn thumb_basename_for(video_file: &str) -> String {
    Path::new(video_file)
        .file_stem()
        .map(|stem| format!("{}.png", stem.to_string_lossy()))
        .unwrap_or_else(|| "thumb.png".to_string())
}

pub fn set_favorite(source: ClipSource, id: i64, favorite: bool) -> Result<(), String> {
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute("UPDATE clips SET favorite = ?1 WHERE id = ?2", params![favorite as i64, id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("clip not found".to_string());
    }
    Ok(())
}

pub fn set_title(source: ClipSource, id: i64, title: &str) -> Result<(), String> {
    let value = if title.is_empty() { "Untitled" } else { title };
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute("UPDATE clips SET title = ?1 WHERE id = ?2", params![value, id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("clip not found".to_string());
    }
    Ok(())
}

pub fn add_hashtag(source: ClipSource, id: i64, tag: &str) -> Result<(), String> {
    if tag.is_empty() {
        return Err("empty tag".to_string());
    }
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    if !clip_exists(&conn, id) {
        return Err("clip not found".to_string());
    }
    conn.execute(
        "INSERT OR IGNORE INTO clip_hashtags (clip_id, tag) VALUES (?1, ?2)",
        params![id, tag],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

pub fn remove_hashtag(source: ClipSource, id: i64, tag: &str) -> Result<(), String> {
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute(
        "DELETE FROM clip_hashtags WHERE clip_id = ?1 AND tag = ?2",
        params![id, tag],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

#[derive(Serialize)]
pub struct BookmarkRow {
    pub seq: i64,
    pub time_seconds: f64,
    pub label: String,
    pub color: String,
}

pub fn list_bookmarks(source: ClipSource, id: i64) -> Result<Vec<BookmarkRow>, String> {
    let Some(conn) = open(source, true) else {
        return Err("catalog unavailable".to_string());
    };
    let Ok(mut stmt) = conn.prepare(
        "SELECT seq, time_seconds, label, color FROM clip_bookmarks
         WHERE clip_id = ?1 ORDER BY seq",
    ) else {
        // Table missing = no bookmarks yet (older DB written before this
        // feature shipped); treat as empty rather than an error.
        return Ok(Vec::new());
    };
    let rows = stmt
        .query_map(params![id], |row| {
            Ok(BookmarkRow {
                seq: row.get(0)?,
                time_seconds: row.get(1)?,
                label: row.get(2)?,
                color: row.get(3)?,
            })
        })
        .map_err(|err| err.to_string())?;
    rows.collect::<Result<Vec<_>, _>>().map_err(|err| err.to_string())
}

pub fn add_bookmark(
    source: ClipSource,
    id: i64,
    time_seconds: f64,
    label: &str,
    color: &str,
) -> Result<(), String> {
    if !time_seconds.is_finite() || time_seconds < 0.0 {
        return Err("invalid bookmark time".to_string());
    }
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    if !clip_exists(&conn, id) {
        return Err("clip not found".to_string());
    }
    let next_seq: i64 = conn
        .query_row(
            "SELECT COALESCE(MAX(seq), 0) + 1 FROM clip_bookmarks WHERE clip_id = ?1",
            params![id],
            |row| row.get(0),
        )
        .map_err(|err| err.to_string())?;
    conn.execute(
        "INSERT INTO clip_bookmarks (clip_id, seq, time_seconds, label, color)
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![id, next_seq, time_seconds, label, color],
    )
    .map_err(|err| err.to_string())?;
    Ok(())
}

pub fn update_bookmark(
    source: ClipSource,
    id: i64,
    seq: i64,
    label: &str,
    color: &str,
) -> Result<(), String> {
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute(
        "UPDATE clip_bookmarks SET label = ?1, color = ?2 WHERE clip_id = ?3 AND seq = ?4",
        params![label, color, id, seq],
    )
    .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("bookmark not found".to_string());
    }
    Ok(())
}

pub fn remove_bookmark(source: ClipSource, id: i64, seq: i64) -> Result<(), String> {
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    conn.execute(
        "DELETE FROM clip_bookmarks WHERE clip_id = ?1 AND seq = ?2",
        params![id, seq],
    )
    .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("bookmark not found".to_string());
    }
    Ok(())
}

pub fn remove_clip(source: ClipSource, id: i64) -> Result<(), String> {
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    let row = conn
        .query_row(
            "SELECT video_file, thumbnail_file FROM clips WHERE id = ?1",
            params![id],
            |row| Ok((row.get::<_, String>(0)?, row.get::<_, Option<String>>(1)?)),
        )
        .map_err(|_| "clip not found".to_string())?;
    let (video_file, thumbnail_file) = row;

    // One transaction: a failure between the DELETEs must not strand hashtag
    // or bookmark rows pointing at a deleted clip.
    let tx = conn.unchecked_transaction().map_err(|err| err.to_string())?;
    tx.execute("DELETE FROM clip_bookmarks WHERE clip_id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    tx.execute("DELETE FROM clip_hashtags WHERE clip_id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    tx.execute("DELETE FROM clips WHERE id = ?1", params![id])
        .map_err(|err| err.to_string())?;
    tx.commit().map_err(|err| err.to_string())?;

    let folder = media_folder(source);
    let _ = fs::remove_file(folder.join(&video_file));
    let thumb = thumbnail_file.unwrap_or_else(|| thumb_basename_for(&video_file));
    let _ = fs::remove_file(folder.join(".thumbs").join(&thumb));
    Ok(())
}

pub fn rename_clip(source: ClipSource, id: i64, new_stem: &str) -> Result<(), String> {
    if new_stem.is_empty() || new_stem.chars().any(|c| "\\/:*?\"<>|.".contains(c)) {
        return Err("invalid clip name".to_string());
    }
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    let row = conn
        .query_row(
            "SELECT video_file, thumbnail_file FROM clips WHERE id = ?1",
            params![id],
            |row| Ok((row.get::<_, String>(0)?, row.get::<_, Option<String>>(1)?)),
        )
        .map_err(|_| "clip not found".to_string())?;
    let (old_video, old_thumb) = row;

    let folder = media_folder(source);
    let ext = Path::new(&old_video)
        .extension()
        .map(|e| e.to_string_lossy().to_string())
        .unwrap_or_default();
    let new_video = if ext.is_empty() {
        new_stem.to_string()
    } else {
        format!("{new_stem}.{ext}")
    };
    let new_thumb = format!("{new_stem}.png");

    if folder.join(&new_video).is_file() {
        return Err("a clip with that name already exists".to_string());
    }

    fs::rename(folder.join(&old_video), folder.join(&new_video))
        .map_err(|_| "could not rename video file".to_string())?;

    let old_thumb_name = old_thumb.unwrap_or_else(|| thumb_basename_for(&old_video));
    let thumb_renamed = fs::rename(
        folder.join(".thumbs").join(&old_thumb_name),
        folder.join(".thumbs").join(&new_thumb),
    )
    .is_ok();

    if let Err(err) = conn.execute(
        "UPDATE clips SET video_file = ?1, thumbnail_file = ?2 WHERE id = ?3",
        params![new_video, if thumb_renamed { Some(&new_thumb) } else { None }, id],
    ) {
        // Roll the filesystem back so the DB never points at files that no
        // longer exist under their recorded names.
        let _ = fs::rename(folder.join(&new_video), folder.join(&old_video));
        if thumb_renamed {
            let _ = fs::rename(
                folder.join(".thumbs").join(&new_thumb),
                folder.join(".thumbs").join(&old_thumb_name),
            );
        }
        return Err(err.to_string());
    }
    Ok(())
}

pub fn save_thumbnail_capture(source: ClipSource, id: i64, png: &[u8]) -> Result<String, String> {
    if png.is_empty() || png.len() > 8 * 1024 * 1024 {
        return Err("invalid thumbnail".to_string());
    }
    if !png.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err("thumbnail must be png".to_string());
    }
    let Some(conn) = open(source, false) else {
        return Err("catalog unavailable".to_string());
    };
    let video_file = video_file_for(&conn, id)?;
    let stem = Path::new(&video_file)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .filter(|s| !s.is_empty())
        .ok_or_else(|| "invalid video name".to_string())?;
    let thumb_file = format!("{stem}.png");
    let folder = media_folder(source).join(".thumbs");
    fs::create_dir_all(&folder).map_err(|err| err.to_string())?;
    fs::write(folder.join(&thumb_file), png).map_err(|err| err.to_string())?;
    conn.execute("UPDATE clips SET thumbnail_file = ?1 WHERE id = ?2", params![thumb_file, id])
        .map_err(|err| err.to_string())?;
    if conn.changes() == 0 {
        return Err("clip not found".to_string());
    }
    Ok(thumb_file)
}
