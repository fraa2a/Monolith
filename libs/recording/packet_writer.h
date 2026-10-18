#pragma once
#include "bounded_worker.h"
#include <encoding/encoding.h>
#include <atomic>
#include <future>
#include <stdexcept>

namespace recording {
class PacketWriter {
    encoding::PacketSink sink_;
    std::atomic<bool> failed_{false};
    BoundedWorker worker_;
public:
    explicit PacketWriter(encoding::PacketSink sink, size_t packets = 4096,
                          size_t bytes = 64 * 1024 * 1024)
        : sink_(std::move(sink)), worker_(packets, bytes, [this] { failed_ = true; }) {}
    bool push(encoding::EncodedPacket packet) {
        if (failed_) return false;
        const auto bytes = packet.size();
        if (worker_.submit([this, packet = std::move(packet)] { sink_(packet); }, bytes))
            return true;
        failed_ = true;
        return false;
    }
    template<class Fn>
    auto control(Fn fn) {
        using Task = std::packaged_task<decltype(fn())()>;
        auto task = std::make_shared<Task>(std::move(fn));
        auto future = task->get_future();
        if (!worker_.submit_control([task] { (*task)(); }))
            throw std::runtime_error("Packet writer control queue is full or closed");
        return future;
    }
    bool failed() const { return failed_.load(); }
    // Reset only with producers stopped; the barrier drains the old session.
    void reset() { control([this] { failed_ = false; }).get(); }
    void close() { worker_.close(); }
};
}
