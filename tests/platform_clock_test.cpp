#include <doctest.h>

#include "platform/clock.hpp"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("platform: the host clock runs forward at the steady clock's rate")
{
    const auto steadyStart = std::chrono::steady_clock::now();
    const double hostStart = azl::hostSeconds();
    std::this_thread::sleep_for(50ms);
    const double host = azl::hostSeconds() - hostStart;
    const double steady = std::chrono::duration<double>(std::chrono::steady_clock::now() - steadyStart).count();
    CHECK(host >= 0.05);
    CHECK(host == doctest::Approx(steady).epsilon(0.1));
}
