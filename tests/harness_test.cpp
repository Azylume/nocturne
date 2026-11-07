#include "golden.hpp"

#include <doctest.h>

#include <stdexcept>

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

namespace {

struct ThrowingTap : azl::FrameTap {
    Done encode(azl::gpu::CommandBuffer&, const azl::Frame&, const azl::Composition&, double) override
    {
        throw std::runtime_error("injected");
    }
};

} // namespace

TEST_CASE("a frame that throws is dropped, the last good frame stays and rendering goes on")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    harness.step(5);
    const azl::Image good = harness.readScreen();
    const uint64_t rendered = harness.stats().rendered;

    harness.engine().setFrameTap(std::make_shared<ThrowingTap>());
    CHECK_NOTHROW(harness.step(3));
    CHECK(harness.stats().rendered == rendered);
    CHECK(harness.readScreen().rgba == good.rgba);

    harness.engine().setFrameTap(nullptr);
    harness.step(3);
    CHECK(harness.stats().rendered == rendered + 3);
}
