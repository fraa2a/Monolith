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
};

// A UI-driven mutation of a clip catalog row. Dispatched to the recorder (the
// single writer) so the Deno UI never writes the DB concurrently. `method` is
// one of: "clip_set_favorite", "clip_add_hashtag", "clip_remove_hashtag",
// "clip_delete". `source` picks the DB ("replay" -> clips.db, "manual" ->
// recs.db).
struct ClipMutation {
    std::string method;
    std::string source;   // "replay" | "manual"
    int64_t     id = 0;
    std::string tag;      // add/remove_hashtag
    bool        favorite = false; // set_favorite
    std::string new_name; // clip_rename (stem, no extension)
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
