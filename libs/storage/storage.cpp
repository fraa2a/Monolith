#include "storage.h"

#include <encoding/encoding.h>

#include <sqlite3.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace storage {
namespace {

std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                  nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

bool is_video_ext(const fs::path& p)
{
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".mp4" || ext == L".mkv";
}

std::wstring thumb_basename_for(const std::wstring& video_basename)
{
    return fs::path(video_basename).stem().wstring() + L".png";
}

// Column set every valid Monolith clip DB must expose.
bool clips_table_valid(sqlite3* db)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(clips)", -1, &st, nullptr) != SQLITE_OK)
        return false;
    std::unordered_set<std::string> cols;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(st, 1);
        if (name) cols.insert(reinterpret_cast<const char*>(name));
    }
    sqlite3_finalize(st);
    for (const char* req : {"id", "video_file", "thumbnail_file",
                            "created_at_utc", "source"})
        if (!cols.count(req)) return false;
    return true;
}

int table_count(sqlite3* db)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%'", -1, &st, nullptr) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool has_column(sqlite3* db, const char* table, const char* col)
{
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(st, 1);
        if (name && col && std::string(reinterpret_cast<const char*>(name)) == col) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

bool exec(sqlite3* db, const char* sql, std::string* error)
{
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        if (error) *error = msg ? msg : "sqlite exec failed";
        if (msg) sqlite3_free(msg);
        return false;
    }
    return true;
}

const char* kCreateSchema =
    "CREATE TABLE IF NOT EXISTS clips ("
    "  id INTEGER PRIMARY KEY,"
    "  video_file TEXT NOT NULL,"
    "  thumbnail_file TEXT,"
    "  title TEXT NOT NULL DEFAULT 'Untitled',"
    "  created_at_utc TEXT NOT NULL,"
    "  source TEXT NOT NULL,"
    "  duration_seconds REAL,"
    "  game_process_name TEXT,"
    "  game_display_name TEXT,"
    "  game_source TEXT,"
    "  steam_app_id INTEGER,"
    "  confidence INTEGER,"
    "  favorite INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS clip_hashtags ("
    "  clip_id INTEGER NOT NULL,"
    "  tag TEXT NOT NULL,"
    "  PRIMARY KEY (clip_id, tag)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_clip_hashtags_tag ON clip_hashtags(tag);";

} // namespace

std::string now_iso8601_utc()
{
    std::time_t t = std::time(nullptr);
    std::tm gm{};
    gmtime_s(&gm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday,
                  gm.tm_hour, gm.tm_min, gm.tm_sec);
    return buf;
}

// ── Settings KV store ──────────────────────────────────────────────────────────

namespace {

sqlite3* open_settings_db(const std::wstring& app_data_dir, std::string* error)
{
    if (app_data_dir.empty()) {
        if (error) *error = "app data dir is empty";
        return nullptr;
    }
    std::error_code ec;
    fs::create_directories(app_data_dir, ec);
    const std::wstring path = app_data_dir + L"\\settings.db";

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(wide_to_utf8(path).c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            != SQLITE_OK) {
        if (error) *error = std::string("cannot open settings.db: ")
                            + (db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        return nullptr;
    }
    exec(db, "PRAGMA journal_mode=WAL;", nullptr);
    exec(db, "PRAGMA synchronous=NORMAL;", nullptr);
    exec(db, "PRAGMA busy_timeout=4000;", nullptr);
    if (!exec(db,
            "CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");", error)) {
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

} // namespace

bool settings_get_all(const std::wstring& app_data_dir,
                      std::vector<std::pair<std::string, std::string>>& out,
                      std::string* error)
{
    out.clear();
    sqlite3* db = open_settings_db(app_data_dir, error);
    if (!db) return false;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM settings", -1, &st, nullptr)
            != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* k = sqlite3_column_text(st, 0);
        const unsigned char* v = sqlite3_column_text(st, 1);
        out.emplace_back(k ? reinterpret_cast<const char*>(k) : "",
                         v ? reinterpret_cast<const char*>(v) : "");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool settings_replace_all(const std::wstring& app_data_dir,
                          const std::vector<std::pair<std::string, std::string>>& kv,
                          std::string* error)
{
    sqlite3* db = open_settings_db(app_data_dir, error);
    if (!db) return false;

    bool ok = exec(db, "BEGIN", error) && exec(db, "DELETE FROM settings", error);
    if (ok) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO settings (key, value) VALUES (?,?)",
                -1, &st, nullptr) == SQLITE_OK) {
            for (const auto& [k, v] : kv) {
                sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) != SQLITE_DONE) {
                    ok = false;
                    if (error) *error = sqlite3_errmsg(db);
                    break;
                }
                sqlite3_reset(st);
            }
            sqlite3_finalize(st);
        } else {
            ok = false;
            if (error) *error = sqlite3_errmsg(db);
        }
    }
    exec(db, ok ? "COMMIT" : "ROLLBACK", nullptr);
    sqlite3_close(db);
    return ok;
}

