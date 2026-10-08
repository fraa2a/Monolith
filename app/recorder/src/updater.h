#pragma once

// Updater.exe owns component download, verification and replacement.
// The recorder launches it and cleans parked files on startup.
namespace updater {

// Store the automatic-check preference (settings: update.auto_check).
// Call once at startup, before check_silent().
void init(bool auto_check_enabled);

// Toggle the automatic-check preference at runtime (settings reload).
void set_auto_check(bool enabled);

// Manual "Check for updates…" (tray menu): launches Updater.exe with its
// window visible immediately.
void check_now();

// Silent check at startup: launches Updater.exe --auto, which exits without
// showing anything unless an update is actually available.
void check_silent();

// Clean parked files on startup; retry locked files on a later launch.
void post_update_cleanup();

// Kept for WM_DESTROY symmetry - nothing to shut down anymore.
void shutdown();

} // namespace updater
