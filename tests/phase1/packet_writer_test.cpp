#include <recording/packet_writer.h>
#include <cassert>
#include <future>
#include <vector>
using namespace std::chrono_literals;
int main() {
    std::promise<void> entered, release;
    auto wait = release.get_future().share();
    std::vector<int> order;
    recording::PacketWriter writer([&](encoding::EncodedPacket p) {
        if (p.pts == 0) { entered.set_value(); wait.wait(); }
        order.push_back(static_cast<int>(p.pts));
    }, 2, 8);
    auto packet = [](int n, size_t size = 4) {
        encoding::EncodedPacket p; p.pts = n;
        auto bytes = std::make_shared<encoding::EncodedBytes>(); bytes->size = size;
        p.bytes = std::move(bytes); return p;
    };
    assert(writer.push(packet(0)));
    entered.get_future().wait();
    assert(writer.push(packet(1)) && writer.push(packet(2)));
    assert(!writer.push(packet(3)) && writer.failed());
    auto stop = writer.control([&] { order.push_back(9); return 42; });
    assert(stop.wait_for(0ms) == std::future_status::timeout);
    assert(!writer.push(packet(4)));
    release.set_value();
    assert(stop.get() == 42);
    assert((order == std::vector<int>{0, 1, 2, 9}));
    writer.reset(); assert(!writer.failed());
    assert(writer.push(packet(5)));
    writer.close();
    assert(order.back() == 5 && !writer.push(packet(6)));
    recording::PacketWriter throwing([](auto) { throw 1; });
    assert(throwing.push(packet(0)));
    throwing.control([] {}).get();
    assert(throwing.failed());
}
