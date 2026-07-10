#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "ipc_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// These match the Cmd enum in main.cpp — kept in sync manually.
static constexpr UINT kCmdSaveReplay     = 1001;
static constexpr UINT kCmdRecordingStart = 1002;
static constexpr UINT kCmdRecordingStop  = 1003;
static constexpr UINT kCmdPauseResume    = 1004;
// Matches WM_SETTINGS_RELOAD (WM_APP + 2) in main.cpp — kept in sync manually.
static constexpr UINT kMsgSettingsReload = WM_APP + 2;

// Reasonable ceiling for briefly-overlapping connections (UI + Stream Deck
// plugin, plus a reconnect race); this is a local-only loopback service, not
// internet-facing, so there is no need for anything larger.
static constexpr int kListenBacklog = 8;

namespace ipc {
namespace {

std::atomic<bool>               g_running{false};
SOCKET                          g_server_socket = INVALID_SOCKET;
HWND                            g_hwnd          = nullptr;
std::function<RecordingState()> g_status_fn;
ClipMutationFn                  g_mutation_fn;
std::thread                     g_accept_thread;
std::string                     g_auth_token;

std::mutex               g_clients_mutex;
std::vector<SOCKET>      g_client_sockets;
std::vector<std::thread> g_client_threads;

void track_client(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_clients_mutex);
    g_client_sockets.push_back(s);
}

void untrack_client(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_clients_mutex);
    auto it = std::find(g_client_sockets.begin(), g_client_sockets.end(), s);
    if (it != g_client_sockets.end()) g_client_sockets.erase(it);
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
                std::string token  = req.value("token", "");

                if (token != g_auth_token) {
                    response = make_error(req_id, -32001, "Unauthorized: missing or invalid token");
                } else if (method == "save_replay") {
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
                    });
                } else if (method == "reload_settings") {
                    PostMessage(g_hwnd, kMsgSettingsReload, 0, 0);
                    response = make_result(req_id, {{"status", "accepted"}});
                } else if (method == "clip_set_favorite" ||
                           method == "clip_add_hashtag" ||
                           method == "clip_remove_hashtag" ||
                           method == "clip_rename" ||
                           method == "clip_set_title" ||
                           method == "clip_regen_thumb" ||
                           method == "clip_delete") {
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

    untrack_client(client);
    closesocket(client);
}

void accept_loop()
{
    while (g_running) {
        SOCKET client = accept(g_server_socket, nullptr, nullptr);
        if (client == INVALID_SOCKET) break; // closed by stop()
        track_client(client);

        std::lock_guard<std::mutex> lk(g_clients_mutex);
        // One thread per accepted connection: this is a low-traffic local
        // control-plane server (UI + Stream Deck plugin, at most a couple of
        // clients), so per-connection threads are simpler than an event loop
        // and are sufficient for this volume.
        g_client_threads.emplace_back(handle_client, client);
    }
}

std::string generate_token()
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937_64 gen(
        (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()));
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlphabet) - 2);

    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i)
        token += kAlphabet[dist(gen)];
    return token;
}

// Writes the token to <app_data_dir>\ipc_token, restricted to the current
// user's SID via an explicit DACL, so other local accounts on the machine
// cannot read it. Best-effort: on failure the file is simply absent/stale and
// every RPC request will be rejected as unauthorized (fail-closed).
bool write_token_file(const std::wstring& app_data_dir, const std::string& token)
{
    std::wstring path = app_data_dir + L"\\ipc_token";

    PSID sid = nullptr;
    HANDLE process_token = nullptr;
    bool sid_ok = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
        DWORD needed = 0;
        GetTokenInformation(process_token, TokenUser, nullptr, 0, &needed);
        std::vector<BYTE> buf(needed);
        if (needed > 0 &&
            GetTokenInformation(process_token, TokenUser, buf.data(), needed, &needed)) {
            auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
            sid = user->User.Sid;
            sid_ok = sid != nullptr;

            if (sid_ok) {
                EXPLICIT_ACCESSW ea{};
                ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
                ea.grfAccessMode        = SET_ACCESS;
                ea.grfInheritance       = NO_INHERITANCE;
                ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
                ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
                ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(sid);

                PACL acl = nullptr;
                if (SetEntriesInAclW(1, &ea, nullptr, &acl) == ERROR_SUCCESS && acl) {
                    SECURITY_ATTRIBUTES sa{};
                    sa.nLength = sizeof(sa);
                    SECURITY_DESCRIPTOR sd;
                    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
                    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
                    sa.lpSecurityDescriptor = &sd;

                    HANDLE file = CreateFileW(
                        path.c_str(), GENERIC_WRITE, 0, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        WriteFile(file, token.data(), static_cast<DWORD>(token.size()),
                                  &written, nullptr);
                        CloseHandle(file);
                        LocalFree(acl);
                        CloseHandle(process_token);
                        return written == token.size();
                    }
                    LocalFree(acl);
                }
            }
        }
        CloseHandle(process_token);
    }

    // Fallback: still write the file (with default ACLs) rather than leave
    // clients unable to authenticate at all.
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, token.data(), static_cast<DWORD>(token.size()), &written, nullptr);
    CloseHandle(file);
    return written == token.size();
}

} // namespace

void start(const std::wstring& app_data_dir,
           HWND hwnd,
           std::function<RecordingState()> status_fn,
           ClipMutationFn mutation_fn)
{
    g_hwnd        = hwnd;
    g_status_fn   = std::move(status_fn);
    g_mutation_fn = std::move(mutation_fn);
    g_auth_token  = generate_token();
    write_token_file(app_data_dir, g_auth_token);

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

    g_running = true;
    g_accept_thread = std::thread(accept_loop);
}

void stop()
{
    g_running = false;
    if (g_server_socket != INVALID_SOCKET) {
        shutdown(g_server_socket, SD_BOTH);
        closesocket(g_server_socket);
        g_server_socket = INVALID_SOCKET;
    }
    // Unblock every handle_client()'s recv() so each client thread (and the
    // accept thread, via the closed listen socket above) can return.
    {
        std::lock_guard<std::mutex> lk(g_clients_mutex);
        for (SOCKET s : g_client_sockets)
            shutdown(s, SD_BOTH);
    }
    if (g_accept_thread.joinable()) g_accept_thread.join();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(g_clients_mutex);
        threads.swap(g_client_threads);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();

    WSACleanup();
}

} // namespace ipc
