#pragma once

// WinSparkle-based auto-update for the recorder process.
// The appcast is hosted on the default public Monolith repo releases page;
// WinSparkle compares the running FileVersion against the feed, downloads the
// signed installer, and relaunches it.
namespace updater {

// Initialize WinSparkle and (optionally) start the background update check.
// Call once after WinRT/COM init, before the message loop.
void init(bool auto_check_enabled);

// Toggle automatic background checks at runtime (settings reload).
void set_auto_check(bool enabled);

// Manual "Check for updates…" with UI (tray menu).
void check_now();

// Must be called before process exit (WM_DESTROY).
void shutdown();

} // namespace updater
