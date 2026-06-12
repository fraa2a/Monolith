#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace settings_window {

void show(HWND owner, UINT reload_message);
void close_running();

} // namespace settings_window
