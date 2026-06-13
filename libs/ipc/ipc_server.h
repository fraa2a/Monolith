#pragma once
#include <windows.h>
#include <functional>

namespace ipc {

struct RecordingState {
    bool is_recording;
    bool is_paused;
    bool replay_enabled;
    bool recording_enabled;
};

// Start the JSON-RPC TCP server on 127.0.0.1:45991.
// Commands (save_replay, recording_start, recording_stop, pause_resume) are
// dispatched via PostMessage to hwnd. status_fn is called from the IPC thread
// for get_status queries; ManualRecorder::state() is mutex-protected so this
// is safe.
void start(HWND hwnd, std::function<RecordingState()> status_fn);
void stop();

} // namespace ipc
