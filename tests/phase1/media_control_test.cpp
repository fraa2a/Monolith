#include <recording/media_control.h>
#include <cassert>
#include <string>
#include <vector>
int main() {
    recording::SessionEpoch epoch;
    const auto old = epoch.begin(), next = epoch.begin();
    assert(!epoch.accepts(old) && epoch.accepts(next));
    assert(recording::target_changed(0, 10));
    assert(recording::target_changed(10, 20));
    assert(!recording::target_changed(20, 20));
    bool recording = false, media = false, session = false;
    assert(recording::start_with_media([&] { recording = true; session = true; return true; },
        [&] { assert(session); media = recording; return media; }, [&] { recording = false; }));
    assert(recording && media);
    recording = false;
    assert(!recording::start_with_media([&] { recording = true; return true; },
        [] { return false; }, [&] { recording = false; }));
    assert(!recording);
    recording::PendingRestart pending;
    pending.request(true); assert(!pending.take(true));
    pending.request(false); assert(pending.take(false)); assert(!pending.take(false));
    recording::SessionBatch<std::string> batch;
    batch.begin(); const auto aid = batch.id(); assert(batch.add("A"));
    auto a = batch.finish(); assert(!batch.add("late A"));
    batch.begin(); assert(!batch.add("stale A", aid)); assert(batch.add("B"));
    auto b = batch.finish();
    assert(a.id != b.id && a.items == std::vector<std::string>{"A"});
    assert(b.items == std::vector<std::string>{"B"});
    assert(recording::capture_time_us(6000001000, 1000000000) == 500000100);
    assert(recording::capture_time_us(999999999, 1000000000) == 0);
}
