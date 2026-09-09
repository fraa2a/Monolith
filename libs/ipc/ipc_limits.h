#pragma once
#include <chrono>
#include <cstddef>
#include <string_view>

namespace ipc::detail {
inline constexpr size_t kMaxClients = 16;
inline constexpr size_t kMaxRequestBytes = 64 * 1024; // excludes LF; includes optional CR
inline constexpr int kMaxJsonDepth = 64;
inline constexpr auto kPartialRequestTimeout = std::chrono::seconds(30);
inline constexpr auto kSendTimeout = std::chrono::seconds(30);
inline constexpr long kPollMicroseconds = 100000;

// send_some must observe its total deadline, return a positive byte count on
// progress and <= 0 on failure. No retry from byte zero after a short write.
template<class SendSome>
bool send_all(std::string_view response, SendSome send_some) {
    while (!response.empty()) {
        const int n = send_some(response.data(), response.size());
        if (n <= 0 || static_cast<size_t>(n) > response.size()) return false;
        response.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}
} // namespace ipc::detail
