// The UDP clock on its own, without an engine: Network.framework on macOS, plain sockets elsewhere and in
// nocturne_socket_tests.

#include "sync/udp_clock.hpp"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace {

double steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool offsetWithin(const azl::ClockPinger& pinger, double expected, double seconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (const auto offset = pinger.offsetAt(steadySeconds()); offset && std::abs(*offset - expected) < 0.0005)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

TEST_CASE("udp clock: one responder answers pingers by host name, IPv4 and IPv6 at once")
{
    azl::ClockResponder responder(0, [] { return steadySeconds() - 42.0; });
    azl::ClockPinger byName("localhost", responder.port(), steadySeconds);
    azl::ClockPinger byIpv4("127.0.0.1", responder.port(), steadySeconds);
    azl::ClockPinger byIpv6("::1", responder.port(), steadySeconds);
    CHECK(offsetWithin(byName, -42.0, 5.0));
    CHECK(offsetWithin(byIpv4, -42.0, 5.0));
    CHECK(offsetWithin(byIpv6, -42.0, 5.0));
    CHECK(byIpv4.roundTrip() < 0.01);
}

TEST_CASE("udp clock: a pinger whose responder is not there yet finds it once it starts")
{
    uint16_t port = 0;
    {
        azl::ClockResponder probe(0, steadySeconds);
        port = probe.port();
    }
    azl::ClockPinger pinger("127.0.0.1", port, steadySeconds);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(pinger.offsetAt(steadySeconds()));
    azl::ClockResponder responder(port, [] { return steadySeconds() + 7.0; });
    CHECK(offsetWithin(pinger, 7.0, 5.0));
}