// ── Impl ───────────────────────────────────────────────────────────────────────

struct ClipDb::Impl {
    sqlite3*     db = nullptr;
    std::wstring folder;
    std::wstring db_path;

    ~Impl() { if (db) sqlite3_close(db); }

    std::wstring thumbs_dir() const { return folder + L"\\.thumbs"; }
    std::wstring video_path(const std::wstring& basename) const {
        return folder + L"\\" + basename;
    }
    std::wstring thumb_path(const std::wstring& basename) const {
        return thumbs_dir() + L"\\" + basename;
    }
};

ClipDb::ClipDb() : impl_(std::make_unique<Impl>()) {}
ClipDb::~ClipDb() = default;

std::unique_ptr<ClipDb> ClipDb::open(const std::wstring& folder,
                                     const std::string& source,
                                     std::string* error)
{
    if (folder.empty()) {
        if (error) *error = "empty output folder";
        return nullptr;
    }

    const std::wstring db_name = (source == "manual") ? L"recs.db" : L"clips.db";
    const std::wstring db_path = folder + L"\\" + db_name;

    std::error_code ec;
    fs::create_directories(folder, ec);
    const bool existed = fs::exists(db_path, ec);

    sqlite3* db = nullptr;
    // sqlite3 takes a UTF-8 filename on Windows and converts internally.
    if (sqlite3_open_v2(wide_to_utf8(db_path).c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            != SQLITE_OK) {
        if (error) *error = std::string("cannot open ") + wide_to_utf8(db_path)
                            + ": " + (db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        return nullptr;
    }

    exec(db, "PRAGMA journal_mode=WAL;", nullptr);
    exec(db, "PRAGMA synchronous=NORMAL;", nullptr);
    exec(db, "PRAGMA busy_timeout=4000;", nullptr);
    exec(db, "PRAGMA foreign_keys=ON;", nullptr);

    if (existed) {
        const int tables = table_count(db);
        const bool has_clips = has_column(db, "clips", "id");
        if (has_clips) {
            if (!clips_table_valid(db)) {
                if (error) *error = "existing " + wide_to_utf8(db_name)
                    + " has an unexpected schema; not overwriting";
                sqlite3_close(db);
                return nullptr;
            }
        } else if (tables > 0) {
            if (error) *error = wide_to_utf8(db_name)
                + " exists but is not a Monolith clip database; not overwriting";
            sqlite3_close(db);
            return nullptr;
        }
    }

    if (!exec(db, kCreateSchema, error)) {
        sqlite3_close(db);
        return nullptr;
    }
    // Forward-compat: add the favorite column if an older DB predates it.
    if (!has_column(db, "clips", "favorite"))
        exec(db, "ALTER TABLE clips ADD COLUMN favorite INTEGER NOT NULL DEFAULT 0;",
             nullptr);
    // Forward-compat: add the title column if an older DB predates it. Existing
    // rows get "Untitled"; the UI lets the user rename them independently of the
    // on-disk filename.
    if (!has_column(db, "clips", "title"))
        exec(db, "ALTER TABLE clips ADD COLUMN title TEXT NOT NULL DEFAULT 'Untitled';",
             nullptr);

    std::unique_ptr<ClipDb> out(new ClipDb());
    out->impl_->db      = db;
    out->impl_->folder  = folder;
    out->impl_->db_path = db_path;
    return out;
}

const std::wstring& ClipDb::folder() const { return impl_->folder; }
std::wstring ClipDb::thumbs_dir() const { return impl_->thumbs_dir(); }

int64_t ClipDb::insert_clip(const ClipRow& row, std::string* error)
{
    static const char* sql =
        "INSERT INTO clips (video_file, thumbnail_file, title, created_at_utc, source, "
        "duration_seconds, game_process_name, game_display_name, game_source, "
        "steam_app_id, confidence, favorite) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return -1;
    }

    const std::string video = wide_to_utf8(row.video_file);
    const std::string thumb = wide_to_utf8(row.thumbnail_file);
    const std::string title = row.title.empty() ? "Untitled" : row.title;
    const std::string created = row.created_at_utc.empty()
        ? now_iso8601_utc() : row.created_at_utc;

    sqlite3_bind_text(st, 1, video.c_str(), -1, SQLITE_TRANSIENT);
    if (thumb.empty()) sqlite3_bind_null(st, 2);
    else sqlite3_bind_text(st, 2, thumb.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, created.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, row.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 6, row.duration_seconds);
    sqlite3_bind_text(st, 7, row.game_process_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, row.game_display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, row.game_source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 10, row.steam_app_id);
    sqlite3_bind_int(st, 11, row.confidence);
    sqlite3_bind_int(st, 12, row.favorite ? 1 : 0);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return -1;
    }
    return sqlite3_last_insert_rowid(impl_->db);
}

