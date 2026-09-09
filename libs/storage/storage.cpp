#include "storage.h"

#include <encoding/encoding.h>
#include <platform-win/platform_win.h>

#include <sqlite3.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace storage {
namespace {

using platform_win::utf8_to_wide;
using platform_win::wide_to_utf8;

bool is_video_ext(const fs::path &p) {
  std::wstring ext = p.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  return ext == L".mp4" || ext == L".mkv";
}

std::wstring thumb_basename_for(const std::wstring &video_basename) {
  return fs::path(video_basename).stem().wstring() + L".png";
}

// Column set every valid Monolith clip DB must expose.
bool clips_table_valid(sqlite3 *db) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA table_info(clips)", -1, &st, nullptr) !=
      SQLITE_OK)
    return false;
  std::unordered_set<std::string> cols;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(st, 1);
    if (name)
      cols.insert(reinterpret_cast<const char *>(name));
  }
  sqlite3_finalize(st);
  for (const char *req :
       {"id", "video_file", "thumbnail_file", "created_at_utc", "source"})
    if (!cols.count(req))
      return false;
  return true;
}

int table_count(sqlite3 *db) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
          "AND name NOT LIKE 'sqlite_%'",
          -1, &st, nullptr) != SQLITE_OK)
    return -1;
  int n = -1;
  if (sqlite3_step(st) == SQLITE_ROW)
    n = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return n;
}

bool has_column(sqlite3 *db, const char *table, const char *col) {
  std::string sql = std::string("PRAGMA table_info(") + table + ")";
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return false;
  bool found = false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(st, 1);
    if (name && col &&
        std::string(reinterpret_cast<const char *>(name)) == col) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(st);
  return found;
}

bool exec(sqlite3 *db, const char *sql, std::string *error) {
  char *msg = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    if (error)
      *error = msg ? msg : "sqlite exec failed";
    if (msg)
      sqlite3_free(msg);
    return false;
  }
  return true;
}

bool clip_exists(sqlite3 *db, int64_t id) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM clips WHERE id=? LIMIT 1", -1, &st,
                         nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int64(st, 1, id);
  const bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

const char *kCreateSchema =
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
    "  game_executable_path TEXT,"
    "  discord_app_id TEXT,"
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
    "CREATE INDEX IF NOT EXISTS idx_clip_hashtags_tag ON clip_hashtags(tag);"
    "CREATE TABLE IF NOT EXISTS clip_bookmarks ("
    "  clip_id INTEGER NOT NULL,"
    "  seq INTEGER NOT NULL,"
    "  time_seconds REAL NOT NULL,"
    "  label TEXT NOT NULL,"
    "  color TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY (clip_id, seq)"
    ");";

} // namespace

std::string now_iso8601_utc() {
  std::time_t t = std::time(nullptr);
  std::tm gm{};
  gmtime_s(&gm, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday, gm.tm_hour,
                gm.tm_min, gm.tm_sec);
  return buf;
}

// ── Settings KV store
// ──────────────────────────────────────────────────────────

namespace {

sqlite3 *open_settings_db(const std::wstring &app_data_dir,
                          std::string *error) {
  if (app_data_dir.empty()) {
    if (error)
      *error = "app data dir is empty";
    return nullptr;
  }
  std::error_code ec;
  fs::create_directories(app_data_dir, ec);
  const std::wstring path = app_data_dir + L"\\settings.db";

  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(wide_to_utf8(path).c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    if (error)
      *error = std::string("cannot open settings.db: ") +
               (db ? sqlite3_errmsg(db) : "unknown");
    if (db)
      sqlite3_close(db);
    return nullptr;
  }
  exec(db, "PRAGMA journal_mode=WAL;", nullptr);
  exec(db, "PRAGMA synchronous=NORMAL;", nullptr);
  exec(db, "PRAGMA busy_timeout=4000;", nullptr);
  if (!exec(db,
            "CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");",
            error)) {
    sqlite3_close(db);
    return nullptr;
  }
  return db;
}

} // namespace

bool settings_get_all(const std::wstring &app_data_dir,
                      std::vector<std::pair<std::string, std::string>> &out,
                      std::string *error) {
  out.clear();
  sqlite3 *db = open_settings_db(app_data_dir, error);
  if (!db)
    return false;

  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, value FROM settings", -1, &st,
                         nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(db);
    sqlite3_close(db);
    return false;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *k = sqlite3_column_text(st, 0);
    const unsigned char *v = sqlite3_column_text(st, 1);
    out.emplace_back(k ? reinterpret_cast<const char *>(k) : "",
                     v ? reinterpret_cast<const char *>(v) : "");
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return true;
}

