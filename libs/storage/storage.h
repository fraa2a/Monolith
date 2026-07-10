#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// libs/storage — SQLite-backed clip catalog co-located with each output folder.
//
// Layout (auto-contained per output folder chosen in Settings > Output):
//   <folder>\clips.db   (source "replay")   or   <folder>\recs.db (source "manual")
//   <folder>\*.mp4|*.mkv                     (the video files, written as before)
//   <folder>\.thumbs\<base>.png              (first-frame thumbnails)
//
// The recorder (the always-on process) is the single writer at clip-save time
// and for reconcile. UI-driven mutations (favorite/hashtag/delete) also route
// through the recorder over IPC so there is exactly one writer per DB. The UI
// process opens the same DB read-only for the grid. All connections use WAL +
// busy_timeout so reads never block the writer.

namespace storage {

// ISO-8601 UTC timestamp, e.g. "2026-07-06T09:12:34Z". Helper for callers that
// need a created_at value at save time.
std::string now_iso8601_utc();

// ── Global settings store (settings.db, key/value) ──────────────────────────
//
// Replaces the old config.json file (ADR-0009 rewrite). Lives at
// <app_data_dir>\settings.db in WAL mode. A generic string KV table; the caller
// (settings_config.cpp) owns the schema meaning of keys — this layer stays
// JSON-agnostic. The UI process reads/writes the same table for the settings popup;
// writes are user-driven and infrequent, so single-writer contention is a
// non-issue in practice, and WAL keeps reads non-blocking either way.

// Reads all (key,value) rows. Returns true on success (out may be empty when the
// DB is new/absent — treated as "no saved settings yet", not an error).
bool settings_get_all(const std::wstring& app_data_dir,
                      std::vector<std::pair<std::string, std::string>>& out,
                      std::string* error);

// Replaces the entire settings table with `kv` in one transaction.
bool settings_replace_all(const std::wstring& app_data_dir,
                          const std::vector<std::pair<std::string, std::string>>& kv,
                          std::string* error);

// One row of the `clips` table. Paths are stored as basenames relative to the
// output folder / .thumbs subfolder, so the catalog stays portable if the
// folder moves.
struct ClipRow {
    std::wstring video_file;       // basename, e.g. L"20260706_...clip.mkv"
    std::wstring thumbnail_file;   // basename in .thumbs, or empty if none
    std::string  title;            // user-facing display name; "" -> "Untitled"
    std::string  created_at_utc;   // ISO-8601; empty -> now_iso8601_utc()
    std::string  source;           // "replay" | "manual"
    double       duration_seconds = 0.0;
    std::string  game_process_name;
    std::string  game_display_name;
    std::string  game_executable_path; // full path; UI extracts an icon from it
    std::string  discord_app_id;
    std::string  game_source;      // "steam"|"game_db"|"heuristic"|"manual"|""
    int64_t      steam_app_id = 0; // 0 = unknown
    int          confidence = 0;
    bool         favorite = false;
};

struct ReconcileStats {
    int removed = 0;             // rows dropped because the video vanished
    int thumbs_regenerated = 0;  // missing thumbs rebuilt
    int imported = 0;            // pre-existing videos with no row, imported
};

// A per-folder clip database. Open with ClipDb::open(); never construct directly.
class ClipDb {
public:
    // Opens (creating if absent) the DB for a self-contained output folder.
    //   source "replay" -> <folder>\clips.db ; source "manual" -> <folder>\recs.db
    // Returns nullptr (and sets *error) if the folder is empty, or if a file is
    // present but is NOT a valid Monolith clip DB — it is never overwritten.
    static std::unique_ptr<ClipDb> open(const std::wstring& folder,
                                        const std::string& source,
                                        std::string* error);

    ~ClipDb();
    ClipDb(const ClipDb&)            = delete;
    ClipDb& operator=(const ClipDb&) = delete;

    const std::wstring& folder() const;      // the output root
    std::wstring thumbs_dir() const;         // <folder>\.thumbs

    // Inserts a clip row. Returns the new row id (>0) or -1 on error.
    int64_t insert_clip(const ClipRow& row, std::string* error);

    // Deletes the row and, when remove_files, the video + thumbnail on disk.
    bool remove_clip(int64_t id, bool remove_files, std::string* error);

    bool set_favorite(int64_t id, bool favorite, std::string* error);

    // Updates the clip's display title only. Does NOT touch the video file on
    // disk — title is independent of the filename. Empty title becomes "Untitled".
    bool set_title(int64_t id, const std::string& title, std::string* error);

    bool add_hashtag(int64_t id, const std::string& tag, std::string* error);
    bool remove_hashtag(int64_t id, const std::string& tag, std::string* error);

    // Regenerates the first-frame thumbnail for a single clip (used when the UI
    // finds a thumbnail missing or corrupt). Decodes the video and rewrites the
    // .png, updating the stored thumbnail_file. Returns false + *error if the
    // clip/video is missing or decoding fails. Blocking; call off the UI thread.
    bool regenerate_thumbnail(int64_t id, std::string* error);

    // Renames the clip FILE on disk: moves the video file and its thumbnail to
    // <new_stem> + original extension (in place, same folder/.thumbs) and updates
    // the row. new_stem is a base name with no path/extension; invalid or
    // colliding names fail without touching disk. This is a distinct, optional
    // action from set_title — the on-screen name is the title, not the filename.
    // Returns false + *error on any failure.
    bool rename_clip(int64_t id, const std::wstring& new_stem, std::string* error);

    // Self-heal + migration. Safe to call from a background thread:
    //   - row whose video file is gone      -> delete row (+ its thumb)
    //   - row whose thumbnail is gone/empty  -> regenerate from the video
    //   - video file on disk with no row     -> import it (+ generate thumb)
    ReconcileStats reconcile(std::string* error);

private:
    ClipDb();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace storage
