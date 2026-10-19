#include <audio/source_recovery.h>
#include <cassert>
#include <vector>

int main() {
    audio::SourceRecovery recovery;
    std::vector<int> calls;
    bool available = false;
    auto stop = [&] { calls.push_back(1); };
    auto start = [&] { calls.push_back(2); return available; };
    assert(!recovery.poll(100, false, stop, start));
    assert(calls.empty());
    assert(!recovery.poll(100, true, stop, start));
    assert((calls == std::vector<int>{1, 2}));
    available = true;
    assert(!recovery.poll(1099, true, stop, start));
    assert(calls.size() == 2);
    assert(recovery.poll(1100, true, stop, start));
    assert((calls == std::vector<int>{1, 2, 1, 2}));
    assert(!recovery.poll(2100, false, stop, start));
    assert(calls.size() == 4);
    assert(recovery.poll(2100, true, stop, start));
}
