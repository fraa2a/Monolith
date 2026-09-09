#include <replay-buffer/packet_ring.h>
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using replay_buffer::detail::PacketRing;
using encoding::EncodedPacket;
EncodedPacket packet(int64_t dts, bool key = false, int stream = 0, size_t size = 100) {
    EncodedPacket p;
    auto bytes = std::make_shared<encoding::EncodedBytes>();
    bytes->size = size;
    p.bytes = bytes; p.dts_usec = dts; p.is_keyframe = key; p.stream_index = stream;
    return p;
}
int main() {
    PacketRing r;
    const auto now = PacketRing::Clock::now();
    r.configure_limits(1, 2);
    for (int i = 0; i < 100000; ++i) {
        r.push_packet(packet(i, false, 1), now); // audio only / WGC failure
        r.push_packet(packet(i), now); // no keyframes
    }
    CHECK(r.ring.empty());
    r.push_packet(packet(0, true, 0, 600000), now);
    r.push_packet(packet(1, false, 0, 600000), now);
    CHECK(r.ring.empty()); // one oversized GOP; never wait for 3 keys
    r.push_packet(packet(2, true, 0, 600000), now);
    r.push_packet(packet(3, true, 0, 600000), now);
    CHECK(r.keyframes == 1 && r.ring.front().dts_usec == 3);
    r.push_packet(packet(4, false, 0, 1048577), now);
    r.push_packet(packet(5), now);
    CHECK(r.ring.empty()); // dropped video invalidates dependencies
    r.push_packet(packet(6, true, 0, 1048577), now);
    CHECK(r.ring.empty());
    r.push_packet(packet(7, true, 0, 1048576), now);
    CHECK(r.total_bytes == 1048576);
    r.push_packet(packet(8, false, 1, 1048577), now);
    CHECK(r.total_bytes == 1048576 && r.keyframes == 1); // oversized audio does not break video references
    r.push_packet(packet(8, false, 1), now);
    CHECK(r.ring.empty());

    r.clear_ring();
    r.push_packet(packet(0, true), now);
    r.push_packet(packet(1000000, true), now);
    r.push_packet(packet(2000001, false, 1), now);
    CHECK(r.keyframes == 1 && r.ring.front().dts_usec == 1000000);
    r.push_packet(packet(4000002, false, 1), now);
    CHECK(r.ring.empty()); // stalled video, audio continues
    r.clear_ring();
    r.push_packet(packet(0, true), now);
    r.push_packet(packet(0), now + std::chrono::seconds(3));
    CHECK(r.ring.empty()); // frozen timestamps bounded by residence
    r.push_packet(packet(0, true), now);
    r.trim(now + std::chrono::seconds(3));
    CHECK(r.ring.empty()); // later stats/save after all producers stop

    r.clear_ring();
    r.push_packet(packet(1000000, true), now);
    r.push_packet(packet(0, false, 1), now); // old packet behind a newer front
    r.push_packet(packet(2000001, true), now);
    CHECK(r.ring.size() == 1); // minimum DTS index, not front/back assumption
    r.push_packet(packet(-1000000, false, 1), now);
    CHECK(r.ring.size() == 1); // too-late audio rejected
    r.push_packet(packet(9000000, false, 1), now);
    CHECK(r.ring.empty()); // skew conservatively shortens replay, no reorder fix

    r.clear_ring(); r.configure_limits(1, 30);
    r.push_packet(packet(0, true, 0, 1), now);
    for (size_t i = 1; i < PacketRing::kMaxPackets; ++i) r.push_packet(packet(0, false, 1, 1), now);
    CHECK(r.ring.size() == PacketRing::kMaxPackets);
    r.push_packet(packet(0, false, 1, 1), now);
    CHECK(r.ring.empty());
    r.push_packet(packet(0, true, 0, 0), now);
    CHECK(r.ring.empty());

    r.push_packet(packet(0, true), now);
    auto snapshot = r.ring; // same ref-counted payload as save_clip
    auto held = snapshot.front().bytes;
    r.clear_ring();
    CHECK(r.total_bytes == 0 && held.use_count() == 2);
    r.configure_limits(0, 30); r.push_packet(packet(0, true), now); CHECK(r.ring.empty());
    r.configure_limits(-1, 30); r.push_packet(packet(0, true), now); CHECK(r.ring.empty());
    r.configure_limits(INT64_MAX, 30); r.push_packet(packet(0, true), now); CHECK(r.ring.empty());
    r.configure_limits(1, 0); r.push_packet(packet(0, true), now); CHECK(r.ring.empty());
    r.configure_limits(1, 30); r.push_packet(packet(INT64_MIN, true), now);
    r.push_packet(packet(INT64_MAX, true), now); CHECK(r.ring.size() == 1);
    r.configure_limits(0, 30); CHECK(r.ring.empty());
    std::cout << "PASS replay_packet_ring: keyframe starvation, byte/age/count bounds, skew, snapshots, invalid limits\n";
}