bool settings_replace_all(
    const std::wstring &app_data_dir,
    const std::vector<std::pair<std::string, std::string>> &kv,
    std::string *error) {
  sqlite3 *db = open_settings_db(app_data_dir, error);
  if (!db)
    return false;

  bool ok = exec(db, "BEGIN", error) && exec(db, "DELETE FROM settings", error);
  if (ok) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO settings (key, value) VALUES (?,?)",
                           -1, &st, nullptr) == SQLITE_OK) {
      for (const auto &[k, v] : kv) {
        sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
          ok = false;
          if (error)
            *error = sqlite3_errmsg(db);
          break;
        }
        sqlite3_reset(st);
      }
      sqlite3_finalize(st);
    } else {
      ok = false;
      if (error)
        *error = sqlite3_errmsg(db);
    }
  }
  if (ok && !exec(db, "COMMIT", error))
    ok = false;
  else if (!ok)
    exec(db, "ROLLBACK", nullptr);
  sqlite3_close(db);
  return ok;
}

// ── Durable file mutations ─────────────────────────────────────────────────
// SQLite and filesystem renames cannot share one atomic transaction.  The
// journal records only leaf names and exact quarantine names so recovery can
// compensate a prepared operation without guessing at arbitrary paths.
namespace {
struct Mutation {
  std::string token, op, stage;
  int64_t id = 0;
  std::wstring old_v, new_v, old_t, new_t;
};
std::mutex g_mutation_map_guard;
std::unordered_map<std::wstring, std::weak_ptr<std::recursive_mutex>>
    g_mutation_map;

bool leaf(const std::wstring &s) {
  return !s.empty() && fs::path(s).filename() == fs::path(s) &&
         s.find_first_of(L"\\/:") == std::wstring::npos &&
         s.find(L'\0') == std::wstring::npos;
}
bool optional_leaf(const std::wstring &s) { return s.empty() || leaf(s); }
bool token_ok(const std::string &s) {
  if (s.size() < 16 || s.size() > 80)
    return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-';
  });
}
std::wstring quarantine_dir(const std::wstring &folder) {
  return folder + L"\\.monolith-mutations";
}
bool no_replace_move(const std::wstring &from, const std::wstring &to) {
#ifdef _WIN32
  return MoveFileW(from.c_str(), to.c_str()) != FALSE;
#else
  std::error_code ec;
  if (fs::exists(to, ec) || ec)
    return false;
  fs::rename(from, to, ec);
  return !ec;
#endif
}
bool path_exists(const std::wstring &p) {
  std::error_code ec;
  return fs::exists(p, ec) && !ec;
}
std::string new_token() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::ostringstream out;
  out << std::hex << rng() << rng();
  return out.str();
}
class MutationLock {
public:
  explicit MutationLock(const std::wstring &db_path) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(db_path, ec);
    key_ = ec ? db_path : canonical.wstring();
    {
      std::lock_guard guard(g_mutation_map_guard);
      auto &weak = g_mutation_map[key_];
      local_ = weak.lock();
      if (!local_) {
        local_ = std::make_shared<std::recursive_mutex>();
        weak = local_;
      }
    }
    local_guard_ = std::unique_lock<std::recursive_mutex>(*local_);
#ifdef _WIN32
    const auto h = std::hash<std::wstring>{}(key_);
    mutex_ =
        CreateMutexW(nullptr, FALSE,
                     (L"Local\\MonolithStorage-" + std::to_wstring(h)).c_str());
    if (!mutex_ || WaitForSingleObject(mutex_, INFINITE) != WAIT_OBJECT_0) {
      if (mutex_)
        CloseHandle(mutex_);
      mutex_ = nullptr;
    }
#endif
  }
  ~MutationLock() {
#ifdef _WIN32
    if (mutex_) {
      ReleaseMutex(mutex_);
      CloseHandle(mutex_);
    }
#endif
  }
  bool ok() const {
#ifdef _WIN32
    return mutex_ != nullptr;
#else
    return true;
#endif
  }

private:
  std::wstring key_;
  std::shared_ptr<std::recursive_mutex> local_;
  std::unique_lock<std::recursive_mutex> local_guard_;
#ifdef _WIN32
  HANDLE mutex_ = nullptr;
#endif
};

