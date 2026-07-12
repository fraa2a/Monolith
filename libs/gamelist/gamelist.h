#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Local, DB-backed Discord game list. Detection is gated on membership in this
// list: a running process is only treated as a "game" when its executable
// basename appears here. Owned by the recorder engine so detection works
// headless (no UI process required).
//
// Source: the Discord public detectable endpoint
//   https://discord.com/api/v10/applications/detectable
// which returns the games/apps Discord recognizes, each with the executable
// name(s) that identify it. We keep the win32 executables mapped to the entry's
// display name and Discord application id.
//
// Persisted to %LocalAppData%\Monolith\game_list.db. Refreshed on startup, then
// every 72h (and immediately if the DB is missing/empty at check time). All
// network + disk work happens on a background worker thread; readers only touch
// an in-memory snapshot, so the tray/UI message loop never blocks.
namespace gamelist {

struct GameEntry {
    std::string display_name;    // Discord entry `name`, e.g. "Counter-Strike 2"
    std::string discord_app_id;  // Discord `id` (used later for artwork lookup)
};

// One executable can be shared by many games (e.g. Minecraft, Spiral Knights and
// dozens of other Java games all run as javaw.exe). We therefore keep every game
// that maps to a given exe and let the caller disambiguate (by window title).
using GameList = std::vector<GameEntry>;

// exe basename, lowercased UTF-8 (e.g. "cs2.exe") -> all games using that exe.
using GameMap = std::unordered_map<std::string, GameList>;

// Optional diagnostic sink (tag, message) so the host can route sync/DB
// failures into its own always-on error log. Set once before init().
void set_log_sink(std::function<void(const char* tag, const char* msg)> sink);

// Loads the on-disk DB into memory and starts the refresh worker. `app_data_dir`
// is %LocalAppData%\Monolith (resolved by the caller). Safe to call once.
void init(const std::wstring& app_data_dir);

// O(1), lock-free-for-readers snapshot of the current map. Never blocks on disk
// or network — this is what the detection poll calls every tick. May be empty
// before the first successful sync.
std::shared_ptr<const GameMap> snapshot();

// Convenience membership test against the current snapshot.
bool contains(const std::string& exe_basename_lower);

// Looks up all games registered for an exe in the current snapshot; returns
// false when absent. `out` receives every game sharing that executable.
bool lookup(const std::string& exe_basename_lower, GameList* out);

// Wakes the worker to refresh now. `force` re-downloads even if not yet stale.
void request_refresh(bool force);

// Number of entries in the current snapshot.
size_t size();

// Signals the worker to stop and joins it. Call from WM_DESTROY.
void shutdown();

} // namespace gamelist
