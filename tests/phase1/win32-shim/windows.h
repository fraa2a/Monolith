#pragma once
#include <cstdint>
using UINT = unsigned int;
using HWND = void*;
inline constexpr UINT WM_APP = 0x8000;
inline constexpr UINT WM_COMMAND = 0x0111;
inline constexpr UINT WM_CLOSE = 0x0010;
constexpr unsigned int MAKEWORD(unsigned int a, unsigned int b) { return a | (b << 8); }
constexpr unsigned int MAKEWPARAM(unsigned int a, unsigned int b) { return a | (b << 16); }
inline bool PostMessage(HWND, UINT, unsigned int, int) { return true; }
