// Compile the real server with a thin POSIX/Win32 test adapter. Including the
// source allows checking reclamation after joining its sole vector owner.
#include "../../libs/ipc/ipc_server.cpp"
#include <iostream>
#include <stdexcept>
#include <csignal>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using namespace std::chrono_literals;
SOCKET connect_client() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(s != INVALID_SOCKET);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(45991);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return s;
}
void write_bytes(SOCKET s, const std::string& data) {
    CHECK(ipc::detail::send_all(data, [s](const char* p, size_t n) {
        return static_cast<int>(send(s, p, n, 0));
    }));
}
std::string read_line(SOCKET s, std::chrono::seconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string result;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ipc::wait_socket(s, false) <= 0) continue;
        char c;
        const int n = static_cast<int>(recv(s, &c, 1, 0));
        if (n <= 0) return result;
        result += c;
        if (c == '\n') return result;
    }
    throw std::runtime_error("read deadline");
}
std::string status(SOCKET s) {
    write_bytes(s, "{\"id\":1,\"method\":\"get_status\"}\n");
    return read_line(s);
}
int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::string sent;
    CHECK(ipc::detail::send_all("abcdef\n", [&](const char* p, size_t n) {
        const size_t count = std::min(n, size_t{2}); sent.append(p, count); return static_cast<int>(count);
    }));
    CHECK(sent == "abcdef\n");
    CHECK(!ipc::detail::send_all("x", [](const char*, size_t) { return 0; }));
    CHECK(!ipc::detail::send_all("x", [](const char*, size_t) { return -1; }));

    // Exercise the real nonblocking send loop against a peer that never reads.
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    u_long nonblocking = 1;
    CHECK(ioctlsocket(pair[0], FIONBIO, &nonblocking) == 0);
    const std::string big_response(8 * 1024 * 1024, 'x');
    ipc::g_running = true;
    auto begin_send = std::chrono::steady_clock::now();
    CHECK(!ipc::send_response(pair[0], big_response));
    CHECK(std::chrono::steady_clock::now() - begin_send >= ipc::detail::kSendTimeout);
    CHECK(std::chrono::steady_clock::now() - begin_send < 32s);
    bool completed = true;
    std::thread sender([&] { completed = ipc::send_response(pair[0], big_response); });
    std::this_thread::sleep_for(100ms);
    ipc::g_running = false;
    sender.join(); CHECK(!completed);
    closesocket(pair[1]);
    ipc::g_running = true;
    CHECK(!ipc::send_response(pair[0], "disconnected\n"));
    ipc::g_running = false; closesocket(pair[0]);

    ipc::start(nullptr, [] { return ipc::RecordingState{false, false, true, true, 0, "test", true, true, "disk failure"}; },
        [](const ipc::ClipMutation& m) {
            CHECK(m.catalog_uid == "catalog" && m.clip_uid == "clip");
            CHECK(m.media_revision == 7 && m.id == 1);
            if (m.method == "clip_set_duration") CHECK(m.duration == 5.0);
            if (m.method == "clip_capture_thumb") CHECK(m.upload_token == std::string(32, 'a'));
            if (m.method == "clip_add_bookmark") CHECK(m.time_seconds == 1.5 && m.label == "marker");
            if (m.method == "clip_update_bookmark" || m.method == "clip_delete_bookmark") CHECK(m.seq == 3);
            return std::string{};
        });
    CHECK(ipc::g_running);
    SOCKET persistent = connect_client();
    CHECK(status(persistent).find("result") != std::string::npos);
    CHECK(status(persistent).find("disk failure") != std::string::npos);
    for (const char* method : {"clip_trim", "clip_add_bookmark", "clip_update_bookmark", "clip_delete_bookmark", "clip_capture_thumb", "clip_set_duration"}) {
        const auto request = nlohmann::json{{"id",2},{"method",method},{"params",{
            {"source","manual"},{"id",1},{"catalog_uid","catalog"},{"clip_uid","clip"},
            {"duration",5.0},{"media_revision",7},{"seq",3},{"time_seconds",1.5},{"label","marker"},{"upload_token",std::string(32,'a')}}}};
        write_bytes(persistent, request.dump()+"\n");
        CHECK(read_line(persistent).find("result") != std::string::npos);
    }
    std::this_thread::sleep_for(6s); // longer than Stream Deck's five-second poll
    CHECK(status(persistent).find("result") != std::string::npos);
    write_bytes(persistent, "not json\n{\"id\":2,\"method\":\"unknown\"}\r\n");
    CHECK(read_line(persistent).find("-32700") != std::string::npos);
    CHECK(read_line(persistent).find("-32601") != std::string::npos);
    write_bytes(persistent, std::string("{\0}", 3) + "\n");
    CHECK(read_line(persistent).find("-32700") != std::string::npos);
    write_bytes(persistent, std::string(1000, '[') + std::string(1000, ']') + "\n");
    CHECK(read_line(persistent).find("-32700") != std::string::npos);
    const std::string request = "{\"id\":1,\"method\":\"get_status\"}";
    write_bytes(persistent, request + std::string(1, '\0') + "junk\n");
    CHECK(read_line(persistent).find("-32700") != std::string::npos);
    write_bytes(persistent, request + std::string(ipc::detail::kMaxRequestBytes - request.size(), ' ') + "\n");
    CHECK(read_line(persistent).find("result") != std::string::npos);
    SOCKET large = connect_client();
    write_bytes(large, std::string(ipc::detail::kMaxRequestBytes + 1, 'x'));
    CHECK(read_line(large).empty()); closesocket(large);

    // Churn exercises the production worker completion/join path.
    for (int i = 0; i < 250; ++i) {
        SOCKET s = connect_client(); CHECK(status(s).find("result") != std::string::npos); closesocket(s);
    }
    std::vector<SOCKET> clients;
    for (size_t i = 1; i < ipc::detail::kMaxClients; ++i) {
        SOCKET s = connect_client(); CHECK(status(s).find("result") != std::string::npos); clients.push_back(s);
    }
    SOCKET excess = connect_client(); CHECK(read_line(excess).empty()); closesocket(excess);
    CHECK(status(persistent).find("result") != std::string::npos);
    for (SOCKET s : clients) closesocket(s);
    std::this_thread::sleep_for(300ms);

    SOCKET silent = connect_client();
    SOCKET slow = connect_client(); write_bytes(slow, "{");
    std::this_thread::sleep_for(16s); write_bytes(slow, " "); // must not reset first-byte deadline
    CHECK(read_line(slow, 17s).empty()); closesocket(slow);
    CHECK(read_line(silent).empty()); closesocket(silent);
    CHECK(status(persistent).find("result") != std::string::npos); // >30s idle survives
    closesocket(persistent);
    std::this_thread::sleep_for(400ms);
    ipc::g_running = false;
    ipc::g_accept_thread.join(); // only now is inspecting the vector race-free
    CHECK(ipc::g_clients.empty());
    ipc::g_accept_thread = std::thread([] {}); // preserve normal stop cleanup path
    ipc::stop(); ipc::stop();

    for (int i = 0; i < 20; ++i) {
        ipc::start(nullptr, [] { return ipc::RecordingState{}; });
        SOCKET idle = connect_client();
        SOCKET partial = connect_client(); write_bytes(partial, "{");
        const auto begin = std::chrono::steady_clock::now();
        ipc::stop();
        CHECK(std::chrono::steady_clock::now() - begin < 2s);
        closesocket(idle); closesocket(partial);
    }
    std::cout << "PASS ipc_server: short sends, persistent idle, parse/size/depth limits, 250 reconnects, 16-client cap, trickle deadline, reclamation, 20 shutdown races\n";
}
