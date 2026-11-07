#include "platform/atomic_max.hpp"

#include <doctest.h>

#include <thread>
#include <vector>

TEST_CASE("atomicMax keeps the largest value raised from four threads at once")
{
    constexpr uint64_t kPerThread = 100000;
    std::atomic<uint64_t> max{0};
    std::vector<std::thread> threads;
    for (uint64_t t = 0; t < 4; ++t)
        threads.emplace_back([&max, t] {
            for (uint64_t i = 1; i <= kPerThread; ++i)
                azl::atomicMax(max, i * 4 + t);
        });
    for (auto& thread : threads)
        thread.join();
    CHECK(max.load() == kPerThread * 4 + 3);
}

TEST_CASE("atomicMax never lowers the value")
{
    std::atomic<uint64_t> max{10};
    azl::atomicMax(max, 5);
    CHECK(max.load() == 10);
}
