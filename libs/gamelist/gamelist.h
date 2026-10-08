#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Detection uses a local Discord-derived database. Refresh it separately from
// process polling; records may share an executable basename.
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

// Initialize once; app_data_dir is supplied by the host.
void init(const std::wstring& app_data_dir);

// Read the current in-memory snapshot; it may be empty before initial sync.
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