bool ClipDb::remove_clip(int64_t id, bool remove_files, std::string* error)
{
    // Look up file basenames first so we can delete the files after the row.
    std::wstring video_base, thumb_base;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT video_file, thumbnail_file FROM clips WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                if (const unsigned char* v = sqlite3_column_text(st, 0))
                    video_base = utf8_to_wide(reinterpret_cast<const char*>(v));
                if (const unsigned char* t = sqlite3_column_text(st, 1))
                    thumb_base = utf8_to_wide(reinterpret_cast<const char*>(t));
            }
            sqlite3_finalize(st);
        }
    }

    if (!exec(impl_->db, "BEGIN", nullptr)) {}
    bool ok = true;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "DELETE FROM clip_hashtags WHERE clip_id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        st = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "DELETE FROM clips WHERE id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        } else {
            ok = false;
        }
    }
    exec(impl_->db, ok ? "COMMIT" : "ROLLBACK", nullptr);
    if (!ok) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }

    if (remove_files) {
        std::error_code ec;
        if (!video_base.empty())
            fs::remove(impl_->video_path(video_base), ec);
        if (thumb_base.empty() && !video_base.empty())
            thumb_base = thumb_basename_for(video_base);
        if (!thumb_base.empty())
            fs::remove(impl_->thumb_path(thumb_base), ec);
    }
    return true;
}

bool ClipDb::set_favorite(int64_t id, bool favorite, std::string* error)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "UPDATE clips SET favorite=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }
    sqlite3_bind_int(st, 1, favorite ? 1 : 0);
    sqlite3_bind_int64(st, 2, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && error) *error = sqlite3_errmsg(impl_->db);
    return ok;
}

bool ClipDb::set_title(int64_t id, const std::string& title, std::string* error)
{
    const std::string value = title.empty() ? "Untitled" : title;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "UPDATE clips SET title=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }
    sqlite3_bind_text(st, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && error) *error = sqlite3_errmsg(impl_->db);
    return ok;
}

