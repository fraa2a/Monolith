#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace disk_segments {
class SessionDirectory {
    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE lock_ = INVALID_HANDLE_VALUE;
#else
    int lock_ = -1;
#endif
    bool acquire(const std::filesystem::path& marker, bool create) {
#ifdef _WIN32
        lock_ = CreateFileW(marker.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return lock_ != INVALID_HANDLE_VALUE;
#else
        lock_ = ::open(marker.c_str(), O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
        if (lock_ < 0) return false;
        if (flock(lock_, LOCK_EX | LOCK_NB) == 0) return true;
        ::close(lock_); lock_ = -1; return false;
#endif
    }
    void release() {
#ifdef _WIN32
        if (lock_ != INVALID_HANDLE_VALUE) CloseHandle(lock_);
        lock_ = INVALID_HANDLE_VALUE;
#else
        if (lock_ >= 0) ::close(lock_);
        lock_ = -1;
#endif
    }
    static bool segment(const std::filesystem::path& path) {
        const auto stem = path.stem().string();
        if (path.extension() != ".mkv" && path.extension() != ".mp4") return false;
        return stem.starts_with("seg_") && stem.size() > 4 &&
            stem.find_first_not_of("0123456789", 4) == std::string::npos;
    }
public:
    ~SessionDirectory() {
        release();
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::directory_iterator files(path_, ec);
        if (ec) return;
        size_t count = 0;
        for (const auto& file : files) if (file.path().filename() != "owner.lock") ++count;
        if (!count) {
            std::filesystem::remove(path_ / "owner.lock", ec);
            if (!ec) std::filesystem::remove(path_, ec);
        }
    }
    const std::filesystem::path& path() const { return path_; }
    bool create(const std::filesystem::path& root) {
        release(); static std::atomic<unsigned> sequence{0};
        std::error_code ec;
        for (int i = 0; i < 16; ++i) {
            auto path = root / ("monolith-segments-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1)));
            if (!std::filesystem::create_directory(path, ec)) { if (ec) return false; continue; }
            if (!acquire(path / "owner.lock", true)) return false;
#ifdef _WIN32
            const char magic[] = "Monolith disk session v1"; DWORD written = 0;
            if (!WriteFile(lock_, magic, sizeof(magic) - 1, &written, nullptr) ||
                written != sizeof(magic) - 1 || !FlushFileBuffers(lock_)) { release(); return false; }
#else
            const std::string magic = "Monolith disk session v1";
            if (::write(lock_, magic.data(), magic.size()) != static_cast<ssize_t>(magic.size()) ||
                fsync(lock_) != 0) { release(); return false; }
#endif
            path_ = std::move(path); return true;
        }
        return false;
    }
    static bool reclaim(const std::filesystem::path& root) {
        namespace fs = std::filesystem; std::error_code ec;
        fs::directory_iterator entries(root, ec); if (ec) return false;
        for (auto it = entries; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) return false;
            const auto dir = it->path();
            if (!dir.filename().string().starts_with("monolith-segments-")) continue;
            if (it->is_symlink(ec) || ec || !it->is_directory(ec)) continue;
            const auto marker = dir / "owner.lock";
            if (fs::is_symlink(marker, ec) || ec) continue;
            std::ifstream in(marker, std::ios::binary);
            const std::string magic((std::istreambuf_iterator<char>(in)), {});
            if (magic != "Monolith disk session v1") continue;
            in.close(); SessionDirectory dead;
            if (!dead.acquire(marker, false)) continue;
            bool foreign = false;
            fs::directory_iterator files(dir, ec); if (ec) return false;
            for (auto file = files; file != fs::directory_iterator(); file.increment(ec)) {
                if (ec) return false;
                if (file->path() == marker) continue;
                if (file->is_symlink(ec) || ec || !file->is_regular_file(ec) || !segment(file->path())) {
                    foreign = true; continue;
                }
                fs::remove(file->path(), ec); if (ec) return false;
            }
            dead.release();
            if (!foreign) {
                fs::remove(marker, ec); if (ec) return false;
                fs::remove(dir, ec); if (ec) return false;
            }
        }
        return !ec;
    }
};
}
