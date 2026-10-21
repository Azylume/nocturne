#include "golden.hpp"

#include <doctest.h>

TEST_CASE("the screen shows the test card")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.step(30);
    checkGolden(harness.readScreen(), "test_card");
}

TEST_CASE("the test card marker moves with the injected clock")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.step();
    const azl::Image first = harness.readScreen();
    harness.step(10);
    CHECK(harness.readScreen().rgba != first.rgba);
}
