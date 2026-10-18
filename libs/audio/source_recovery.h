#pragma once
#include <cstdint>

namespace audio {
class SourceRecovery {
    uint64_t next_attempt_ = 0;
public:
    template<class Stop, class Start>
    bool poll(uint64_t now, bool needed, Stop stop, Start start) {
        if (!needed || now < next_attempt_) return false;
        next_attempt_ = now + 1000;
        stop();
        return start();
    }
};
}
