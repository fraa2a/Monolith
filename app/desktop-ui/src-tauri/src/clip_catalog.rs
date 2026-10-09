use crate::{game_catalog, settings_store};
use rusqlite::{params, Connection, OpenFlags, OptionalExtension};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeSet, HashMap};
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

#[derive(Clone, Serialize)]
pub struct Clip {
    pub id: i64,
    pub source: String,
    pub catalog_uid: String,
    pub clip_uid: String,
    pub media_revision: i64,
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

fn catalog_path(source: ClipSource, folder: &Path) -> PathBuf {
    folder.join(match source { ClipSource::Replay=>"clips.db",ClipSource::Manual=>"recs.db" })
}

fn media_folder(source: ClipSource) -> PathBuf {
    let dirs = settings_store::output_dirs();
    match source {
        ClipSource::Replay => dirs.clips,
        ClipSource::Manual => dirs.recs,
    }
}

fn open(source: ClipSource) -> Option<Connection> {
    let conn =
        Connection::open_with_flags(catalog_path(source,&media_folder(source)),OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    conn.busy_timeout(std::time::Duration::from_millis(4000))
        .ok()?;
    Some(conn)
}

#[derive(Clone)]
pub struct ClipIdentity {
    pub catalog_uid: String,
    pub clip_uid: String,
}

pub fn identity(source: ClipSource, id: i64) -> Result<ClipIdentity, String> {
    let conn = open(source).ok_or_else(|| "catalog unavailable".to_string())?;
    let identity = conn.query_row(
        "SELECT (SELECT value FROM catalog_metadata WHERE key='catalog_uid'), clip_uid FROM clips WHERE id=?1",
        params![id], |row| Ok(ClipIdentity { catalog_uid: row.get(0)?, clip_uid: row.get(1)? }),
    ).map_err(|err| format!("catalog identity unavailable: {err}"))?;
    if identity.catalog_uid.is_empty() || identity.clip_uid.is_empty() {
        return Err("catalog identity migration required; start the recorder".into());
    }
    Ok(identity)
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
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
    }) else {
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
    let uid_expr = if has_column(conn, "clips", "clip_uid") {
        "clip_uid"
    } else {
        "''"
    };
    let revision_expr = if has_column(conn, "clips", "media_revision") {
        "media_revision"
    } else {
        "0"
    };
    let catalog_uid = conn
        .query_row(
            "SELECT value FROM catalog_metadata WHERE key='catalog_uid'",
            [],
            |row| row.get::<_, String>(0),
        )
        .unwrap_or_default();
    let catalog_uid = catalog_uid.replace('\'', "''");
    format!(
        "SELECT id, video_file, thumbnail_file, created_at_utc, duration_seconds,
                game_process_name, game_display_name, favorite,
                COALESCE(NULLIF(title, ''), 'Untitled'), {discord_expr}, {exe_path_expr}, '{catalog_uid}', {uid_expr}, {revision_expr}
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
    // Artwork lookup is cache-only; network refresh runs separately.
    let art = artwork.resolve(discord_app_id.as_deref(), game_process_name.as_deref());
    let video_path = folder.join(&video_file);
    let thumbnail_path = thumbnail_file.as_ref().map(|file| {
        folder
            .join(".thumbs")
            .join(file)
            .to_string_lossy()
            .to_string()
    });

    Ok(Clip {
        id,
        source: source.as_str().to_string(),
        catalog_uid: row.get(11)?,
        clip_uid: row.get(12)?,
        media_revision: row.get(13)?,
        video_file,
        title: row
            .get::<_, String>(8)
            .unwrap_or_else(|_| "Untitled".to_string()),
        thumbnail_file,
        created_at_utc: row.get(3)?,
        duration_seconds: row.get(4)?,
        game_process_name,
        game_display_name: game_display_name.or_else(|| {
            if art.display_name.is_empty() {
                None
            } else {
                Some(art.display_name.clone())
            }
        }),
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

fn read_source(source: ClipSource,filter: &ClipFilter) -> Vec<Clip> {
    read_source_in(source,filter,&media_folder(source))
}

pub(crate) fn read_source_in(source: ClipSource,filter: &ClipFilter,folder: &Path) -> Vec<Clip> {
    let Ok(conn)=Connection::open_with_flags(catalog_path(source,folder),OpenFlags::SQLITE_OPEN_READ_ONLY) else {
        return Vec::new();
    };
    let _=conn.busy_timeout(std::time::Duration::from_millis(4000));
    let tags = clip_hashtags(&conn);
    let artwork = game_catalog::ArtworkCache::load();
    // ISO-8601 strings sort lexicographically; list_clips re-sorts the merged
    // sources anyway, and datetime() here would defeat any index.
    let sql = format!("{} ORDER BY created_at_utc DESC", clip_select_sql(&conn));

    let Ok(mut stmt) = conn.prepare(&sql) else {
        return Vec::new();
    };

    let Ok(rows) = stmt.query_map([], |row| {
        map_clip_row(row,source,folder,&tags,&artwork)
    }) else {
        return Vec::new();
    };

    rows.flatten()
        .filter(|clip| matches_filter(clip, filter))
        .collect()
}

pub fn clips_by_ids(source: ClipSource,ids: &[i64]) -> Result<HashMap<i64,Clip>,String> {
    clips_by_ids_in(source,ids,&media_folder(source))
}

pub(crate) fn clips_by_ids_in(source: ClipSource,ids: &[i64],folder: &Path) -> Result<HashMap<i64,Clip>,String> {
    let path=catalog_path(source,folder);
    path.metadata()
        .map_err(|err| format!("catalog unavailable: {err}"))?;
    let conn = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY)
        .map_err(|err| err.to_string())?;
    conn.busy_timeout(std::time::Duration::from_millis(4000))
        .map_err(|err| err.to_string())?;
    let tags = clip_hashtags(&conn);
    let artwork = game_catalog::ArtworkCache::load();
    let sql = format!("{} WHERE id = ?1", clip_select_sql(&conn));
    let mut stmt = conn.prepare(&sql).map_err(|err| err.to_string())?;
    let mut clips = HashMap::new();
    for &id in ids {
        let clip = stmt
            .query_row(params![id], |row| {
                map_clip_row(row,source,folder,&tags,&artwork)
            })
            .optional()
            .map_err(|err| err.to_string())?;
        if let Some(clip) = clip {
            match std::fs::metadata(&clip.video_path) {
                Ok(meta) if meta.is_file() => {
                    clips.insert(id, clip);
                }
                Ok(_) => {}
                Err(err) if err.kind() == std::io::ErrorKind::NotFound => {}
                Err(err) => return Err(err.to_string()),
            }
        }
    }
    Ok(clips)
}

pub fn clip_by_id(source: ClipSource, id: i64) -> Option<Clip> {
    clips_by_ids(source, &[id]).ok()?.remove(&id)
}

fn matches_filter(clip: &Clip, filter: &ClipFilter) -> bool {
    if filter
        .game
        .as_deref()
        .is_some_and(|game| clip.game_display_name.as_deref() != Some(game))
    {
        return false;
    }
    if filter.favorite == Some(true) && !clip.favorite {
        return false;
    }
    if filter
        .hashtag
        .as_ref()
        .is_some_and(|tag| !clip.hashtags.contains(tag))
    {
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

#[derive(Serialize)]
pub struct BookmarkRow {
    pub seq: i64,
    pub time_seconds: f64,
    pub label: String,
    pub color: String,
}

pub fn list_bookmarks(source: ClipSource, id: i64) -> Result<Vec<BookmarkRow>, String> {
    let Some(conn) = open(source) else {
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
    rows.collect::<Result<Vec<_>, _>>()
        .map_err(|err| err.to_string())
}
