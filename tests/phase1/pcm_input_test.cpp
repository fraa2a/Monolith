#include <encoding/pcm_input.h>
#include <array>
#include <iostream>
#include <stdexcept>

void check(bool condition) { if (!condition) throw std::runtime_error("PCM contract failed"); }
int main() {
    encoding::detail::PcmInput pcm;
    check(!pcm.prepare(nullptr, 6, 2, 1, false));
    check(!pcm.prepare(nullptr, 6, 0, 24, false));
    check(!pcm.prepare(nullptr, 5, 2, 24, false));
    check(!pcm.prepare(nullptr, 6, 2, 24, true));
    check(pcm.prepare(nullptr, 4, 2, 8, false));
    check(pcm.data[0] == 128 && pcm.frames == 2);
    std::array<uint8_t, 9> samples{0xff, 0xff, 0x7f, 0, 0, 0x80, 0xff, 0xff, 0xff};
    check(pcm.prepare(samples.data(), 9, 1, 24, false));
    check(pcm.bit_depth == 32 && pcm.frames == 3);
    check(pcm.expanded[0] == 0x7fffff00);
    check(pcm.expanded[1] == INT32_MIN && pcm.expanded[2] == -256);
    check(pcm.prepare(nullptr, 6, 2, 24, false));
    check(pcm.expanded[0] == 0 && pcm.expanded[1] == 0);
    std::cout << "PCM format and packed sample tests passed\n";
}
