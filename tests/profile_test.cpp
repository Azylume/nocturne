#include "harness/engine_harness.hpp"
#include "platform/profile.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#define AZL_TEXT(...) AZL_TEXT_OF(__VA_ARGS__)
#define AZL_TEXT_OF(...) #__VA_ARGS__

namespace {

#ifdef _WIN32
// Mentions of Tracy in the engine binary: a Windows executable carries no symbol names, but Tracy's client brings
// strings of its own (TRACY_PORT and others).
int tracySymbols()
{
    std::ifstream in(bundleBinary("nocturne"), std::ios::binary);
    REQUIRE(in);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::transform(bytes.begin(), bytes.end(), bytes.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    int count = 0;
    for (size_t at = bytes.find("tracy"); at != std::string::npos; at = bytes.find("tracy", at + 1))
        ++count;
    return count;
}
#else
// Symbols of the engine binary, both architectures, whose name mentions Tracy.
int tracySymbols()
{
    const std::string command = "nm -j '" + bundleBinary("nocturne").string() + "' | grep -ci tracy";
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    int count = -1;
    CHECK(std::fscanf(pipe, "%d", &count) == 1);
    pclose(pipe);
    return count;
}
#endif

} // namespace

#ifndef NOCTURNE_TRACY

#ifdef TRACY_ENABLE
#error "the default build must not enable Tracy"
#endif

static_assert(!azl::profile::kEnabled);
static_assert(std::string_view(AZL_TEXT(AZL_ZONE("zone") AZL_FRAME_MARK AZL_FRAME_MARK_NAMED("frames")
                                            AZL_THREAD_NAME("thread") AZL_GPU_ZONE(timeline, desc, "encoder")))
                  .empty());

TEST_CASE("the default build links no Tracy code into the engine")
{
    CHECK(tracySymbols() == 0);
}

#else

static_assert(azl::profile::kEnabled);

TEST_CASE("a Tracy build renders and presents with no profiler connected")
{
    CHECK(tracySymbols() > 0);
    azl::Composition composition = azl::defaultComposition();
    composition.screens = {{.id = 1, .name = "Main", .display = "window-1", .slices = {{.id = 1}}}};
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, composition);
    harness.displays().attach({"window-1", "window-1", 960, 540, 60.0});
    harness.step(3);
    const uint64_t rendered = harness.stats().rendered;
    harness.runLive(0.5);
    CHECK(harness.stats().rendered - rendered > 10);
    for (azl::DisplayOutput* output : harness.outputs().all())
        CHECK(output->stats().presents > 10);
    CHECK(harness.outputs().all().size() == 1);
}

#endif
