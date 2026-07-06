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

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// These match the Cmd enum in main.cpp — kept in sync manually.
static constexpr UINT kCmdSaveReplay     = 1001;
static constexpr UINT kCmdRecordingStart = 1002;
static constexpr UINT kCmdRecordingStop  = 1003;
static constexpr UINT kCmdPauseResume    = 1004;
// Matches WM_SETTINGS_RELOAD (WM_APP + 2) in main.cpp — kept in sync manually.
static constexpr UINT kMsgSettingsReload = WM_APP + 2;

namespace ipc {
namespace {

std::atomic<bool>               g_running{false};
SOCKET                          g_server_socket = INVALID_SOCKET;
std::atomic<SOCKET>             g_client_socket{INVALID_SOCKET};
HWND                            g_hwnd          = nullptr;
std::function<RecordingState()> g_status_fn;
ClipMutationFn                  g_mutation_fn;
std::thread                     g_thread;

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
    char        tmp[4096];

    while (g_running) {
        int n = recv(client, tmp, static_cast<int>(sizeof(tmp)) - 1, 0);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        std::size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            int         req_id   = -1;
            std::string response;

            try {
                auto        req    = nlohmann::json::parse(line);
                req_id             = req.value("id", -1);
                std::string method = req.value("method", "");

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
                    RecordingState st = g_status_fn();
                    response = make_result(req_id, {
                        {"recording",         st.is_recording},
                        {"paused",            st.is_paused},
                        {"replay_enabled",    st.replay_enabled},
                        {"recording_enabled", st.recording_enabled},
                    });
                } else if (method == "reload_settings") {
                    PostMessage(g_hwnd, kMsgSettingsReload, 0, 0);
                    response = make_result(req_id, {{"status", "accepted"}});
                } else if (method == "clip_set_favorite" ||
                           method == "clip_add_hashtag" ||
                           method == "clip_remove_hashtag" ||
                           method == "clip_rename" ||
                           method == "clip_delete") {
                    if (!g_mutation_fn) {
                        response = make_error(req_id, -32601, "Mutations unavailable");
                    } else {
                        const auto& p = req.contains("params") ? req["params"]
                                                               : nlohmann::json::object();
                        ClipMutation m;
                        m.method   = method;
                        m.source   = p.value("source", "replay");
                        m.id       = p.value("id", static_cast<int64_t>(0));
                        m.tag      = p.value("tag", std::string());
                        m.favorite = p.value("favorite", false);
                        m.new_name = p.value("new_name", std::string());
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
                response = make_error(req_id, -32700, "Parse error");
            }

            if (!response.empty()) {
                send(client,
                     response.c_str(),
                     static_cast<int>(response.size()),
                     0);
            }
        }
    }
}

void server_loop()
{
    while (g_running) {
        SOCKET client = accept(g_server_socket, nullptr, nullptr);
        if (client == INVALID_SOCKET) break; // closed by stop()
        g_client_socket.store(client, std::memory_order_release);
        handle_client(client);
        g_client_socket.store(INVALID_SOCKET, std::memory_order_release);
        closesocket(client);
    }
}

} // namespace

void start(HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn)
{
    g_hwnd        = hwnd;
    g_status_fn   = std::move(status_fn);
    g_mutation_fn = std::move(mutation_fn);

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
        listen(g_server_socket, 1) == SOCKET_ERROR)
    {
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    g_running = true;
    g_thread  = std::thread(server_loop);
}

void stop()
{
    g_running = false;
    if (g_server_socket != INVALID_SOCKET) {
        shutdown(g_server_socket, SD_BOTH);
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
    }
    // Unblock handle_client()'s recv() so server_loop() (and the join below)
    // can return even while a client is still connected.
    SOCKET client = g_client_socket.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
    if (client != INVALID_SOCKET)
        shutdown(client, SD_BOTH);
    if (g_thread.joinable()) g_thread.join();
    WSACleanup();
}

} // namespace ipc
