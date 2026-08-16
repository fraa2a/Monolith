#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace ipc {

struct RecordingState {
    bool is_recording;
    bool is_paused;
    bool replay_enabled;
    bool recording_enabled;
    // Monotonically increasing counter bumped each time a clip is cataloged
    // (replay save or manual-recording stop). The UI host polls get_status and
    // pushes a live refresh to the webview whenever this value changes, so
    // the clip library updates in real time without an app restart.
    uint64_t clip_generation = 0;
    // Engine component version (MONOLITH_VERSION_STRING). Surfaced through
    // get_status so the Settings UI can show engine + interface versions
    // side by side — the two are versioned independently.
    std::string version;
};

// A UI-driven mutation of a clip catalog row. Dispatched to the recorder (the
// single writer) so the UI never writes the DB concurrently. `method` is one of:
// "clip_set_favorite", "clip_add_hashtag", "clip_remove_hashtag", "clip_rename",
// "clip_set_title", "clip_delete", "clip_trim". `source` picks the DB
// ("replay" -> clips.db, "manual" -> recs.db).
struct ClipMutation {
    std::string method;
    std::string source;   // "replay" | "manual"
    int64_t     id = 0;
    std::string tag;      // add/remove_hashtag
    bool        favorite = false; // set_favorite
    std::string new_name; // clip_rename (stem, no extension)
    std::string title;    // clip_set_title (display name, independent of file)
    double      start = 0.0; // clip_trim: trim window start (seconds)
    double      end   = 0.0; // clip_trim: trim window end (seconds)
};

// Returns "" on success, or a human-readable error message on failure.
using ClipMutationFn = std::function<std::string(const ClipMutation&)>;

// Adds a bookmark at the current recording position of the running manual
// recording. Called on an IPC client thread (timestamp accuracy matters, so it
// must NOT round-trip through the message loop). Returns "" on success or an
// error message ("not recording", "recording is paused", ...).
using AddBookmarkFn = std::function<std::string()>;

// UI-driven selection of which detected game to record/clip when several are
// running. `exe` is the lowercased executable basename (stable across restarts);
// `pid` is an optional live hint. Empty exe + pid 0 clears the selection (auto).
// Called on an IPC client thread — the handler must only touch synchronized
// state and defer engine work to the message loop.
using SelectGameFn = std::function<void(const std::string& exe, uint32_t pid)>;

// Closes the running Monolith.UI process so Updater.exe can swap ui\* on
// disk. Called on an IPC client thread by update_close_ui; blocks until the
// UI has exited (graceful close, then terminate after a timeout) exactly
// like the engine's own shutdown path.
using UpdateCloseUiFn = std::function<void()>;

// Start the JSON-RPC TCP server on 127.0.0.1:45991.
// Recording commands (save_replay, recording_start/stop, pause_resume) are
// dispatched via PostMessage to hwnd. `status_fn` answers get_status and
// `mutation_fn` performs clip_* mutations; both may be called concurrently
// from multiple client-handler threads and must be internally thread-safe.
// `select_fn` handles set_selected_game. `add_bookmark_fn` handles
// recording_add_bookmark directly on the IPC thread. `update_close_ui_fn`
// handles update_close_ui (blocks until the UI process is gone).
// `reload_settings` posts WM_APP+2 to hwnd; `update_engine_exit` posts
// WM_CLOSE to hwnd (graceful shutdown — stops any recording first).
void start(HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn = nullptr,
           SelectGameFn select_fn = nullptr,
           AddBookmarkFn add_bookmark_fn = nullptr,
           UpdateCloseUiFn update_close_ui_fn = nullptr);
void stop();

} // namespace ipc
