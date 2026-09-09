#pragma once
// Linux-only test adapter for compiling the real IPC server, not a production
// portability layer. Windows socket semantics still require Windows testing.
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
using SOCKET = int;
inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;
inline constexpr int SD_BOTH = SHUT_RDWR;
inline constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
struct WSADATA {};
inline int WSAStartup(int, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }
inline int WSAGetLastError() { return errno; }
inline int closesocket(SOCKET s) { return close(s); }
inline int ioctlsocket(SOCKET s, unsigned long request, unsigned long* value) { return ioctl(s, request, value); }
inline int win_select(int, fd_set* r, fd_set* w, fd_set* e, timeval* t) { return ::select(FD_SETSIZE, r, w, e, t); }
#define select win_select
