#include <doctest.h>

#include "platform/work_queue.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <class Done>
bool waitFor(Done done, std::chrono::milliseconds limit = 2000ms)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return done();
}

} // namespace

TEST_CASE("platform: a work queue runs its tasks one at a time, in order, off the calling thread")
{
    azl::WorkQueue queue("nocturne.test.queue");
    std::vector<int> order;
    std::atomic<int> running{0};
    std::atomic<bool> overlapped{false}, onCaller{false};
    const std::thread::id caller = std::this_thread::get_id();
    for (int i = 0; i < 200; ++i)
        queue.async([&, i] {
            if (running.fetch_add(1) != 0)
                overlapped = true;
            if (std::this_thread::get_id() == caller)
                onCaller = true;
            order.push_back(i);
            running.fetch_sub(1);
        });
    queue.flush();
    REQUIRE(order.size() == 200);
    for (int i = 0; i < 200; ++i)
        CHECK(order[size_t(i)] == i);
    CHECK_FALSE(overlapped);
    CHECK_FALSE(onCaller);
}

TEST_CASE("platform: a work queue runs every queued task before its destructor returns")
{
    std::atomic<int> ran{0};
    {
        azl::WorkQueue queue("nocturne.test.drain", azl::ThreadRole::Background);
        queue.async([] { std::this_thread::sleep_for(20ms); });
        for (int i = 0; i < 50; ++i)
            queue.async([&] { ++ran; });
    }
    CHECK(ran == 50);
}

TEST_CASE("platform: a queue timer fires after its delay, repeats, and never runs once destroyed")
{
    azl::WorkQueue queue("nocturne.test.timer");
    std::atomic<int> ticks{0};
    auto timer = std::make_unique<azl::QueueTimer>(queue, 5ms, 5ms, [&] { ++ticks; });
    CHECK(waitFor([&] { return ticks >= 3; }));
    timer.reset();
    const int stopped = ticks;
    std::this_thread::sleep_for(50ms);
    CHECK(ticks == stopped);

    std::atomic<int> once{0};
    azl::QueueTimer single(queue, 5ms, 0ns, [&] { ++once; });
    CHECK(waitFor([&] { return once == 1; }));
    std::this_thread::sleep_for(50ms);
    CHECK(once == 1);

    std::atomic<int> early{0};
    std::make_unique<azl::QueueTimer>(queue, 30ms, 0ns, [&] { ++early; }).reset();
    std::this_thread::sleep_for(80ms);
    CHECK(early == 0);
}

TEST_CASE("platform: parallelFor calls the body once for every index and returns when all are done")
{
    std::vector<std::atomic<int>> calls(1000);
    azl::parallelFor(calls.size(), azl::ThreadRole::Decode, [&](size_t i) { ++calls[i]; });
    for (const std::atomic<int>& c : calls)
        CHECK(c == 1);
    int none = 0;
    azl::parallelFor(0, azl::ThreadRole::Background, [&](size_t) { ++none; });
    CHECK(none == 0);
}
