use crate::settings_store;
use rusqlite::{Connection, OpenFlags};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy)]
enum ClipSource {
    Replay,
    Manual,
}

impl ClipSource {
    fn as_str(self) -> &'static str {
        match self {
            Self::Replay => "replay",
            Self::Manual => "manual",
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
    pub thumbnail_file: Option<String>,
    pub created_at_utc: String,
    pub duration_seconds: Option<f64>,
    pub game_process_name: Option<String>,
    pub game_display_name: Option<String>,
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

fn open_readonly(source: ClipSource) -> Option<Connection> {
    let path = db_path(source);
    if !path.is_file() {
        return None;
    }
    let conn = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    let _ = conn.busy_timeout(std::time::Duration::from_millis(4000));
    Some(conn)
}

fn file_size(path: &Path) -> u64 {
    path.metadata().map(|m| m.len()).unwrap_or(0)
}

fn clip_hashtags(conn: &Connection) -> Vec<(i64, String)> {
    let Ok(mut stmt) = conn.prepare("SELECT clip_id, tag FROM clip_hashtags") else {
        return Vec::new();
    };
    let Ok(rows) = stmt.query_map([], |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))) else {
        return Vec::new();
    };
    rows.flatten().collect()
}

fn tags_for(tags: &[(i64, String)], clip_id: i64) -> Vec<String> {
    tags.iter()
        .filter(|(id, _)| *id == clip_id)
        .map(|(_, tag)| tag.clone())
        .collect()
}

fn read_source(source: ClipSource, filter: &ClipFilter) -> Vec<Clip> {
    let Some(conn) = open_readonly(source) else {
        return Vec::new();
    };
    let folder = media_folder(source);
    let tags = clip_hashtags(&conn);

    let Ok(mut stmt) = conn.prepare(
        "SELECT id, video_file, thumbnail_file, created_at_utc, duration_seconds,
                game_process_name, game_display_name, favorite
         FROM clips ORDER BY datetime(created_at_utc) DESC",
    ) else {
        return Vec::new();
    };

    let Ok(rows) = stmt.query_map([], |row| {
        let id = row.get::<_, i64>(0)?;
        let video_file = row.get::<_, String>(1)?;
        let thumbnail_file = row.get::<_, Option<String>>(2)?;
        let video_path = folder.join(&video_file);
        let thumbnail_path = thumbnail_file
            .as_ref()
            .map(|file| folder.join(".thumbs").join(file).to_string_lossy().to_string());

        Ok(Clip {
            id,
            source: source.as_str().to_string(),
            video_file,
            thumbnail_file,
            created_at_utc: row.get(3)?,
            duration_seconds: row.get(4)?,
            game_process_name: row.get(5)?,
            game_display_name: row.get(6)?,
            favorite: row.get::<_, i64>(7).unwrap_or(0) != 0,
            hashtags: tags_for(&tags, id),
            size_bytes: file_size(&video_path),
            video_path: video_path.to_string_lossy().to_string(),
            thumbnail_path,
        })
    }) else {
        return Vec::new();
    };

    rows.flatten().filter(|clip| matches_filter(clip, filter)).collect()
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
        let in_file = clip.video_file.to_lowercase().contains(&query);
        let in_game = clip
            .game_display_name
            .as_ref()
            .is_some_and(|game| game.to_lowercase().contains(&query));
        if !in_file && !in_game {
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
