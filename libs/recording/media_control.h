#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>
namespace recording {
class SessionEpoch {
    std::atomic<uint64_t> value_{0};
public:
    uint64_t begin() { return value_.fetch_add(1) + 1; }
    uint64_t current() const { return value_.load(); }
    bool accepts(uint64_t value) const { return value == current(); }
};
template<class Target>
bool target_changed(Target opened, Target desired) { return opened != desired; }
template<class Start, class Media, class Rollback>
bool start_with_media(Start start, Media media, Rollback rollback) {
    if (!start()) return false;
    if (media()) return true;
    rollback(); return false;
}
class PendingRestart {
    bool pending_ = false;
public:
    void request(bool changed) { pending_ = pending_ || changed; }
    bool take(bool recording) {
        if (recording) return false;
        return std::exchange(pending_, false);
    }
};
template<class Item>
class SessionBatch {
public:
    struct Batch { uint64_t id = 0; std::vector<Item> items; };
private:
    std::mutex mutex_;
    uint64_t next_ = 0;
    Batch current_;
public:
    void begin() { std::lock_guard lock(mutex_); current_ = {++next_, {}}; }
    uint64_t id() { std::lock_guard lock(mutex_); return current_.id; }
    bool add(Item item, uint64_t expected = 0) {
        std::lock_guard lock(mutex_);
        if (!current_.id || current_.items.size() >= 4096 ||
            (expected && expected != current_.id)) return false;
        current_.items.push_back(std::move(item)); return true;
    }
    Batch finish() { std::lock_guard lock(mutex_); return std::exchange(current_, {}); }
};
// WASAPI QPC positions are expressed in 100 ns units.
inline int64_t capture_time_us(int64_t position_100ns, int64_t origin_100ns) {
    return std::max<int64_t>(0, (position_100ns - origin_100ns) / 10);
}
}
