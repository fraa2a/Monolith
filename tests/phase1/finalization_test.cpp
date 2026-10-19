#include <recording/finalization.h>
#include <cassert>
#include <stdexcept>
int main() {
    recording::FinalizationState state;
    int stops = 0, attempts = 0;
    auto stop = [&] { ++stops; return std::wstring(L"session.mkv"); };
    auto publish = [&](const std::wstring& path) {
        assert(path == L"session.mkv");
        if (++attempts == 1) throw std::runtime_error("journal unavailable");
    };
    try { recording::finalize_once(state, stop, publish); assert(false); }
    catch (const std::runtime_error&) {}
    assert(state.finalized && state.pending() && stops == 1);
    recording::finalize_once(state, stop, publish);
    assert(!state.pending() && stops == 1 && attempts == 2);
    recording::finalize_once(state, stop, publish);
    assert(stops == 1 && attempts == 2);
}
