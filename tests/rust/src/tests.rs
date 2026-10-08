use crate::{archive, clip_catalog, collections, game_catalog, manifest, paths, settings_store};
use base64::Engine;
use rusqlite::Connection;
use serde_json::json;
use std::{fs, io::Write, path::Path};

fn component() -> manifest::ComponentInfo {
    manifest::ComponentInfo {
        version: "1.2.3".into(),
        url: "https://example.com/component.zip".into(),
        size: 1,
        sha256: "ab".repeat(32),
        ed_signature: base64::engine::general_purpose::STANDARD.encode([0u8; 64]),
    }
}

#[test]
fn manifest_requires_integrity_and_bounded_components() {
    let valid = component();
    assert!(valid.validate().is_ok());
    let mut bad = valid.clone();
    bad.ed_signature.clear();
    assert!(bad.validate().is_err());
    bad = valid.clone();
    bad.sha256 = "gg".repeat(32);
    assert!(bad.validate().is_err());
    bad = valid.clone();
    bad.size = 0;
    assert!(bad.validate().is_err());
    bad = valid.clone();
    bad.size = manifest::MAX_COMPONENT_BYTES + 1;
    assert!(bad.validate().is_err());
    bad = valid.clone();
    bad.url = "http://example.com/a".into();
    assert!(bad.validate().is_err());
    bad = valid.clone();
    bad.url.push('\0');
    assert!(bad.validate().is_err());
    bad = valid;
    bad.version = "nonsense".into();
    assert!(bad.validate().is_err());
    let mut value: manifest::Manifest =
        serde_json::from_value(json!({"schema": 1, "components": {}})).unwrap();
    assert!(value.validate().is_err());
    value.components.insert("engine".into(), component());
    assert!(value.validate().is_ok());
    value.components.insert("other".into(), component());
    assert!(value.validate().is_err());
    value.components.remove("other");
    value.schema = 2;
    assert!(value.validate().is_err());
}

fn make_zip(path: &Path, name: &str) {
    let mut writer = zip::ZipWriter::new(fs::File::create(path).unwrap());
    writer
        .start_file(name, zip::write::SimpleFileOptions::default())
        .unwrap();
    writer.write_all(b"payload").unwrap();
    writer.finish().unwrap();
}

