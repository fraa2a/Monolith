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
    // UI hosts poll this catalog generation to request a library refresh.
    uint64_t clip_generation = 0;
    // Engine and interface component versions are independent.
    std::string version;
    bool capture_running = false;
    bool replay_running = false;
    std::string recording_error = {};
};

// Mutation source selects clips.db or recs.db. Callbacks run on IPC threads.
struct ClipMutation {
    std::string method;
    std::string source;   // "replay" | "manual"
    int64_t     id = 0;
    std::string catalog_uid, clip_uid;
    int64_t media_revision = -1;
    int seq = 0;
    double time_seconds = 0, duration = 0;
    std::string label, color, upload_token;
    std::string tag;      // add/remove_hashtag
    bool        favorite = false; // set_favorite
    std::string new_name; // clip_rename (stem, no extension)
    std::string title;    // clip_set_title (display name, independent of file)
    double      start = 0.0; // clip_trim: trim window start (seconds)
    double      end   = 0.0; // clip_trim: trim window end (seconds)
};

// Returns "" on success, or a human-readable error message on failure.
using ClipMutationFn = std::function<std::string(const ClipMutation&)>;

// Timestamp bookmarks on the IPC thread; return an empty string or an error.
using AddBookmarkFn = std::function<std::string()>;

// An empty exe and zero PID clears selection. Defer engine work to the message loop.
using SelectGameFn = std::function<void(const std::string& exe, uint32_t pid)>;

// Block the IPC worker until UI exit, with graceful close then forced termination.
using UpdateCloseUiFn = std::function<void()>;

// Server limits: 16 clients, 64 KiB lines, JSON depth 64 and 30 s I/O deadlines.
// Callbacks can run concurrently and must not call start/stop. stop interrupts
// socket I/O and waits for callbacks; silent clients expire before their first request.
void start(HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn = nullptr,
           SelectGameFn select_fn = nullptr,
           AddBookmarkFn add_bookmark_fn = nullptr,
           UpdateCloseUiFn update_close_ui_fn = nullptr);
void stop();

} // namespace ipc
