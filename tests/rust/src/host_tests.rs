use crate::{clip_catalog, collections, paths, settings_store};
use rusqlite::Connection;
use serde_json::json;
use std::fs;

#[test]
fn memberships_survive_missing_media_and_reject_reused_ids_and_catalogs() {
    let _guard = crate::CATALOG_ENV_LOCK
        .lock()
        .unwrap_or_else(|err| err.into_inner());
    let root = std::env::temp_dir().join(format!("monolith-host-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();
    std::env::set_var("LOCALAPPDATA", &root);
    let clips = root.join("clips");
    fs::create_dir_all(&clips).unwrap();
    settings_store::write_config(
        &json!({"output":{"clips_directory":clips,"recordings_directory":root.join("recs")}}),
    )
    .unwrap();
    let db = Connection::open(clips.join("clips.db")).unwrap();
    db.execute_batch("CREATE TABLE catalog_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
      INSERT INTO catalog_metadata VALUES('catalog_uid','catalog-a');
      CREATE TABLE clips(id INTEGER PRIMARY KEY,video_file TEXT,thumbnail_file TEXT,created_at_utc TEXT,duration_seconds REAL,game_process_name TEXT,game_display_name TEXT,favorite INTEGER,title TEXT,clip_uid TEXT NOT NULL);
      CREATE TABLE clip_hashtags(clip_id INTEGER,tag TEXT);
      INSERT INTO clips VALUES(1,'a.mp4',NULL,'2026-10-08T00:00:00Z',5,NULL,NULL,0,'A','clip-a');").unwrap();
    fs::write(clips.join("a.mp4"), b"video").unwrap();
    fs::create_dir_all(paths::monolith_data_dir()).unwrap();
    let legacy = Connection::open(paths::monolith_data_dir().join("collections.db")).unwrap();
    legacy.execute_batch("CREATE TABLE collections(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,color TEXT NOT NULL,created_at_utc TEXT NOT NULL);
      CREATE TABLE collection_clips(collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,source TEXT NOT NULL,clip_id INTEGER NOT NULL,added_at_utc TEXT NOT NULL,PRIMARY KEY(collection_id,source,clip_id));
      INSERT INTO collections VALUES(1,'Legacy','','2026-10-08T00:00:00Z');
      INSERT INTO collection_clips VALUES(1,'replay',1,'2026-10-08T00:00:00Z');").unwrap();
    let summaries = collections::list_collections().unwrap();
    assert_eq!(summaries[0].unresolved_count, 1);
    assert_eq!(summaries[0].clip_count, 1);
    assert!(collections::collection_clips(1).unwrap().is_empty());
    collections::add_clip_to_collection(1, clip_catalog::ClipSource::Replay, 1).unwrap();
    assert_eq!(
        collections::list_collections().unwrap()[0].unresolved_count,
        0
    );
    assert_eq!(
        collections::collection_memberships(clip_catalog::ClipSource::Replay, 1).unwrap(),
        vec![1]
    );
    collections::delete_collection(1).unwrap();
    drop(legacy);
    let collection = collections::create_collection("Games", "").unwrap();
    collections::add_clip_to_collection(collection, clip_catalog::ClipSource::Replay, 1).unwrap();
    fs::rename(clips.join("a.mp4"), clips.join("moved.mp4")).unwrap();
    assert!(collections::collection_clips(collection)
        .unwrap()
        .is_empty());
    let members = Connection::open(paths::monolith_data_dir().join("collections.db")).unwrap();
    let count: i64 = members
        .query_row("SELECT COUNT(*) FROM collection_clips", [], |r| r.get(0))
        .unwrap();
    assert_eq!(
        count, 1,
        "a transient missing media file must not remove membership"
    );
    fs::rename(clips.join("moved.mp4"), clips.join("a.mp4")).unwrap();
    assert_eq!(collections::collection_clips(collection).unwrap().len(), 1);
    db.execute_batch("ALTER TABLE clip_hashtags RENAME TO tags_unavailable;")
        .unwrap();
    assert_eq!(
        collections::collection_memberships(clip_catalog::ClipSource::Replay, 1).unwrap(),
        vec![collection]
    );
    db.execute_batch("ALTER TABLE tags_unavailable RENAME TO clip_hashtags;")
        .unwrap();
    db.execute_batch("DELETE FROM clips;INSERT INTO clips VALUES(1,'a.mp4',NULL,'2026-10-09T00:00:00Z',5,NULL,NULL,0,'B','clip-b');").unwrap();
    assert!(
        collections::collection_clips(collection)
            .unwrap()
            .is_empty(),
        "reused row IDs must not inherit membership"
    );
    assert!(collections::add_clip_with_identity(
        collection,
        clip_catalog::ClipSource::Replay,
        1,
        clip_catalog::ClipIdentity {
            catalog_uid: "catalog-a".into(),
            clip_uid: "clip-a".into()
        }
    )
    .is_err());
    db.execute_batch(
        "UPDATE clips SET clip_uid='clip-a';UPDATE catalog_metadata SET value='catalog-b';",
    )
    .unwrap();
    assert!(
        collections::collection_clips(collection)
            .unwrap()
            .is_empty(),
        "another catalog must not inherit membership"
    );
    drop(members);
    drop(db);
    fs::remove_dir_all(root).unwrap();
}
