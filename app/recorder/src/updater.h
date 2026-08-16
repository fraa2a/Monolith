#pragma once

// Component self-update for the recorder, delegated to Updater.exe
// (app/updater). The dedicated process owns the whole flow: it fetches
// update-manifest.json from the releases page, downloads only the
// components whose version changed (engine / ui / updater are versioned
// independently), verifies the Ed25519 signatures and swaps the files on
// disk. The recorder merely launches it and cleans up the *.old files the
// swap dance parks next to replaced binaries.
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

// Deletes *.old leftovers from previous self-updates (app root + ui\) and
// the legacy WinSparkle.dll of pre-component-updater installs. Call early
// at startup; files still locked are retried on the next launch.
void post_update_cleanup();

// Kept for WM_DESTROY symmetry — nothing to shut down anymore.
void shutdown();

} // namespace updater
