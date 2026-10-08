#include <encoding/encoding.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using namespace std::chrono_literals;

int main() {
    encoding::VideoEncoder video;
    encoding::VideoEncoder::Config cfg;
    CHECK(!video.open(cfg, {}));
    cfg.width = 64; cfg.height = 64; cfg.preferred_encoder = "libx264";
    cfg.codec = encoding::VideoCodec::AV1;
    if (video.open(cfg, {})) {
        CHECK(video.stream_params().codec == encoding::VideoCodec::AV1);
        video.close();
    }
    cfg.codec = encoding::VideoCodec::H264;
    int packets = 0;
    CHECK(video.open(cfg, [&](auto) { ++packets; }));
    std::vector<uint8_t> bgra(64 * 64 * 4);
    video.push_bgra(bgra.data(), 1, 64, 64);
    CHECK(video.perf_stats().frames_submitted == 0);
    video.push_bgra(bgra.data(), 64 * 4, 64, 64);
    video.flush(); CHECK(packets > 0); video.close();

    encoding::TrackMixer mixer;
    CHECK(!mixer.open(48000, 65, {}));
    std::promise<void> called;
    CHECK(mixer.open(48000, 2, [&](auto, auto, auto, auto, auto, auto) {
        called.set_value(); throw std::runtime_error("sink failed");
    }));
    CHECK(called.get_future().wait_for(2s) == std::future_status::ready);
    for (int i = 0; mixer.is_open() && i < 100; ++i) std::this_thread::sleep_for(1ms);
    CHECK(!mixer.is_open()); mixer.close();

    std::atomic<int> callbacks{0};
    std::atomic<bool> invalid{false};
    CHECK(mixer.open(48000, 2, [&](const uint8_t* data, int bytes, int rate, int channels, int bits, bool is_float) {
        if (bytes <= 0 || bytes > 4800 * 2 * 4 || rate != 48000 || channels != 2 || bits != 32 || !is_float)
            invalid = true;
        const auto* samples = reinterpret_cast<const float*>(data);
        for (int i = 0; i < bytes / 4; ++i)
            if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 1.0f) invalid = true;
        ++callbacks;
    }));
    const auto muted_source = mixer.add_source(std::numeric_limits<float>::quiet_NaN());
    CHECK(muted_source >= 0);
    const auto source = mixer.add_source(); CHECK(source >= 0);
    mixer.set_source_gain(source, std::numeric_limits<float>::infinity());
    std::vector<float> burst(96000 * 2, 0.5f);
    mixer.push(source, reinterpret_cast<const uint8_t*>(burst.data()), static_cast<int>(burst.size() * 4),
        48000, 2, 32, true);
    for (int i = 0; callbacks < 3 && i < 500; ++i) std::this_thread::sleep_for(1ms);
    mixer.close(); CHECK(callbacks >= 3 && !invalid);
}
