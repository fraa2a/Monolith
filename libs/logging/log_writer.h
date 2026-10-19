#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <share.h>
#endif

namespace logging {
class LogWriter {
public:
    explicit LogWriter(std::filesystem::path path, uint64_t limit = 5 * 1024 * 1024,
                       int backups = 3, size_t queue_limit = 128 * 1024)
        : path_(std::move(path)), limit_(limit), backups_(backups), queue_limit_(queue_limit),
          worker_([this] { run(); }) {}
    ~LogWriter() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_one();
        worker_.join();
    }
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;
    bool write(std::string record) {
        std::lock_guard lock(mutex_);
        if (stopping_ || record.size() > queue_limit_ - queued_bytes_) return false;
        queued_bytes_ += record.size();
        queue_.push_back(std::move(record));
        ready_.notify_one();
        return true;
    }
private:
    FILE* open() const {
#ifdef _WIN32
        return _wfsopen(path_.c_str(), L"ab", _SH_DENYNO);
#else
        return std::fopen(path_.c_str(), "ab");
#endif
    }
    bool rotate() const {
        std::error_code ec;
        for (int i = backups_; i >= 1; --i) {
            auto source = path_;
            if (i > 1) source += "." + std::to_string(i - 1);
            auto target = path_;
            target += "." + std::to_string(i);
            const bool present = std::filesystem::exists(source, ec);
            if (ec) return false;
            if (!present) continue;
            std::filesystem::remove(target, ec);
            if (ec) return false;
            std::filesystem::rename(source, target, ec);
            if (ec) return false;
        }
        return true;
    }
    static void fallback(const std::string& records) {
        std::fwrite(records.data(), 1, records.size(), stderr);
    }
    void run() {
        FILE* file = nullptr;
        uint64_t bytes = 0;
        std::string pending;
        for (;;) {
            std::deque<std::string> batch;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_) break;
                batch.swap(queue_);
                queued_bytes_ = 0;
            }
            for (const auto& record : batch) {
                if (!file) {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(path_, ec);
                    bytes = ec ? 0 : size;
                    file = open();
                }
                if (file && bytes != 0 && (bytes >= limit_ || record.size() > limit_ - bytes)) {
                    if (std::fclose(file) != 0) fallback(pending);
                    pending.clear();
                    file = nullptr;
                    if (rotate()) bytes = 0;
                    file = open();
                }
                if (!file || std::fwrite(record.data(), 1, record.size(), file) != record.size()) {
                    fallback(pending);
                    fallback(record);
                    pending.clear();
                    if (file) std::fclose(file);
                    file = nullptr;
                } else {
                    pending += record;
                    bytes += record.size();
                }
            }
            if (file && std::fflush(file) != 0) {
                fallback(pending);
                std::fclose(file);
                file = nullptr;
            }
            pending.clear();
        }
        if (file) std::fclose(file);
    }
    std::filesystem::path path_;
    uint64_t limit_;
    int backups_;
    size_t queue_limit_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    size_t queued_bytes_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};
} // namespace logging
