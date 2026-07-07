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
    // pushes a live refresh to the webview whenever this value changes, so the
    // clip library updates in real time without an app restart.
    uint64_t clip_generation = 0;
};

// A UI-driven mutation of a clip catalog row. Dispatched to the recorder (the
// single writer) so the UI never writes the DB concurrently. `method` is one of:
// "clip_set_favorite", "clip_add_hashtag", "clip_remove_hashtag", "clip_rename",
// "clip_set_title", "clip_delete". `source` picks the DB ("replay" -> clips.db,
// "manual" -> recs.db).
struct ClipMutation {
    std::string method;
    std::string source;   // "replay" | "manual"
    int64_t     id = 0;
    std::string tag;      // add/remove_hashtag
    bool        favorite = false; // set_favorite
    std::string new_name; // clip_rename (stem, no extension)
    std::string title;    // clip_set_title (display name, independent of file)
};

// Returns "" on success, or a human-readable error message on failure.
using ClipMutationFn = std::function<std::string(const ClipMutation&)>;

// Start the JSON-RPC TCP server on 127.0.0.1:45991.
// Recording commands (save_replay, recording_start/stop, pause_resume) are
// dispatched via PostMessage to hwnd. `status_fn` answers get_status and
// `mutation_fn` performs clip_* mutations; both run on the IPC thread and must
// be internally thread-safe. `reload_settings` posts WM_APP+2 to hwnd.
void start(HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn = nullptr);
void stop();

} // namespace ipc
