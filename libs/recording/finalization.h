#pragma once
#include <string>
#include <utility>
namespace recording {
struct FinalizationState {
    std::wstring path;
    bool finalized = false;
    bool published = false;
    bool pending() const { return !published; }
};
template<class Stop, class Publish>
void finalize_once(FinalizationState& state, Stop stop, Publish publish) {
    if (!state.finalized) {
        state.path = stop();
        state.finalized = true;
    }
    if (!state.published) {
        publish(state.path);
        state.published = true;
    }
}
}