bool journal_schema(sqlite3 *db, std::string *error) {
  return exec(
      db,
      "CREATE TABLE IF NOT EXISTS file_mutation_journal (token TEXT PRIMARY "
      "KEY, version INTEGER NOT NULL, operation TEXT NOT NULL, clip_id INTEGER "
      "NOT NULL, old_video TEXT NOT NULL, new_video TEXT NOT NULL, old_thumb "
      "TEXT NOT NULL, new_thumb TEXT NOT NULL, stage TEXT NOT NULL);",
      error);
}
bool journal_put(sqlite3 *db, const Mutation &m, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          db, "INSERT INTO file_mutation_journal VALUES (?,?,?,?,?,?,?,?,?)",
          -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_text(st, 1, m.token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, 1);
  sqlite3_bind_text(st, 3, m.op.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, m.id);
  const std::string ov = wide_to_utf8(m.old_v), nv = wide_to_utf8(m.new_v),
                    ot = wide_to_utf8(m.old_t), nt = wide_to_utf8(m.new_t);
  sqlite3_bind_text(st, 5, ov.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, nv.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, ot.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, nt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, m.stage.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  if (!ok && error)
    *error = sqlite3_errmsg(db);
  sqlite3_finalize(st);
  return ok;
}
bool journal_stage(sqlite3 *db, const std::string &token, const char *stage,
                   std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          db, "UPDATE file_mutation_journal SET stage=? WHERE token=?", -1, &st,
          nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_text(st, 1, stage, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, token.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
  if (!ok && error)
    *error = sqlite3_errmsg(db);
  sqlite3_finalize(st);
  return ok;
}
bool journal_clear(sqlite3 *db, const std::string &token, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM file_mutation_journal WHERE token=?",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_text(st, 1, token.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  if (!ok && error)
    *error = sqlite3_errmsg(db);
  sqlite3_finalize(st);
  return ok;
}
bool valid_mutation(const Mutation &m) {
  return token_ok(m.token) && (m.op == "delete" || m.op == "rename") &&
         (m.stage == "prepared" || m.stage == "committed") && leaf(m.old_v) &&
         optional_leaf(m.old_t) && optional_leaf(m.new_v) &&
         optional_leaf(m.new_t);
}
bool delete_clip_rows(sqlite3 *db, int64_t id, std::string *error) {
  for (const char *sql : {"DELETE FROM clip_hashtags WHERE clip_id=?",
                          "DELETE FROM clip_bookmarks WHERE clip_id=?",
                          "DELETE FROM clips WHERE id=?"}) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (error)
        *error = sqlite3_errmsg(db);
      return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && error)
      *error = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    if (!ok)
      return false;
  }
  return true;
}
} // namespace

// ── Impl
// ───────────────────────────────────────────────────────────────────────

struct ClipDb::Impl {
  sqlite3 *db = nullptr;
  std::wstring folder;
  std::wstring db_path;

  ~Impl() {
    if (db)
      sqlite3_close_v2(db);
  }

  std::wstring thumbs_dir() const { return folder + L"\\.thumbs"; }
  std::wstring video_path(const std::wstring &basename) const {
    return folder + L"\\" + basename;
  }
  std::wstring thumb_path(const std::wstring &basename) const {
    return thumbs_dir() + L"\\" + basename;
  }
};

namespace {
bool row_has_video(sqlite3 *db, int64_t id, const std::wstring &video) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT video_file FROM clips WHERE id=?", -1, &st,
                         nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int64(st, 1, id);
  bool ok = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto *v = sqlite3_column_text(st, 0);
    ok = v && utf8_to_wide(reinterpret_cast<const char *>(v)) == video;
  }
  sqlite3_finalize(st);
  return ok;
}
bool recover_mutations(sqlite3 *db, const std::wstring &folder,
                       std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT "
          "token,operation,clip_id,old_video,new_video,old_thumb,new_thumb,"
          "stage,version FROM file_mutation_journal",
          -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(db);
    return false;
  }
  std::vector<Mutation> all;
  while (sqlite3_step(st) == SQLITE_ROW) {
    Mutation m;
    const auto text = [&](int n) {
      const auto *x = sqlite3_column_text(st, n);
      return x ? std::string(reinterpret_cast<const char *>(x)) : std::string();
    };
    m.token = text(0);
    m.op = text(1);
    m.id = sqlite3_column_int64(st, 2);
    m.old_v = utf8_to_wide(text(3));
    m.new_v = utf8_to_wide(text(4));
    m.old_t = utf8_to_wide(text(5));
    m.new_t = utf8_to_wide(text(6));
    m.stage = text(7);
    if (sqlite3_column_int(st, 8) != 1 || !valid_mutation(m)) {
      sqlite3_finalize(st);
      if (error)
        *error = "invalid file mutation journal entry; remove only after "
                 "manual inspection";
      return false;
    }
    all.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  for (const auto &m : all) {
    const bool deleting = m.op == "delete";
    const std::wstring vd = deleting ? quarantine_dir(folder) + L"\\" + m.new_v
                                     : folder + L"\\" + m.new_v;
    const std::wstring td =
        deleting
            ? (m.new_t.empty() ? L"" : quarantine_dir(folder) + L"\\" + m.new_t)
            : (m.new_t.empty() ? L"" : folder + L"\\.thumbs\\" + m.new_t);
    const std::wstring vs = folder + L"\\" + m.old_v;
    const std::wstring ts =
        m.old_t.empty() ? L"" : folder + L"\\.thumbs\\" + m.old_t;
    if (m.stage == "prepared") {
      if (path_exists(vs) && path_exists(vd)) {
        if (error)
          *error = "ambiguous prepared file mutation; both original and "
                   "destination exist";
        return false;
      }
      if (path_exists(vd) && !no_replace_move(vd, vs)) {
        if (error)
          *error = "cannot restore prepared video mutation";
        return false;
      }
      if (!ts.empty() && !td.empty() && path_exists(ts) && path_exists(td)) {
        if (error)
          *error = "ambiguous prepared thumbnail mutation";
        return false;
      }
      if (!ts.empty() && !td.empty() && path_exists(td) &&
          !no_replace_move(td, ts)) {
        if (error)
          *error = "cannot restore prepared thumbnail mutation";
        return false;
      }
      if (!journal_clear(db, m.token, error))
        return false;
    } else {
      if (deleting) {
        if (path_exists(vs)) {
          if (error)
            *error = "ambiguous committed delete; original video exists";
          return false;
        }
        std::error_code ec;
        if (path_exists(vd))
          fs::remove(vd, ec);
        if (!td.empty() && path_exists(td))
          fs::remove(td, ec);
        if (path_exists(vd) || (!td.empty() && path_exists(td))) {
          if (error)
            *error = "committed delete cleanup failed";
          return false;
        }
      } else {
        if (!row_has_video(db, m.id, m.new_v) || !path_exists(vd) ||
            path_exists(vs)) {
          if (error)
            *error = "committed rename state is inconsistent";
          return false;
        }
      }
      if (!journal_clear(db, m.token, error))
        return false;
    }
  }
  return true;
}
} // namespace

