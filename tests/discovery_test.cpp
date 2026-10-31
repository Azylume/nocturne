// Discovery without an engine, for platforms other than macOS (whose Bonjour case is in sync_test.cpp): an advertised
// engine is found with its port and TXT record, and a role change comes through.

#include "sync/discovery.hpp"

#include <doctest.h>

#include <chrono>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace {

bool waitUntil(const std::function<bool()>& done, double seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

} // namespace

TEST_CASE("discovery: an advertised engine is found with its port and role, and its role change is seen")
{
    const std::string name = "nocturne-test-" + std::to_string(std::random_device{}() % 1000000);
    azl::Advertiser advertiser(name, 45123, {{"role", "main"}});
    azl::Browser browser;
    auto role = [&]() -> std::string {
        for (const azl::Service& s : browser.services())
            if (s.name == name && s.port == 45123)
                return s.txt.count("role") ? s.txt.at("role") : "";
        return "";
    };
    REQUIRE_MESSAGE(waitUntil([&] { return role() == "main"; }, 10.0), "not discovered");
    advertiser.update({{"role", "node"}});
    CHECK(waitUntil([&] { return role() == "node"; }, 10.0));
    CHECK_FALSE(azl::machineName().empty());
}