bool ClipDb::add_hashtag(int64_t id, const std::string& tag, std::string* error)
{
    if (tag.empty()) { if (error) *error = "empty tag"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "INSERT OR IGNORE INTO clip_hashtags (clip_id, tag) VALUES (?,?)",
            -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && error) *error = sqlite3_errmsg(impl_->db);
    return ok;
}

bool ClipDb::remove_hashtag(int64_t id, const std::string& tag, std::string* error)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "DELETE FROM clip_hashtags WHERE clip_id=? AND tag=?",
            -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && error) *error = sqlite3_errmsg(impl_->db);
    return ok;
}

bool ClipDb::regenerate_thumbnail(int64_t id, std::string* error)
{
    std::wstring video_base, thumb_base;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT video_file, thumbnail_file FROM clips WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
            if (error) *error = sqlite3_errmsg(impl_->db);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            if (const unsigned char* v = sqlite3_column_text(st, 0))
                video_base = utf8_to_wide(reinterpret_cast<const char*>(v));
            if (const unsigned char* t = sqlite3_column_text(st, 1))
                thumb_base = utf8_to_wide(reinterpret_cast<const char*>(t));
        }
        sqlite3_finalize(st);
    }
    if (video_base.empty()) { if (error) *error = "clip not found"; return false; }

    std::error_code ec;
    fs::create_directories(impl_->thumbs_dir(), ec);
    const std::wstring thumb = thumb_base.empty()
        ? thumb_basename_for(video_base) : thumb_base;
    const std::wstring vpath = impl_->video_path(video_base);
    const std::wstring tpath = impl_->thumb_path(thumb);
    if (!fs::exists(vpath, ec)) { if (error) *error = "video file missing"; return false; }

    if (!encoding::generate_thumbnail(vpath, tpath)) {
        if (error) *error = "thumbnail generation failed";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "UPDATE clips SET thumbnail_file=? WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        const std::string tb = wide_to_utf8(thumb);
        sqlite3_bind_text(st, 1, tb.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}

bool ClipDb::rename_clip(int64_t id, const std::wstring& new_stem, std::string* error)
{
    // Reject path separators / extension / empty — a stem only.
    if (new_stem.empty() ||
        new_stem.find_first_of(L"\\/:*?\"<>|.") != std::wstring::npos) {
        if (error) *error = "invalid clip name";
        return false;
    }

    // Fetch current basenames.
    std::wstring old_video, old_thumb;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT video_file, thumbnail_file FROM clips WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
            if (error) *error = sqlite3_errmsg(impl_->db);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            if (const unsigned char* v = sqlite3_column_text(st, 0))
                old_video = utf8_to_wide(reinterpret_cast<const char*>(v));
            if (const unsigned char* t = sqlite3_column_text(st, 1))
                old_thumb = utf8_to_wide(reinterpret_cast<const char*>(t));
        }
        sqlite3_finalize(st);
    }
    if (old_video.empty()) { if (error) *error = "clip not found"; return false; }

    const std::wstring ext = fs::path(old_video).extension().wstring();
    const std::wstring new_video = new_stem + ext;
    const std::wstring new_thumb = new_stem + L".png";

    std::error_code ec;
    // Collision guard: refuse if target video already exists.
    if (fs::exists(impl_->video_path(new_video), ec)) {
        if (error) *error = "a clip with that name already exists";
        return false;
    }

    // Move the video first (the source of truth); bail if it fails.
    fs::rename(impl_->video_path(old_video), impl_->video_path(new_video), ec);
    if (ec) { if (error) *error = "could not rename video file"; return false; }

    // Thumbnail is best-effort: if the move fails, drop the stored name so
    // reconcile regenerates it later.
    std::wstring stored_thumb;
    const std::wstring old_thumb_name = old_thumb.empty()
        ? thumb_basename_for(old_video) : old_thumb;
    std::error_code tec;
    fs::rename(impl_->thumb_path(old_thumb_name), impl_->thumb_path(new_thumb), tec);
    if (!tec) stored_thumb = new_thumb;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "UPDATE clips SET video_file=?, thumbnail_file=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(impl_->db);
        return false;
    }
    const std::string nv = wide_to_utf8(new_video);
    const std::string nt = wide_to_utf8(stored_thumb);
    sqlite3_bind_text(st, 1, nv.c_str(), -1, SQLITE_TRANSIENT);
    if (nt.empty()) sqlite3_bind_null(st, 2);
    else sqlite3_bind_text(st, 2, nt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && error) *error = sqlite3_errmsg(impl_->db);
    return ok;
}

