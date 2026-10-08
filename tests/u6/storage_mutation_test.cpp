#include <encoding/encoding.h>
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
int main() {
  try {
    auto root = fs::temp_directory_path() / "monolith-u6-storage";
    fs::remove_all(root);
    fs::create_directories(root / ".thumbs");
    auto folder = root.wstring();
    fs::remove(fs::path(folder + L"\\clips.db"));
    fs::remove_all(fs::path(folder + L"\\.monolith-mutations"));
    std::string e;
    auto db = storage::ClipDb::open(folder, "replay", &e);
    check(db != nullptr, "open1");
    touch(folder + L"\\a.mp4");
    touch(folder + L"\\.thumbs\\a.png");
    storage::ClipRow r;
    r.video_file = L"a.mp4";
    r.thumbnail_file = L"a.png";
    r.source = "replay";
    auto id = db->insert_clip(r, &e);
    check(id > 0, e.c_str());
    check(db->rename_clip(id, L"b", &e), e.c_str());
    check(fs::exists(folder + L"\\b.mp4"), "renamed video");
    check(db->remove_clip(id, true, &e), e.c_str());
    check(!fs::exists(folder + L"\\b.mp4"), "deleted video");
    db.reset();
    // Prepared delete: video resides in exact quarantine target and is restored
    // at open.
    touch(folder + L"\\p.mp4");
    db = storage::ClipDb::open(folder, "replay", &e);
    r.video_file = L"p.mp4";
    r.thumbnail_file = L"";
    id = db->insert_clip(r, &e);
    db.reset();
    fs::create_directories(folder + L"\\.monolith-mutations");
    fs::rename(folder + L"\\p.mp4",
               folder + L"\\.monolith-mutations\\1234567890abcdef-video");
    auto path = fs::path(folder + L"\\clips.db");
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('1234567890abcdef',1,'delete',2,'p.mp4','1234567890abcdef-"
              "video','','','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "prepared delete: " << e << "\n";
    check(db != nullptr, "open2");
    check(fs::exists(folder + L"\\p.mp4"), "prepared delete restore");
    db.reset();
    // Prepared rename restores the old name; a committed delete removes only
    // its quarantine file.
    fs::rename(folder + L"\\p.mp4", folder + L"\\q.mp4");
    sql(path,
        "INSERT INTO file_mutation_journal VALUES "
        "('abcdef1234567890',1,'rename',2,'p.mp4','q.mp4','','','prepared');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "prepared rename: " << e << "\n";
    check(db != nullptr, "open3");
    check(fs::exists(folder + L"\\p.mp4"), "prepared rename restore");
    db.reset();
    touch(folder + L"\\.monolith-mutations\\fedcba0987654321-video");
    sql(path, "INSERT INTO file_mutation_journal VALUES "
              "('fedcba0987654321',1,'delete',8,'gone.mp4','fedcba0987654321-"
              "video','','','committed');");
    db = storage::ClipDb::open(folder, "replay", &e);
    if (!db)
      std::cerr << "committed: " << e << "\n";
    check(db != nullptr, "open4");
    check(
        !fs::exists(folder + L"\\.monolith-mutations\\fedcba0987654321-video"),
        "committed cleanup");
    db.reset();
    // Both paths present is ambiguous and fails closed.
    touch(folder + L"\\amb.mp4");
    touch(folder + L"\\amb-new.mp4");
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
