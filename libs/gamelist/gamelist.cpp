#include "gamelist.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <platform-win/platform_win.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using platform_win::wide_to_utf8;

namespace gamelist {

namespace {

// Discord public detectable endpoint.
constexpr wchar_t kHost[] = L"discord.com";
constexpr wchar_t kPath[] = L"/api/v10/applications/detectable";
constexpr INTERNET_PORT kPort = INTERNET_DEFAULT_HTTPS_PORT;

// Refresh cadence and the retry backoff used when the very first sync fails and
// the DB is still empty (so we don't wait the full 72h with no data).
constexpr std::time_t kRefreshSeconds = 72 * 60 * 60;
constexpr std::time_t kEmptyRetrySeconds = 60 * 60;

// A real response has thousands of entries; anything tiny is treated as a bad
// fetch and must never overwrite a good DB.
constexpr size_t kMinPlausibleEntries = 50;

std::function<void(const char*, const char*)> g_log_sink;

void log_msg(const char* tag, const std::string& msg)
{
    if (g_log_sink) g_log_sink(tag, msg.c_str());
    OutputDebugStringA(("[gamelist] " + msg + "\n").c_str());
}

// ── Shared state ────────────────────────────────────────────────────────────

std::mutex                       g_snapshot_mutex;
std::shared_ptr<const GameMap>   g_snapshot = std::make_shared<const GameMap>();

std::wstring                     g_db_path;
std::thread                      g_worker;
std::mutex                       g_worker_mutex;
std::condition_variable          g_worker_cv;
bool                             g_stop = false;
bool                             g_force = false;
bool                             g_started = false;

void publish(std::shared_ptr<const GameMap> map)
{
    std::lock_guard<std::mutex> lk(g_snapshot_mutex);
    g_snapshot = std::move(map);
}

// ── SQLite ──────────────────────────────────────────────────────────────────

bool exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        if (err) sqlite3_free(err);
        log_msg("gamelist", std::string("sqlite exec failed: ") + msg);
        return false;
    }
    return true;
}

sqlite3* open_db()
{
    if (g_db_path.empty()) return nullptr;
    std::error_code ec;
    fs::create_directories(fs::path(g_db_path).parent_path(), ec);

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(wide_to_utf8(g_db_path).c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            != SQLITE_OK) {
        log_msg("gamelist", std::string("cannot open game_list.db: ")
                            + (db ? sqlite3_errmsg(db) : "unknown"));
        if (db) sqlite3_close(db);
        return nullptr;
    }
    exec(db, "PRAGMA journal_mode=WAL;");
    exec(db, "PRAGMA synchronous=NORMAL;");
    exec(db, "PRAGMA busy_timeout=4000;");
    if (!exec(db,
            "CREATE TABLE IF NOT EXISTS games ("
            "  exe_lower TEXT PRIMARY KEY,"
            "  display_name TEXT NOT NULL,"
            "  discord_app_id TEXT NOT NULL"
            ");")
        || !exec(db,
            "CREATE TABLE IF NOT EXISTS meta ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");")) {
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

std::shared_ptr<GameMap> load_from_db(sqlite3* db)
{
    auto map = std::make_shared<GameMap>();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT exe_lower, display_name, discord_app_id FROM games",
            -1, &st, nullptr) != SQLITE_OK)
        return map;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* exe = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        const char* app = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        if (!exe) continue;
        GameEntry e;
        e.display_name = name ? name : "";
        e.discord_app_id = app ? app : "";
        (*map)[exe] = std::move(e);
    }
    sqlite3_finalize(st);
    return map;
}

std::time_t read_last_refresh(sqlite3* db)
{
    std::time_t out = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key='last_refresh_utc'",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (v) out = static_cast<std::time_t>(std::strtoll(v, nullptr, 10));
        }
        sqlite3_finalize(st);
    }
    return out;
}

bool write_games(sqlite3* db, const GameMap& map, std::time_t now)
{
    if (!exec(db, "BEGIN IMMEDIATE;")) return false;
    if (!exec(db, "DELETE FROM games;")) { exec(db, "ROLLBACK;"); return false; }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO games (exe_lower, display_name, discord_app_id)"
            " VALUES (?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
        exec(db, "ROLLBACK;");
        return false;
    }
    for (const auto& [exe, entry] : map) {
        sqlite3_bind_text(st, 1, exe.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, entry.display_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, entry.discord_app_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            exec(db, "ROLLBACK;");
            return false;
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);

    sqlite3_stmt* ms = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO meta (key, value) VALUES ('last_refresh_utc', ?)",
            -1, &ms, nullptr) == SQLITE_OK) {
        const std::string v = std::to_string(static_cast<long long>(now));
        sqlite3_bind_text(ms, 1, v.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ms);
        sqlite3_finalize(ms);
    }
    return exec(db, "COMMIT;");
}

// ── HTTP ────────────────────────────────────────────────────────────────────

