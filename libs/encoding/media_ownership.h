#pragma once
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
namespace encoding {
namespace detail {
inline std::mutex media_mutex;
inline std::map<std::wstring, size_t> media_writers;
inline std::wstring media_key(const std::wstring& path) {
    std::error_code ec;
    auto key = std::filesystem::weakly_canonical(path, ec);
    auto result = ec ? path : key.wstring();
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
#endif
    return result;
}
}
// Keep ownership from before file creation until catalog publication.
class MediaWriteGuard {
public:
    explicit MediaWriteGuard(const std::wstring& path) : key_(detail::media_key(path)) {
        std::lock_guard lock(detail::media_mutex);
        ++detail::media_writers[key_];
    }
    ~MediaWriteGuard() {
        std::lock_guard lock(detail::media_mutex);
        auto found = detail::media_writers.find(key_);
        if (found != detail::media_writers.end() && --found->second == 0)
            detail::media_writers.erase(found);
    }
    MediaWriteGuard(const MediaWriteGuard&) = delete;
    MediaWriteGuard& operator=(const MediaWriteGuard&) = delete;
private:
    std::wstring key_;
};
inline bool media_is_owned(const std::wstring& path) {
    const auto key = detail::media_key(path);
    std::lock_guard lock(detail::media_mutex);
    return detail::media_writers.contains(key);
}
}