#[test]
fn archive_rejects_windows_path_aliases_and_traversal() {
    let root = std::env::temp_dir().join(format!("monolith-archive-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();
    let zip = root.join("component.zip");
    for name in [
        "../outside",
        "/outside",
        "ui/file:stream",
        "ui/CON.txt",
        "ui/lpt1.log",
        "ui/trailing.",
        "ui/trailing ",
        "ui\\escape",
    ] {
        make_zip(&zip, name);
        assert!(
            archive::extract_zip(&zip, &root.join("staging")).is_err(),
            "accepted {name}"
        );
    }
    make_zip(&zip, "ui/Monolith.UI.exe");
    archive::extract_zip(&zip, &root.join("staging")).unwrap();
    assert_eq!(
        fs::read(root.join("staging/ui/Monolith.UI.exe")).unwrap(),
        b"payload"
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn catalogs_preserve_membership_on_errors_and_serialize_bookmarks() {
    let root = std::env::temp_dir().join(format!("monolith-catalogs-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();
    std::env::set_var("LOCALAPPDATA", &root);
    let clips = root.join("clips");
    let recs = root.join("recs");
    fs::create_dir_all(&clips).unwrap();
    fs::create_dir_all(&recs).unwrap();
    settings_store::write_config(
        &json!({"output": {"clips_directory": clips, "recordings_directory": recs}}),
    )
    .unwrap();
    assert_eq!(settings_store::output_dirs().clips, clips);
    let alternate = root.join("other");
    settings_store::write_config(
        &json!({"output": {"clips_directory": alternate, "recordings_directory": recs}}),
    )
    .unwrap();
    assert_eq!(settings_store::output_dirs().clips, alternate);
    settings_store::write_config(
        &json!({"output": {"clips_directory": clips, "recordings_directory": recs}}),
    )
    .unwrap();
    let settings = Connection::open(settings_store::settings_db_path()).unwrap();
    settings
        .execute_batch("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;")
        .unwrap();
    let alternate_config = json!({"clips_directory": alternate, "recordings_directory": recs});
    settings
        .execute(
            "UPDATE settings SET value=?1 WHERE key='output'",
            [alternate_config.to_string()],
        )
        .unwrap();
    assert_eq!(settings_store::output_dirs().clips, alternate);
    settings
        .execute(
            "UPDATE settings SET value=?1 WHERE key='output'",
            [json!({"clips_directory": clips,"recordings_directory": recs}).to_string()],
        )
        .unwrap();
    assert_eq!(settings_store::output_dirs().clips, clips);
    drop(settings);
    let game_db = paths::monolith_data_dir().join("game_catalog.db");
    let game = Connection::open(&game_db).unwrap();
    game.execute_batch("CREATE TABLE game_catalog (process_name_lower TEXT PRIMARY KEY);")
        .unwrap();
    game_catalog::store_exe_icon("Game.EXE", b"png");
    assert_eq!(
        game_catalog::cached_exe_icon("game.exe"),
        Some(b"png".to_vec())
    );
    assert!(game_catalog::entry_by_process("game.exe").is_some());
    let timestamp: i64 = game
        .query_row("SELECT last_updated FROM game_catalog", [], |r| r.get(0))
        .unwrap();
    assert_eq!(timestamp, 0);
    let db = Connection::open(clips.join("clips.db")).unwrap();
    db.execute_batch(
        "CREATE TABLE clips (id INTEGER PRIMARY KEY, video_file TEXT, thumbnail_file TEXT,
        created_at_utc TEXT, duration_seconds REAL, game_process_name TEXT, game_display_name TEXT,
        favorite INTEGER, title TEXT);
        CREATE TABLE clip_hashtags (clip_id INTEGER, tag TEXT);
        INSERT INTO clips VALUES (1,'a.mp4',NULL,'2026-10-08T00:00:00Z',5,NULL,NULL,0,'A');",
    )
    .unwrap();
    fs::write(clips.join("a.mp4"), b"video").unwrap();
    let collection = collections::create_collection("Games", "").unwrap();
    collections::add_clip_to_collection(collection, clip_catalog::ClipSource::Replay, 1).unwrap();
    assert_eq!(collections::collection_clips(collection).unwrap().len(), 1);
    let mut workers = Vec::new();
    for index in 0..8 {
        workers.push(std::thread::spawn(move || {
            clip_catalog::add_bookmark(
                clip_catalog::ClipSource::Replay,
                1,
                f64::from(index),
                "",
                "",
            )
            .unwrap();
        }));
    }
    for worker in workers {
        worker.join().unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(DISTINCT seq) FROM clip_bookmarks", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 8);
    db.execute_batch("ALTER TABLE clips RENAME TO broken;")
        .unwrap();
    assert!(collections::collection_clips(collection).is_err());
    let col_db = Connection::open(paths::monolith_data_dir().join("collections.db")).unwrap();
    let count: i64 = col_db
        .query_row("SELECT COUNT(*) FROM collection_clips", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
    db.execute_batch("ALTER TABLE broken RENAME TO clips;")
        .unwrap();
    fs::rename(clips.join("clips.db"), clips.join("clips.db.saved")).unwrap();
    assert!(collections::collection_clips(collection).is_err());
    let count: i64 = col_db
        .query_row("SELECT COUNT(*) FROM collection_clips", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
    fs::rename(clips.join("clips.db.saved"), clips.join("clips.db")).unwrap();
    fs::remove_file(clips.join("a.mp4")).unwrap();
    assert!(collections::collection_clips(collection)
        .unwrap()
        .is_empty());
    collections::delete_collection(collection).unwrap();
    drop(col_db);
    drop(db);
    drop(game);
    fs::remove_dir_all(root).unwrap();
}