ReconcileStats ClipDb::reconcile(std::string* error)
{
    ReconcileStats stats;
    std::error_code ec;
    fs::create_directories(impl_->thumbs_dir(), ec);

    // Snapshot rows first so we can mutate without invalidating a live cursor.
    struct Row { int64_t id; std::wstring video; std::wstring thumb; };
    std::vector<Row> rows;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT id, video_file, thumbnail_file FROM clips",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                Row r;
                r.id = sqlite3_column_int64(st, 0);
                if (const unsigned char* v = sqlite3_column_text(st, 1))
                    r.video = utf8_to_wide(reinterpret_cast<const char*>(v));
                if (const unsigned char* t = sqlite3_column_text(st, 2))
                    r.thumb = utf8_to_wide(reinterpret_cast<const char*>(t));
                rows.push_back(std::move(r));
            }
            sqlite3_finalize(st);
        } else if (error) {
            *error = sqlite3_errmsg(impl_->db);
        }
    }

    std::unordered_set<std::wstring> known;
    for (auto& r : rows) {
        const std::wstring vpath = impl_->video_path(r.video);
        if (!fs::exists(vpath, ec)) {
            if (remove_clip(r.id, /*remove_files=*/true, nullptr))
                stats.removed++;
            continue;
        }
        known.insert(r.video);

        std::wstring thumb = r.thumb.empty() ? thumb_basename_for(r.video) : r.thumb;
        const std::wstring tpath = impl_->thumb_path(thumb);
        if (!fs::exists(tpath, ec)) {
            if (encoding::generate_thumbnail(vpath, tpath)) {
                // persist the (possibly new) thumbnail basename
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(impl_->db,
                        "UPDATE clips SET thumbnail_file=? WHERE id=?",
                        -1, &st, nullptr) == SQLITE_OK) {
                    const std::string tb = wide_to_utf8(thumb);
                    sqlite3_bind_text(st, 1, tb.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(st, 2, r.id);
                    sqlite3_step(st);
                    sqlite3_finalize(st);
                }
                stats.thumbs_regenerated++;
            }
        }
    }

    // Import orphan video files (migration of clips created before the DB).
    if (fs::is_directory(impl_->folder, ec)) {
        for (const auto& entry : fs::directory_iterator(impl_->folder, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const fs::path& p = entry.path();
            if (!is_video_ext(p)) continue;
            const std::wstring base = p.filename().wstring();
            if (known.count(base)) continue;

            const std::wstring thumb = thumb_basename_for(base);
            std::wstring thumb_stored;
            if (encoding::generate_thumbnail(impl_->video_path(base),
                                             impl_->thumb_path(thumb)))
                thumb_stored = thumb;

            ClipRow row;
            row.video_file     = base;
            row.thumbnail_file = thumb_stored;
            row.created_at_utc = now_iso8601_utc();
            row.source         = (impl_->db_path.find(L"recs.db") != std::wstring::npos)
                                   ? "manual" : "replay";
            if (insert_clip(row, nullptr) > 0)
                stats.imported++;
        }
    }
    return stats;
}

} // namespace storage
