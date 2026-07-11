#pragma once

#include <string>

// Rotating file logger for the recorder engine. Disabled by default — no log
// file is created and log() is a cheap no-op — until the user opts in via
// Settings > Advanced (advanced.logging_enabled), wired through set_enabled()
// on settings reload.
namespace logging {

// Call once at startup. `dir` is the directory the log file lives in
// (%LocalAppData%\Monolith); the caller resolves it (see app_data_dir() in
// main.cpp) since other subsystems need that path too.
void init(bool enabled, const std::wstring& dir);

// Toggles logging at runtime (settings reload). Opens or closes the log file
// to match; safe to call repeatedly, including with the same value.
void set_enabled(bool enabled);

bool enabled();

void log(const char* tag, const char* msg);

// Always-on error channel. Unlike log(), this is NOT gated by enabled(): the
// message is emitted to the debugger and appended to the log file (opened on
// demand even when verbose logging is off) so genuine failures are never
// silent. Reserve for real errors — it bypasses the opt-in and can create the
// log file.
void log_error(const char* tag, const char* msg);

} // namespace logging
