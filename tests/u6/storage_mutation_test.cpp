#include <encoding/encoding.h>
#include <encoding/trim.h>
#include <encoding/media_ownership.h>
#include <recording/bookmark_journal.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <platform-win/platform_win.h>
#include <sqlite3.h>
#include <stdexcept>
#include <storage/storage.h>
namespace fs = std::filesystem;
namespace platform_win {
std::wstring utf8_to_wide(const std::string &s) { return {s.begin(), s.end()}; }
std::string wide_to_utf8(const std::wstring &s) { return {s.begin(), s.end()}; }
} // namespace platform_win
namespace encoding {
bool generate_thumbnail(const std::wstring &, const std::wstring &, int) {
  return false;
}
bool trim_clip_lossless(const std::wstring& path, double start, double end,
    const std::wstring& out, std::string*, TrimResult* result) {
  fs::copy_file(path, out, fs::copy_options::overwrite_existing);
  *result = {start, end-start}; return true;
}
bool trim_clip_reencode(const std::wstring&, double, double,
    const std::wstring&, std::string*, TrimResult*) { return false; }
} // namespace encoding
static void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
static void touch(const std::wstring &p) {
  std::ofstream(fs::path(p)).put('x');
}
static void sql(const fs::path &db, const char *q) {
  sqlite3 *x = nullptr;
  check(sqlite3_open(db.string().c_str(), &x) == SQLITE_OK, "open sqlite");
  char *e = nullptr;
  check(sqlite3_exec(x, q, nullptr, nullptr, &e) == SQLITE_OK, e ? e : "sql");
  sqlite3_free(e);
  sqlite3_close(x);
}
static std::string scalar(const fs::path& path, const char* query) {
  sqlite3* db = nullptr; sqlite3_stmt* st = nullptr;
  check(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "open scalar");
  check(sqlite3_prepare_v2(db, query, -1, &st, nullptr) == SQLITE_OK, "prepare scalar");
  check(sqlite3_step(st) == SQLITE_ROW, "read scalar");
  const auto* value = sqlite3_column_text(st, 0);
  std::string result = value ? reinterpret_cast<const char*>(value) : "";
  sqlite3_finalize(st); sqlite3_close(db); return result;
}
int main() {
  try {
    auto root = fs::temp_directory_path() / "monolith-u6-storage";
    fs::remove_all(root);
    fs::create_directories(root / ".thumbs");
    auto folder = root.wstring();
    fs::remove(fs::path((root / L"clips.db").wstring()));
    fs::remove_all(fs::path((root / L".monolith-mutations").wstring()));
    std::string e;
    auto db = storage::ClipDb::open(folder, "replay", &e);
    check(db != nullptr, "open1");
    touch((root / L"a.mp4").wstring());
    touch((root / L".thumbs/a.png").wstring());
    storage::ClipRow r;
    r.video_file = L"a.mp4";
    r.thumbnail_file = L"a.png";
    r.source = "replay";
    auto id = db->insert_clip(r, &e);
    check(id > 0, e.c_str());
    check(db->rename_clip(id, L"b", &e), e.c_str());
    check(fs::exists((root / L"b.mp4").wstring()), "renamed video");
    check(db->remove_clip(id, true, &e), e.c_str());
    check(!fs::exists((root / L"b.mp4").wstring()), "deleted video");
    db.reset();
    // Prepared delete: video resides in exact quarantine target and is restored
    // at open.
    touch((root / L"p.mp4").wstring());
    db = storage::ClipDb::open(folder, "replay", &e);
    r.video_file = L"p.mp4";
    r.thumbnail_file = L"";
    id = db->insert_clip(r, &e);
    db.reset();
    fs::create_directories((root / L".monolith-mutations").wstring());
    fs::rename((root / L"p.mp4").wstring(),
               (root / L".monolith-mutations/1234567890abcdef-video").wstring());
    auto path = fs::path((root / L"clips.db").wstring());
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('1234567890abcdef',1,'delete',1,'p.mp4','1234567890abcdef-"
              "video','','','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "prepared delete: " << e << "\n";
    check(db != nullptr, "open2");
    check(fs::exists((root / L"p.mp4").wstring()), "prepared delete restore");
    db.reset();
    // Prepared rename restores the old name; a committed delete removes only
    // its quarantine file.
    fs::rename((root / L"p.mp4").wstring(), (root / L"q.mp4").wstring());
    sql(path,
        "INSERT INTO file_mutation_journal VALUES "
        "('abcdef1234567890',1,'rename',1,'p.mp4','q.mp4','','','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "prepared rename: " << e << "\n";
    check(db != nullptr, "open3");
    check(fs::exists((root / L"p.mp4").wstring()), "prepared rename restore");
    db.reset();
    touch((root / L".monolith-mutations/fedcba0987654321-video").wstring());
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('fedcba0987654321',1,'delete',8,'gone.mp4','fedcba0987654321-"
              "video','','','committed');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "committed: " << e << "\n";
    check(db != nullptr, "open4");
    check(
        !fs::exists((root / L".monolith-mutations/fedcba0987654321-video").wstring()),
        "committed cleanup");
    db.reset();
    // Both paths present is ambiguous and fails closed.
    touch((root / L"amb.mp4").wstring());
    touch((root / L"amb-new.mp4").wstring());
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('1122334455667788',1,'rename',9,'amb.mp4','amb-new.mp4','','','"
              "prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    check(!db, "ambiguous journal rejected");
    sql(path,
        "DELETE FROM file_mutation_journal WHERE token='1122334455667788';");
    // Directory aliases cannot enter catalog file mutations.
    db = storage::ClipDb::open(folder, "replay", &e);
    check(db != nullptr, "open after ambiguous recovery");
    check(!db->rename_clip(1, L"..", &e), "parent alias rejected");
    check(!db->rename_clip(1, L"trailing ", &e), "trailing space rejected");
    const auto uid = scalar(path, "SELECT value FROM catalog_metadata WHERE key='catalog_uid'");
    const auto clip_uid = scalar(path, "SELECT clip_uid FROM clips WHERE id=1");
    check(!db->mutate_verified(id, "other", clip_uid, 0, [] { return true; }, &e), "wrong catalog rejected");
    check(!db->mutate_verified(id, uid, "other", 0, [] { return true; }, &e), "wrong clip rejected");
    check(db->mutate_verified(id, uid, clip_uid, 0, [] { return true; }, &e), "matching identity accepted");
    {
      encoding::MediaWriteGuard owner((root / "p.mp4").wstring());
      check(!db->remove_clip(id, true, &e), "owned media cannot be deleted");
      check(!db->trim_clip(id, 1, 3, &e), "owned media cannot be trimmed");
    }
    check(db->add_bookmark(id, 1, 2, "kept", "red", &e), "seed bookmark");
    check(db->add_bookmark(id, 2, 8, "removed", "red", &e), "seed outside bookmark");
    check(db->trim_clip(id, 1, 4, &e), e.c_str());
    check(scalar(path, "SELECT media_revision FROM clips WHERE id=1") == "1", "trim increments revision");
    check(!db->mutate_verified(id, uid, clip_uid, 0, [] { return true; }, &e), "stale timeline rejected");
    std::vector<storage::ClipDb::BookmarkRow> bookmarks;
    check(db->list_bookmarks(id, bookmarks, &e) && bookmarks.size() == 1 && bookmarks[0].time_seconds == 1, "trim retimes bookmarks");
    check(scalar(path, "SELECT count(*) FROM file_mutation_journal") == "0", "trim cleanup");
    sql(path, "CREATE TRIGGER fail_trim BEFORE UPDATE OF duration_seconds ON clips BEGIN SELECT RAISE(ABORT,'injected'); END;");
    check(!db->trim_clip(id, 0, 1, &e), "metadata failure rejects trim");
    check(fs::exists(root / "p.mp4"), "failed trim restores original");
    check(scalar(path, "SELECT media_revision FROM clips WHERE id=1") == "1", "failed trim preserves revision");
    sql(path, "DROP TRIGGER fail_trim;");
    check(recording::write_bookmark_journal((root / "p.mp4").wstring(), {{0.5,"recovered","red"}}, &e), "write recovery sidecar");
    db->reconcile(&e);
    check(db->list_bookmarks(id, bookmarks, &e) && bookmarks.size() == 1 && bookmarks[0].label == "recovered", "recover sidecar bookmarks");
    check(!fs::exists(recording::bookmark_journal_path((root / "p.mp4").wstring())), "remove completed sidecar");
    db.reset();
    const auto work = root / ".monolith-mutations" / "aabbccddeeff0011";
    fs::create_directories(work);
    fs::rename(root / "p.mp4", work / "original.mp4");
    touch((root / "p.mp4").wstring());
    sql(path, "INSERT INTO file_mutation_journal VALUES ('aabbccddeeff0011',1,'trim',1,'p.mp4','original.mp4','','trimmed.mp4','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    check(db != nullptr && fs::exists(root / "p.mp4") && !fs::exists(work), "interrupted trim restores and cleans");
    fs::create_symlink(root / "p.mp4", root / "linked.mp4");
    r.video_file = L"linked.mp4";
    const auto linked_id = db->insert_clip(r, &e);
    check(linked_id > 0 && !db->trim_clip(linked_id, 0, 1, &e), "trim rejects symlink");
    db.reset();
    db = storage::ClipDb::open(folder, "replay", &e);
    check(db != nullptr, "open for thumbnail capture");
    fs::create_directories(root / "upload");
    const std::string token(32, 'a');
    std::ofstream(root / "upload" / (token+".png"), std::ios::binary).write("\x89PNG\r\n\x1a\n", 8);
    check(!db->capture_thumbnail(id, (root / "upload").wstring(), "../invalid", &e), "thumbnail traversal token rejected");
    check(db->capture_thumbnail(id, (root / "upload").wstring(), token, &e), e.c_str());
    check(scalar(path, "SELECT thumbnail_file FROM clips WHERE id=1").starts_with("capture-"), "captured thumbnail published");
    db.reset();
    // Invalid journal is rejected rather than interpreted.
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('bad',1,'delete',3,'../x','q','','','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    check(!db, "invalid journal rejected");
    fs::remove_all(root);
    std::cout << "u6 storage mutation tests passed\n";
  } catch (const std::exception &x) {
    std::cerr << x.what() << '\n';
    return 1;
  }
}
