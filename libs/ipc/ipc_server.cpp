#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "ipc_server.h"
#include "ipc_limits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <mutex>
#include <memory>
#include <chrono>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

// These match the Cmd enum in main.cpp — kept in sync manually.
static constexpr UINT kCmdSaveReplay     = 1001;
static constexpr UINT kCmdRecordingStart = 1002;
static constexpr UINT kCmdRecordingStop  = 1003;
static constexpr UINT kCmdPauseResume    = 1004;
// Matches WM_SETTINGS_RELOAD (WM_APP + 2) in main.cpp — kept in sync manually.
static constexpr UINT kMsgSettingsReload = WM_APP + 2;

// Pending accept queue only; kMaxClients separately caps accepted workers.
static constexpr int kListenBacklog = 8;

namespace ipc {
namespace {

std::atomic<bool>               g_running{false};
SOCKET                          g_server_socket = INVALID_SOCKET;
HWND                            g_hwnd          = nullptr;
std::function<RecordingState()> g_status_fn;
ClipMutationFn                  g_mutation_fn;
SelectGameFn                    g_select_fn;
AddBookmarkFn                   g_add_bookmark_fn;
UpdateCloseUiFn                 g_update_close_ui_fn;
std::thread                     g_accept_thread;

// start/stop are serialized; the accept thread exclusively owns the worker
// vector until joined. Socket shutdown/close share a lock to prevent handle reuse.
std::mutex g_lifecycle_mutex;
std::mutex g_clients_mutex;
struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::atomic<bool> done{false};
    std::thread thread;
};
std::vector<std::unique_ptr<Client>> g_clients;

int wait_socket(SOCKET socket, bool writing)
{
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    timeval timeout{0, detail::kPollMicroseconds};
    return select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
}

bool send_response(SOCKET client, const std::string& response)
{
    const auto deadline = std::chrono::steady_clock::now() + detail::kSendTimeout;
    return detail::send_all(response, [&](const char* data, size_t size) {
        while (g_running && std::chrono::steady_clock::now() < deadline) {
            const int ready = wait_socket(client, true);
            if (ready < 0) return -1;
            if (ready == 0) continue;
            if (std::chrono::steady_clock::now() >= deadline) return -1;
            const int n = send(client, data, static_cast<int>(std::min(size, size_t{65536})), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return n;
        }
        return -1;
    });
}

std::string make_result(int id, const nlohmann::json& result)
{
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"]      = id;
    r["result"]  = result;
    return r.dump() + "\n";
}

std::string make_error(int id, int code, const char* msg)
{
    nlohmann::json r;
    r["jsonrpc"]         = "2.0";
    r["id"]              = id;
    r["error"]["code"]   = code;
    r["error"]["message"]= msg;
    return r.dump() + "\n";
}

void handle_client(SOCKET client)
{
    std::string buf;
    char tmp[4096];
    auto partial_started = std::chrono::steady_clock::now();

    while (g_running) {
        if (!buf.empty() && std::chrono::steady_clock::now() - partial_started >=
            detail::kPartialRequestTimeout) return;
        const int ready = wait_socket(client, false);
        if (ready < 0) return;
        if (ready == 0) continue;
        if (!buf.empty() && std::chrono::steady_clock::now() - partial_started >=
            detail::kPartialRequestTimeout) return;
        const int n = recv(client, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) return;
        // Process bytes by explicit length: embedded NUL is invalid JSON, not
        // a C-string terminator. Never accumulate an unbounded pipelined batch.
        for (int i = 0; i < n && g_running; ++i) {
            if (tmp[i] != '\n') {
                if (buf.size() == detail::kMaxRequestBytes) return;
                if (buf.empty()) partial_started = std::chrono::steady_clock::now();
                buf.push_back(tmp[i]);
                continue;
            }
            std::string line;
            line.swap(buf);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            int         req_id   = -1;
            std::string response;

            try {
                if (line.find('\0') != std::string::npos) throw std::invalid_argument("NUL in JSON");
                auto req = nlohmann::json::parse(line, [](int depth, nlohmann::json::parse_event_t,
                                                          nlohmann::json&) {
                    if (depth > detail::kMaxJsonDepth) throw std::length_error("JSON nesting limit");
                    return true;
                });
                req_id             = req.value("id", -1);
                std::string method = req.value("method", "");

                try {
                    if (method == "save_replay") {
                        PostMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(kCmdSaveReplay, 0), 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "recording_start") {
                        PostMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(kCmdRecordingStart, 0), 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "recording_stop") {
                        PostMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(kCmdRecordingStop, 0), 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "pause_resume") {
                        PostMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(kCmdPauseResume, 0), 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "get_status") {
                        // g_status_fn/g_mutation_fn are set once in start() before the
                        // accept loop begins and never reassigned, so concurrent
                        // handle_client() threads reading them is safe without a lock.
                        // Each call reads live engine state (g_recording, etc.), which
                        // is independently synchronized by its own owner.
                        RecordingState st = g_status_fn();
                        response = make_result(req_id, {
                            {"recording",         st.is_recording},
                            {"paused",            st.is_paused},
                            {"replay_enabled",    st.replay_enabled},
                            {"recording_enabled", st.recording_enabled},
                            {"clip_generation",   st.clip_generation},
                            {"version",           st.version},
                        });
                    } else if (method == "reload_settings") {
                        PostMessage(g_hwnd, kMsgSettingsReload, 0, 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "update_close_ui") {
                        // Updater.exe asks the engine to close the UI process
                        // so ui\* can be swapped on disk. Runs directly on
                        // this IPC thread (blocks up to ~3s: graceful close,
                        // then terminate) — the reply is the confirmation
                        // that the files are safe to replace.
                        if (!g_update_close_ui_fn) {
                            response = make_error(req_id, -32601, "Update control unavailable");
                        } else {
                            g_update_close_ui_fn();
                            response = make_result(req_id, {{"status", "ok"}});
                        }
                    } else if (method == "update_engine_exit") {
                        // Updater.exe asks the engine to shut down so the
                        // engine files can be swapped. Graceful by design:
                        // WM_CLOSE → WM_DESTROY stops any recording cleanly.
                        // The reply may never arrive (the engine tears its
                        // IPC server down during shutdown) — the updater
                        // polls for the port to close instead.
                        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
                        response = make_result(req_id, {{"status", "accepted"}});
                    } else if (method == "recording_add_bookmark") {
                        // Handled directly on the IPC thread: bookmark timestamp
                        // accuracy matters, so it must not round-trip through the
                        // message loop.
                        if (!g_add_bookmark_fn) {
                            response = make_error(req_id, -32601, "Bookmark handler unavailable");
                        } else {
                            const std::string err = g_add_bookmark_fn();
                            if (err.empty())
                                response = make_result(req_id, {{"status", "ok"}});
                            else
                                response = make_error(req_id, -32000, err.c_str());
                        }
                    } else if (method == "set_selected_game") {
                        if (!g_select_fn) {
                            response = make_error(req_id, -32601, "Selection unavailable");
                        } else {
                            const auto& p = req.contains("params") ? req["params"]
                                                                   : nlohmann::json::object();
                            std::string exe = (p.contains("exe") && p["exe"].is_string())
                                                  ? p["exe"].get<std::string>() : std::string();
                            std::transform(exe.begin(), exe.end(), exe.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            uint32_t pid = (p.contains("pid") && p["pid"].is_number_unsigned())
                                               ? p["pid"].get<uint32_t>() : 0u;
                            if (exe == "auto") exe.clear();
                            g_select_fn(exe, pid);
                            response = make_result(req_id, {{"status", "accepted"}});
                        }
                    } else if (method == "clip_set_favorite" ||
                               method == "clip_add_hashtag" ||
                               method == "clip_remove_hashtag" ||
                               method == "clip_rename" ||
                               method == "clip_set_title" ||
                               method == "clip_regen_thumb" ||
                               method == "clip_delete" ||
                               method == "clip_trim") {
                        if (!g_mutation_fn) {
                            response = make_error(req_id, -32601, "Mutations unavailable");
                        } else {
                            const auto& p = req.contains("params") ? req["params"]
                                                                   : nlohmann::json::object();
                            auto get_or = [&p](const char* key, auto default_value) {
                                using T = decltype(default_value);
                                if (p.contains(key) && !p[key].is_null())
                                    return p.value(key, default_value);
                                return T(default_value);
                            };
                            ClipMutation m;
                            m.method   = method;
                            m.source   = get_or("source", std::string("replay"));
                            m.id       = get_or("id", static_cast<int64_t>(0));
                            m.tag      = get_or("tag", std::string());
                            m.favorite = get_or("favorite", false);
                            m.new_name = get_or("new_name", std::string());
                            m.title    = get_or("title", std::string());
                            m.start    = get_or("start", 0.0);
                            m.end      = get_or("end", 0.0);
                            // handle_client runs on its own thread per connection; the
                            // mutation callback (handle_clip_mutation in main.cpp)
                            // opens its own DB handle per call and only touches
                            // mutex-guarded globals, so concurrent invocations from
                            // different client threads are safe.
                            std::string err = g_mutation_fn(m);
                            if (err.empty())
                                response = make_result(req_id, {{"status", "ok"}});
                            else
                                response = make_error(req_id, -32000, err.c_str());
                        }
                    } else {
                        response = make_error(req_id, -32601, "Method not found");
                    }
                } catch (...) {
                    // Handler failure (bad_alloc, DB error...) — not a parse
                    // problem; report it as an internal error.
                    response = make_error(req_id, -32603, "Internal error");
                }
            } catch (...) {
                response = make_error(req_id, -32700, "Parse error");
            }

            if (!response.empty() && !send_response(client, response)) return;
        }
    }
}

void accept_loop(SOCKET server)
{
    while (g_running) {
        // Poll even without new connections so the last completed worker is
        // reclaimed promptly, not merely at the next accept or at shutdown.
        for (auto it = g_clients.begin(); it != g_clients.end();) {
            if ((*it)->done.load()) {
                (*it)->thread.join();
                it = g_clients.erase(it);
            } else ++it;
        }
        const int ready = wait_socket(server, false);
        if (ready < 0) break;
        if (ready == 0) continue;
        SOCKET socket = accept(server, nullptr, nullptr);
        if (socket == INVALID_SOCKET) continue;
        if (!g_running || g_clients.size() >= detail::kMaxClients) {
            closesocket(socket);
            continue;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(socket);
            continue;
        }
        // All allocating setup can fail; no unowned socket or joinable thread
        // may escape. The vector owns Client before the worker can access it.
        try {
            auto client = std::make_unique<Client>();
            client->socket = socket;
            g_clients.push_back(std::move(client));
        } catch (...) {
            closesocket(socket);
            continue;
        }
        Client* client = g_clients.back().get();
        try {
            client->thread = std::thread([client, socket] {
                try { handle_client(socket); } catch (...) { /* close below */ }
                {
                    std::lock_guard lk(g_clients_mutex);
                    closesocket(client->socket);
                    client->socket = INVALID_SOCKET;
                }
                client->done.store(true);
            });
        } catch (...) {
            closesocket(socket);
            g_clients.pop_back();
        }
    }
}

} // namespace

void start(HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn,
           SelectGameFn select_fn,
           AddBookmarkFn add_bookmark_fn,
           UpdateCloseUiFn update_close_ui_fn)
{
    std::lock_guard lifecycle(g_lifecycle_mutex);
    if (g_accept_thread.joinable()) return;
    g_hwnd                 = hwnd;
    g_status_fn            = std::move(status_fn);
    g_mutation_fn          = std::move(mutation_fn);
    g_select_fn            = std::move(select_fn);
    g_add_bookmark_fn      = std::move(add_bookmark_fn);
    g_update_close_ui_fn   = std::move(update_close_ui_fn);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    g_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_server_socket == INVALID_SOCKET) { WSACleanup(); return; }

    int opt = 1;
    setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(45991);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(g_server_socket,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) == SOCKET_ERROR ||
        listen(g_server_socket, kListenBacklog) == SOCKET_ERROR)
    {
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(g_server_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
        WSACleanup();
        return;
    }
    g_running = true;
    try {
        g_accept_thread = std::thread(accept_loop, g_server_socket);
    } catch (...) {
        g_running = false;
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
        WSACleanup();
    }
}

void stop()
{
    std::lock_guard lifecycle(g_lifecycle_mutex);
    if (!g_accept_thread.joinable()) return;
    g_running = false;
    // Nonblocking accept + bounded select: no close/reuse race on the listener.
    g_accept_thread.join();
    closesocket(g_server_socket);
    g_server_socket = INVALID_SOCKET;
    {
        std::lock_guard lk(g_clients_mutex);
        for (const auto& client : g_clients)
            if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH);
    }
    for (auto& client : g_clients) client->thread.join();
    g_clients.clear();
    WSACleanup();
}

} // namespace ipc
