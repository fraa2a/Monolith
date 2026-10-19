#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
namespace recording {
class BoundedWorker {
    struct Job { std::function<void()> run; size_t bytes; };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    size_t max_jobs_, max_bytes_, bytes_ = 0;
    bool active_ = false, closing_ = false;
    std::function<void()> error_;
    std::thread thread_;
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            changed_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
            if (jobs_.empty()) break;
            auto job = std::move(jobs_.front());
            jobs_.pop_front(); bytes_ -= job.bytes; active_ = true;
            lock.unlock();
            try { job.run(); } catch (...) { try { if (error_) error_(); } catch (...) {} }
            lock.lock(); active_ = false; changed_.notify_all();
        }
    }
public:
    BoundedWorker(size_t max_jobs, size_t max_bytes, std::function<void()> error = {})
        : max_jobs_(max_jobs), max_bytes_(max_bytes), error_(std::move(error)),
          thread_([this] { run(); }) {}
    ~BoundedWorker() { close(); }
    BoundedWorker(const BoundedWorker&) = delete;
    BoundedWorker& operator=(const BoundedWorker&) = delete;
    bool submit(std::function<void()> run, size_t bytes = 0) {
        std::lock_guard lock(mutex_);
        if (closing_ || jobs_.size() >= max_jobs_ || bytes > max_bytes_ - bytes_) return false;
        jobs_.push_back({std::move(run), bytes}); bytes_ += bytes;
        changed_.notify_one(); return true;
    }
    bool submit_control(std::function<void()> run) {
        std::lock_guard lock(mutex_);
        if (closing_ || jobs_.size() >= max_jobs_ + 1) return false;
        jobs_.push_back({std::move(run), 0}); changed_.notify_one(); return true;
    }
    void drain() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return jobs_.empty() && !active_; });
    }
    void close() {
        { std::lock_guard lock(mutex_); closing_ = true; changed_.notify_all(); }
        if (thread_.joinable()) thread_.join();
    }
};
}