ClipDb::ClipDb() : impl_(std::make_unique<Impl>()) {}
ClipDb::~ClipDb() = default;

std::unique_ptr<ClipDb> ClipDb::open(const std::wstring &folder,
                                     const std::string &source,
                                     std::string *error) {
  if (folder.empty()) {
    if (error)
      *error = "empty output folder";
    return nullptr;
  }

  const std::wstring db_name = (source == "manual") ? L"recs.db" : L"clips.db";
  const std::wstring db_path = folder + L"\\" + db_name;

  std::error_code ec;
  fs::create_directories(folder, ec);
  const bool existed = fs::exists(db_path, ec);

  sqlite3 *db = nullptr;
  // sqlite3 takes a UTF-8 filename on Windows and converts internally.
  if (sqlite3_open_v2(wide_to_utf8(db_path).c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    if (error)
      *error = std::string("cannot open ") + wide_to_utf8(db_path) + ": " +
               (db ? sqlite3_errmsg(db) : "unknown");
    if (db)
      sqlite3_close(db);
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
        if (error)
          *error = "existing " + wide_to_utf8(db_name) +
                   " has an unexpected schema; not overwriting";
        sqlite3_close(db);
        return nullptr;
      }
    } else if (tables > 0) {
      if (error)
        *error = wide_to_utf8(db_name) +
                 " exists but is not a Monolith clip database; not overwriting";
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
    exec(db,
         "ALTER TABLE clips ADD COLUMN favorite INTEGER NOT NULL DEFAULT 0;",
         nullptr);
  // Forward-compat: add the title column if an older DB predates it. Existing
  // rows get "Untitled"; the UI lets the user rename them independently of the
  // on-disk filename.
  if (!has_column(db, "clips", "title"))
    exec(db,
         "ALTER TABLE clips ADD COLUMN title TEXT NOT NULL DEFAULT 'Untitled';",
         nullptr);
  if (!has_column(db, "clips", "discord_app_id"))
    exec(db, "ALTER TABLE clips ADD COLUMN discord_app_id TEXT;", nullptr);
  if (!has_column(db, "clips", "game_executable_path"))
    exec(db, "ALTER TABLE clips ADD COLUMN game_executable_path TEXT;",
         nullptr);

  MutationLock mutation_lock(db_path);
  if (!mutation_lock.ok() || !journal_schema(db, error) ||
      !recover_mutations(db, folder, error)) {
    if (error && error->empty())
      *error = "cannot acquire catalog mutation lock";
    sqlite3_close(db);
    return nullptr;
  }

  std::unique_ptr<ClipDb> out(new ClipDb());
  out->impl_->db = db;
  out->impl_->folder = folder;
  out->impl_->db_path = db_path;
  return out;
}

const std::wstring &ClipDb::folder() const { return impl_->folder; }
std::wstring ClipDb::thumbs_dir() const { return impl_->thumbs_dir(); }

int64_t ClipDb::insert_clip(const ClipRow &row, std::string *error) {
  static const char *sql =
      "INSERT INTO clips (video_file, thumbnail_file, title, created_at_utc, "
      "source, "
      "duration_seconds, game_process_name, game_display_name, "
      "game_executable_path, "
      "discord_app_id, game_source, steam_app_id, confidence, favorite) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return -1;
  }

  const std::string video = wide_to_utf8(row.video_file);
  const std::string thumb = wide_to_utf8(row.thumbnail_file);
  const std::string title = row.title.empty() ? "Untitled" : row.title;
  const std::string created =
      row.created_at_utc.empty() ? now_iso8601_utc() : row.created_at_utc;

  sqlite3_bind_text(st, 1, video.c_str(), -1, SQLITE_TRANSIENT);
  if (thumb.empty())
    sqlite3_bind_null(st, 2);
  else
    sqlite3_bind_text(st, 2, thumb.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, created.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, row.source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 6, row.duration_seconds);
  sqlite3_bind_text(st, 7, row.game_process_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, row.game_display_name.c_str(), -1, SQLITE_TRANSIENT);
  if (row.game_executable_path.empty())
    sqlite3_bind_null(st, 9);
  else
    sqlite3_bind_text(st, 9, row.game_executable_path.c_str(), -1,
                      SQLITE_TRANSIENT);
  if (row.discord_app_id.empty())
    sqlite3_bind_null(st, 10);
  else
    sqlite3_bind_text(st, 10, row.discord_app_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 11, row.game_source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 12, row.steam_app_id);
  sqlite3_bind_int(st, 13, row.confidence);
  sqlite3_bind_int(st, 14, row.favorite ? 1 : 0);

  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return -1;
  }
  return sqlite3_last_insert_rowid(impl_->db);
}