bool http_get(std::string* body)
{
    body->clear();
    HINTERNET session = WinHttpOpen(L"Monolith/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { log_msg("gamelist", "WinHttpOpen failed"); return false; }

    // resolve/connect/send/receive: 15s each.
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, kHost, kPort, 0);
    HINTERNET request = nullptr;
    if (connect) {
        request = WinHttpOpenRequest(connect, L"GET", kPath, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {

        DWORD status = 0, status_len = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) break;
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, buf.data(), avail, &read)) break;
                body->append(buf.data(), read);
            } while (avail > 0);
            ok = !body->empty();
        } else {
            log_msg("gamelist", "HTTP status " + std::to_string(status));
        }
    } else {
        log_msg("gamelist", "WinHttp request failed, error "
                            + std::to_string(GetLastError()));
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// ── Parse ───────────────────────────────────────────────────────────────────

std::string basename_lower(const std::string& name)
{
    size_t slash = name.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base;
}

bool parse_detectables(const std::string& json, GameMap* out)
{
    out->clear();
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_array()) return false;

    for (const auto& app : doc) {
        if (!app.is_object()) continue;
        const auto exes = app.find("executables");
        if (exes == app.end() || !exes->is_array()) continue;

        std::string display = app.value("name", std::string());
        std::string app_id = app.value("id", std::string());
        if (display.empty()) continue;

        for (const auto& exe : *exes) {
            if (!exe.is_object()) continue;
            if (exe.value("os", std::string()) != "win32") continue;
            // Skip bare launchers (Steam/Epic/etc.) so we detect the game, not
            // the storefront that started it.
            if (exe.value("is_launcher", false)) continue;
            const std::string raw = exe.value("name", std::string());
            if (raw.empty()) continue;
            const std::string key = basename_lower(raw);
            if (key.empty()) continue;
            // First writer wins; the list is largely unique per exe anyway.
            out->emplace(key, GameEntry{display, app_id});
        }
    }
    return true;
}

// Returns the new map on success (already published + persisted), or nullptr on
// failure (existing snapshot left intact).
bool sync_now()
{
    std::string body;
    if (!http_get(&body)) return false;

    auto fresh = std::make_shared<GameMap>();
    if (!parse_detectables(body, fresh.get())) {
        log_msg("gamelist", "detectable JSON parse failed");
        return false;
    }
    if (fresh->size() < kMinPlausibleEntries) {
        log_msg("gamelist", "refusing to apply small game list ("
                            + std::to_string(fresh->size()) + " entries)");
        return false;
    }

    sqlite3* db = open_db();
    if (!db) return false;
    const std::time_t now = std::time(nullptr);
    const bool wrote = write_games(db, *fresh, now);
    sqlite3_close(db);
    if (!wrote) {
        log_msg("gamelist", "failed to persist game list");
        return false;
    }

    publish(std::const_pointer_cast<const GameMap>(fresh));
    log_msg("gamelist", "synced " + std::to_string(fresh->size()) + " games");
    return true;
}

void worker_main()
{
    for (;;) {
        // Decide whether a sync is due.
        bool due = false;
        bool empty = false;
        {
            sqlite3* db = open_db();
            if (db) {
                auto loaded = load_from_db(db);
                empty = loaded->empty();
                if (!empty) publish(std::const_pointer_cast<const GameMap>(loaded));
                const std::time_t last = read_last_refresh(db);
                const std::time_t now = std::time(nullptr);
                due = empty || last == 0 || (now - last) >= kRefreshSeconds;
                sqlite3_close(db);
            } else {
                due = true;
                empty = true;
            }
        }

        bool forced;
        {
            std::lock_guard<std::mutex> lk(g_worker_mutex);
            forced = g_force;
            g_force = false;
            if (g_stop) return;
        }

        bool synced_ok = true;
        if (due || forced) {
            synced_ok = sync_now();
            if (synced_ok) empty = false;
        }

        // Sleep until the next refresh (shorter backoff if we still have no data).
        std::time_t wait_s = kRefreshSeconds;
        if (empty && !synced_ok) wait_s = kEmptyRetrySeconds;

        std::unique_lock<std::mutex> lk(g_worker_mutex);
        g_worker_cv.wait_for(lk, std::chrono::seconds(wait_s),
                             [] { return g_stop || g_force; });
        if (g_stop) return;
    }
}

} // namespace

void set_log_sink(std::function<void(const char* tag, const char* msg)> sink)
{
    g_log_sink = std::move(sink);
}

void init(const std::wstring& app_data_dir)
{
    if (g_started) return;
    g_started = true;
    g_db_path = app_data_dir + L"\\game_list.db";

    // Load whatever we already have synchronously so detection has data on the
    // first tick even before the network sync completes.
    if (sqlite3* db = open_db()) {
        auto loaded = load_from_db(db);
        if (!loaded->empty())
            publish(std::const_pointer_cast<const GameMap>(loaded));
        sqlite3_close(db);
    }

    g_worker = std::thread(worker_main);
}

std::shared_ptr<const GameMap> snapshot()
{
    std::lock_guard<std::mutex> lk(g_snapshot_mutex);
    return g_snapshot;
}

bool contains(const std::string& exe_basename_lower)
{
    auto snap = snapshot();
    return snap && snap->find(exe_basename_lower) != snap->end();
}

bool lookup(const std::string& exe_basename_lower, GameEntry* out)
{
    auto snap = snapshot();
    if (!snap) return false;
    auto it = snap->find(exe_basename_lower);
    if (it == snap->end()) return false;
    if (out) *out = it->second;
    return true;
}

void request_refresh(bool force)
{
    {
        std::lock_guard<std::mutex> lk(g_worker_mutex);
        g_force = g_force || force;
    }
    g_worker_cv.notify_all();
}

size_t size()
{
    auto snap = snapshot();
    return snap ? snap->size() : 0;
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_worker_mutex);
        g_stop = true;
    }
    g_worker_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_started = false;
}

} // namespace gamelist
