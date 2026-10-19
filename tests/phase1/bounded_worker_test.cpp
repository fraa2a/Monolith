#include <recording/bounded_worker.h>
#include <atomic>
#include <cassert>
#include <future>
#include <vector>
int main() {
    std::atomic<int> errors{0};
    recording::BoundedWorker worker(2, 10, [&] { ++errors; });
    std::promise<void> entered, release;
    auto wait = release.get_future().share();
    assert(worker.submit([&] { entered.set_value(); wait.wait(); }, 1));
    entered.get_future().wait();
    std::vector<int> order;
    assert(worker.submit([&] { order.push_back(1); }, 6));
    assert(!worker.submit([] {}, 5));
    assert(worker.submit([&] { order.push_back(2); }, 4));
    assert(!worker.submit([] {}, 0));
    assert(worker.submit_control([&] { order.push_back(9); }));
    release.set_value(); worker.drain();
    assert((order == std::vector<int>{1, 2, 9}));
    assert(worker.submit([] { throw 1; })); worker.drain(); assert(errors == 1);
    assert(worker.submit([&] { order.push_back(3); })); worker.close();
    assert(order.back() == 3 && !worker.submit([] {}));
    recording::BoundedWorker producer(4, 10), control(1, 0);
    std::promise<void> slow_entered, slow_release, complete;
    auto slow_wait = slow_release.get_future().share(), completed = complete.get_future().share();
    std::atomic<int> packets{0};
    assert(producer.submit([&] { slow_entered.set_value(); slow_wait.wait(); ++packets; }, 5));
    slow_entered.get_future().wait(); assert(producer.submit([&] { ++packets; }, 5));
    assert(control.submit([&] { producer.drain(); complete.set_value(); }));
    assert(completed.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    slow_release.set_value();
    assert(completed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(packets == 2); control.close(); producer.close();
}