bool ClipDb::remove_clip(int64_t id, bool remove_files, std::string *error) {
  MutationLock lock(impl_->db_path);
  if (!lock.ok()) {
    if (error)
      *error = "cannot acquire catalog mutation lock";
    return false;
  }
  std::wstring video, thumb;
  sqlite3_stmt *lookup = nullptr;
  if (sqlite3_prepare_v2(
          impl_->db, "SELECT video_file,thumbnail_file FROM clips WHERE id=?",
          -1, &lookup, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(lookup, 1, id);
  if (sqlite3_step(lookup) == SQLITE_ROW) {
    const auto *v = sqlite3_column_text(lookup, 0);
    const auto *t = sqlite3_column_text(lookup, 1);
    if (v)
      video = utf8_to_wide(reinterpret_cast<const char *>(v));
    if (t)
      thumb = utf8_to_wide(reinterpret_cast<const char *>(t));
  }
  sqlite3_finalize(lookup);
  if (video.empty() || !leaf(video) || !optional_leaf(thumb)) {
    if (error)
      *error = "clip not found or has invalid catalog paths";
    return false;
  }
  if (!remove_files) {
    if (!exec(impl_->db, "BEGIN", error) ||
        !delete_clip_rows(impl_->db, id, error) ||
        !exec(impl_->db, "COMMIT", error)) {
      exec(impl_->db, "ROLLBACK", nullptr);
      return false;
    }
    return true;
  }
  const std::wstring source_video = impl_->video_path(video);
  if (!path_exists(source_video)) { // Reconcile's missing-video case: rows
                                    // only, no quarantine required.
    if (!exec(impl_->db, "BEGIN", error) ||
        !delete_clip_rows(impl_->db, id, error) ||
        !exec(impl_->db, "COMMIT", error)) {
      exec(impl_->db, "ROLLBACK", nullptr);
      return false;
    }
    return true;
  }
  if (thumb.empty())
    thumb = thumb_basename_for(video);
  const bool have_thumb = path_exists(impl_->thumb_path(thumb));
  Mutation m{new_token(), "delete", "prepared",
             id,          video,    utf8_to_wide(new_token()),
             thumb,       L""};
  // Use one token-derived, leaf-only destination per owned source.
  m.new_v = utf8_to_wide(m.token + "-video");
  if (have_thumb)
    m.new_t = utf8_to_wide(m.token + "-thumb");
  else
    m.old_t.clear();
  std::error_code ec;
  fs::create_directories(quarantine_dir(impl_->folder), ec);
  if (ec) {
    if (error)
      *error = "cannot create mutation quarantine";
    return false;
  }
  if (!journal_put(impl_->db, m, error))
    return false;
  const std::wstring qv = quarantine_dir(impl_->folder) + L"\\" + m.new_v;
  if (!no_replace_move(source_video, qv)) {
    journal_clear(impl_->db, m.token, nullptr);
    if (error)
      *error = "could not quarantine video without overwrite";
    return false;
  }
  if (have_thumb &&
      !no_replace_move(impl_->thumb_path(thumb),
                       quarantine_dir(impl_->folder) + L"\\" + m.new_t)) {
    if (!no_replace_move(qv, source_video)) {
      if (error)
        *error = "thumbnail move failed and video restore failed";
      return false;
    }
    journal_clear(impl_->db, m.token, nullptr);
    if (error)
      *error = "could not quarantine thumbnail without overwrite";
    return false;
  }
  if (!exec(impl_->db, "BEGIN", error) ||
      !delete_clip_rows(impl_->db, id, error) ||
      !journal_stage(impl_->db, m.token, "committed", error) ||
      !exec(impl_->db, "COMMIT", error)) {
    exec(impl_->db, "ROLLBACK", nullptr);
    if (error && error->empty())
      *error = "catalog delete transaction failed; recovery required";
    return false;
  }
  std::error_code cleanup;
  fs::remove(qv, cleanup);
  if (have_thumb)
    fs::remove(quarantine_dir(impl_->folder) + L"\\" + m.new_t, cleanup);
  if (path_exists(qv) ||
      (have_thumb &&
       path_exists(quarantine_dir(impl_->folder) + L"\\" + m.new_t))) {
    if (error)
      *error = "delete committed; quarantine cleanup deferred";
    return false;
  }
  return journal_clear(impl_->db, m.token, error);
}

bool ClipDb::set_favorite(int64_t id, bool favorite, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, "UPDATE clips SET favorite=? WHERE id=?",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int(st, 1, favorite ? 1 : 0);
  sqlite3_bind_int64(st, 2, id);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  const int changed = sqlite3_changes(impl_->db);
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  if (ok && changed == 0) {
    if (error)
      *error = "clip not found";
    return false;
  }
  return ok;
}

bool ClipDb::set_duration(int64_t id, double seconds, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "UPDATE clips SET duration_seconds=? WHERE id=?", -1,
                         &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_double(st, 1, seconds);
  sqlite3_bind_int64(st, 2, id);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  const int changed = sqlite3_changes(impl_->db);
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  if (ok && changed == 0) {
    if (error)
      *error = "clip not found";
    return false;
  }
  return ok;
}

std::wstring ClipDb::video_file_for(int64_t id) const {
  std::wstring result;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, "SELECT video_file FROM clips WHERE id=?",
                         -1, &st, nullptr) != SQLITE_OK)
    return result;
  sqlite3_bind_int64(st, 1, id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    if (const unsigned char *v = sqlite3_column_text(st, 0))
      result = utf8_to_wide(reinterpret_cast<const char *>(v));
  }
  sqlite3_finalize(st);
  return result;
}

