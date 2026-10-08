#pragma once

#include <string>

// Verbose logging is opt-in; error logging has a separate always-on path.
namespace logging {

// Call once at startup. `dir` is the directory the log file lives in
// (%LocalAppData%\Monolith); the caller resolves it (see app_data_dir() in
// main.cpp) since other subsystems need that path too.
void init(bool enabled, const std::wstring& dir);

void set_enabled(bool enabled);

bool enabled();

void log(const char* tag, const char* msg);

// Error logging bypasses enabled() and opens the file on demand.
void log_error(const char* tag, const char* msg);

} // namespace logging