bool ClipDb::add_bookmark(int64_t id, int seq, double time_seconds,
                          const std::string &label, const std::string &color,
                          std::string *error) {
  if (!clip_exists(impl_->db, id)) {
    if (error)
      *error = "clip not found";
    return false;
  }
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "INSERT OR REPLACE INTO clip_bookmarks (clip_id, seq, "
                         "time_seconds, label, color) "
                         "VALUES (?,?,?,?,?)",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int(st, 2, seq);
  sqlite3_bind_double(st, 3, time_seconds);
  sqlite3_bind_text(st, 4, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, color.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  return ok;
}

bool ClipDb::update_bookmark(int64_t id, int seq, const std::string &label,
                             const std::string &color, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "UPDATE clip_bookmarks SET label=?, color=? WHERE "
                         "clip_id=? AND seq=?",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_text(st, 1, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, color.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, id);
  sqlite3_bind_int(st, 4, seq);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  const int changed = sqlite3_changes(impl_->db);
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  if (ok && changed == 0) {
    if (error)
      *error = "bookmark not found";
    return false;
  }
  return ok;
}

bool ClipDb::remove_bookmark(int64_t id, int seq, std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "DELETE FROM clip_bookmarks WHERE clip_id=? AND seq=?",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int(st, 2, seq);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  return ok;
}

bool ClipDb::set_bookmark_time(int64_t id, int seq, double time_seconds,
                               std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          impl_->db,
          "UPDATE clip_bookmarks SET time_seconds=? WHERE clip_id=? AND seq=?",
          -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_double(st, 1, time_seconds);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_int(st, 3, seq);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  const int changed = sqlite3_changes(impl_->db);
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  if (ok && changed == 0) {
    if (error)
      *error = "bookmark not found";
    return false;
  }
  return ok;
}

bool ClipDb::list_bookmarks(int64_t id, std::vector<BookmarkRow> &out,
                            std::string *error) const {
  out.clear();
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          impl_->db,
          "SELECT seq, time_seconds, label, color FROM clip_bookmarks "
          "WHERE clip_id=? ORDER BY seq",
          -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    BookmarkRow row;
    row.seq = sqlite3_column_int(st, 0);
    row.time_seconds = sqlite3_column_double(st, 1);
    if (const unsigned char *v = sqlite3_column_text(st, 2))
      row.label = reinterpret_cast<const char *>(v);
    if (const unsigned char *c = sqlite3_column_text(st, 3))
      row.color = reinterpret_cast<const char *>(c);
    out.push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
}

bool ClipDb::set_title(int64_t id, const std::string &title,
                       std::string *error) {
  const std::string value = title.empty() ? "Untitled" : title;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, "UPDATE clips SET title=? WHERE id=?", -1,
                         &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_text(st, 1, value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  const int changed = sqlite3_changes(impl_->db);
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  if (ok && changed == 0) {
    if (error)
      *error = "clip not found";
    return false;
  }
  return ok;
}

bool ClipDb::add_hashtag(int64_t id, const std::string &tag,
                         std::string *error) {
  if (tag.empty()) {
    if (error)
      *error = "empty tag";
    return false;
  }
  if (!clip_exists(impl_->db, id)) {
    if (error)
      *error = "clip not found";
    return false;
  }
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          impl_->db,
          "INSERT OR IGNORE INTO clip_hashtags (clip_id, tag) VALUES (?,?)", -1,
          &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  return ok;
}

bool ClipDb::remove_hashtag(int64_t id, const std::string &tag,
                            std::string *error) {
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "DELETE FROM clip_hashtags WHERE clip_id=? AND tag=?",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok && error)
    *error = sqlite3_errmsg(impl_->db);
  return ok;
}

bool ClipDb::regenerate_thumbnail(int64_t id, std::string *error) {
  std::wstring video_base, thumb_base;
  {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(
            impl_->db,
            "SELECT video_file, thumbnail_file FROM clips WHERE id=?", -1, &st,
            nullptr) != SQLITE_OK) {
      if (error)
        *error = sqlite3_errmsg(impl_->db);
      return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
      if (const unsigned char *v = sqlite3_column_text(st, 0))
        video_base = utf8_to_wide(reinterpret_cast<const char *>(v));
      if (const unsigned char *t = sqlite3_column_text(st, 1))
        thumb_base = utf8_to_wide(reinterpret_cast<const char *>(t));
    }
    sqlite3_finalize(st);
  }
  if (video_base.empty()) {
    if (error)
      *error = "clip not found";
    return false;
  }

  std::error_code ec;
  fs::create_directories(impl_->thumbs_dir(), ec);
  const std::wstring thumb =
      thumb_base.empty() ? thumb_basename_for(video_base) : thumb_base;
  const std::wstring vpath = impl_->video_path(video_base);
  const std::wstring tpath = impl_->thumb_path(thumb);
  if (!fs::exists(vpath, ec)) {
    if (error)
      *error = "video file missing";
    return false;
  }

  if (!encoding::generate_thumbnail(vpath, tpath)) {
    if (error)
      *error = "thumbnail generation failed";
    return false;
  }

  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "UPDATE clips SET thumbnail_file=? WHERE id=?", -1,
                         &st, nullptr) == SQLITE_OK) {
    const std::string tb = wide_to_utf8(thumb);
    sqlite3_bind_text(st, 1, tb.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  return true;
}

bool ClipDb::rename_clip(int64_t id, const std::wstring &new_stem,
                         std::string *error) {
  if (new_stem.empty() ||
      new_stem.find_first_of(L"\\/:*?\"<>|.") != std::wstring::npos) {
    if (error)
      *error = "invalid clip name";
    return false;
  }
  MutationLock lock(impl_->db_path);
  if (!lock.ok()) {
    if (error)
      *error = "cannot acquire catalog mutation lock";
    return false;
  }
  std::wstring old_v, old_t;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(
          impl_->db, "SELECT video_file,thumbnail_file FROM clips WHERE id=?",
          -1, &st, nullptr) != SQLITE_OK) {
    if (error)
      *error = sqlite3_errmsg(impl_->db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto *v = sqlite3_column_text(st, 0);
    const auto *t = sqlite3_column_text(st, 1);
    if (v)
      old_v = utf8_to_wide(reinterpret_cast<const char *>(v));
    if (t)
      old_t = utf8_to_wide(reinterpret_cast<const char *>(t));
  }
  sqlite3_finalize(st);
  if (!leaf(old_v) || !optional_leaf(old_t)) {
    if (error)
      *error = "clip not found or has invalid catalog paths";
    return false;
  }
  const std::wstring new_v = new_stem + fs::path(old_v).extension().wstring();
  const std::wstring old_thumb =
      old_t.empty() ? thumb_basename_for(old_v) : old_t;
  const bool have_thumb = path_exists(impl_->thumb_path(old_thumb));
  const std::wstring new_t = have_thumb ? new_stem + L".png" : L"";
  if (path_exists(impl_->video_path(new_v)) ||
      (!new_t.empty() && path_exists(impl_->thumb_path(new_t)))) {
    if (error)
      *error = "a clip with that name already exists";
    return false;
  }
  Mutation m{new_token(),
             "rename",
             "prepared",
             id,
             old_v,
             new_v,
             have_thumb ? old_thumb : L"",
             new_t};
  if (!journal_put(impl_->db, m, error))
    return false;
  if (!no_replace_move(impl_->video_path(old_v), impl_->video_path(new_v))) {
    journal_clear(impl_->db, m.token, nullptr);
    if (error)
      *error = "could not rename video without overwrite";
    return false;
  }
  if (have_thumb && !no_replace_move(impl_->thumb_path(old_thumb),
                                     impl_->thumb_path(new_t))) {
    if (!no_replace_move(impl_->video_path(new_v), impl_->video_path(old_v))) {
      if (error)
        *error = "thumbnail rename failed and video restore failed";
      return false;
    }
    journal_clear(impl_->db, m.token, nullptr);
    if (error)
      *error = "could not rename thumbnail without overwrite";
    return false;
  }
  if (!exec(impl_->db, "BEGIN", error)) {
    return false;
  }
  st = nullptr;
  bool ok = sqlite3_prepare_v2(
                impl_->db,
                "UPDATE clips SET video_file=?,thumbnail_file=? WHERE id=?", -1,
                &st, nullptr) == SQLITE_OK;
  if (ok) {
    auto v = wide_to_utf8(new_v), t = wide_to_utf8(new_t);
    sqlite3_bind_text(st, 1, v.c_str(), -1, SQLITE_TRANSIENT);
    if (t.empty())
      sqlite3_bind_null(st, 2);
    else
      sqlite3_bind_text(st, 2, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(impl_->db) == 1;
    if (!ok && error)
      *error = sqlite3_errmsg(impl_->db);
    sqlite3_finalize(st);
  }
  if (!ok || !journal_stage(impl_->db, m.token, "committed", error) ||
      !exec(impl_->db, "COMMIT", error)) {
    exec(impl_->db, "ROLLBACK", nullptr);
    return false;
  }
  return journal_clear(impl_->db, m.token, error);
}

ReconcileStats ClipDb::reconcile(std::string *error) {
  MutationLock lock(impl_->db_path);
  if (!lock.ok()) {
    if (error)
      *error = "cannot acquire catalog mutation lock";
    return {};
  }
  ReconcileStats stats;
  std::error_code ec;
  fs::create_directories(impl_->thumbs_dir(), ec);

  // Snapshot rows first so we can mutate without invalidating a live cursor.
  struct Row {
    int64_t id;
    std::wstring video;
    std::wstring thumb;
  };
  std::vector<Row> rows;
  {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
                           "SELECT id, video_file, thumbnail_file FROM clips",
                           -1, &st, nullptr) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW) {
        Row r;
        r.id = sqlite3_column_int64(st, 0);
        if (const unsigned char *v = sqlite3_column_text(st, 1))
          r.video = utf8_to_wide(reinterpret_cast<const char *>(v));
        if (const unsigned char *t = sqlite3_column_text(st, 2))
          r.thumb = utf8_to_wide(reinterpret_cast<const char *>(t));
        rows.push_back(std::move(r));
      }
      sqlite3_finalize(st);
    } else if (error) {
      *error = sqlite3_errmsg(impl_->db);
    }
  }

  std::unordered_set<std::wstring> known;
  for (auto &r : rows) {
    const std::wstring vpath = impl_->video_path(r.video);
    if (!fs::exists(vpath, ec)) {
      if (remove_clip(r.id, /*remove_files=*/true, nullptr))
        stats.removed++;
      continue;
    }
    known.insert(r.video);

    std::wstring thumb =
        r.thumb.empty() ? thumb_basename_for(r.video) : r.thumb;
    const std::wstring tpath = impl_->thumb_path(thumb);
    if (!fs::exists(tpath, ec)) {
      if (encoding::generate_thumbnail(vpath, tpath)) {
        // persist the (possibly new) thumbnail basename
        sqlite3_stmt *st = nullptr;
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
    for (const auto &entry : fs::directory_iterator(impl_->folder, ec)) {
      if (!entry.is_regular_file(ec))
        continue;
      const fs::path &p = entry.path();
      if (!is_video_ext(p))
        continue;
      const std::wstring base = p.filename().wstring();
      if (known.count(base))
        continue;

      const std::wstring thumb = thumb_basename_for(base);
      std::wstring thumb_stored;
      if (encoding::generate_thumbnail(impl_->video_path(base),
                                       impl_->thumb_path(thumb)))
        thumb_stored = thumb;

      ClipRow row;
      row.video_file = base;
      row.thumbnail_file = thumb_stored;
      row.created_at_utc = now_iso8601_utc();
      row.source = (impl_->db_path.find(L"recs.db") != std::wstring::npos)
                       ? "manual"
                       : "replay";
      if (insert_clip(row, nullptr) > 0)
        stats.imported++;
    }
  }
  return stats;
}

} // namespace storage
