#include "golden.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>

// Stills of a show for before/after comparisons of a build change, rendered deterministically (every clip's frame is
// waited for). Skipped unless the environment names a show:
//   NOCTURNE_STILL_SHOW=FILE NOCTURNE_STILL_MEDIA=DIR NOCTURNE_STILL_OUT=FILE.png [NOCTURNE_STILL_FRAMES=N]
//   NOCTURNE_STILL_COMPARE=OTHER.png also prints how the still differs from OTHER.png.
//   NOCTURNE_STILL_OPAQUE=ID[,ID...] sets those layers' opacity to 1 (layers an audio modulator opens up, silent here).
//   NOCTURNE_STILL_COLUMN=ID triggers that column at once.
TEST_CASE("a still of a show at a frame")
{
    const char* show = std::getenv("NOCTURNE_STILL_SHOW");
    const char* out = std::getenv("NOCTURNE_STILL_OUT");
    if (!show || !out) {
        MESSAGE("skipped: set NOCTURNE_STILL_SHOW, NOCTURNE_STILL_MEDIA and NOCTURNE_STILL_OUT");
        return;
    }
    const char* media = std::getenv("NOCTURNE_STILL_MEDIA");
    const char* frames = std::getenv("NOCTURNE_STILL_FRAMES");
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    Message load{{"type", "composition"}, {"op", "load"}, {"path", show}};
    if (media)
        load["mediaRoot"] = media;
    client.send(load);
    REQUIRE(client.next()["type"] == "composition");
    REQUIRE(client.next()["op"] == "loadComposition");
    if (const char* opaque = std::getenv("NOCTURNE_STILL_OPAQUE"))
        for (const char* at = opaque; *at; at += *at == ',') {
            char* end = nullptr;
            const uint64_t layer = std::strtoull(at, &end, 10);
            client.send(msg::command(azl::SetLayerOpacity{layer, 1.0}));
            at = end;
        }
    if (const char* column = std::getenv("NOCTURNE_STILL_COLUMN")) {
        client.send(msg::command(azl::SetQuantise{0}));
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", std::strtoull(column, nullptr, 10)}});
    }
    harness.step(frames ? std::atoi(frames) : 240);
    const azl::Image still = harness.readScreen();
    savePng(still, out);

    if (const char* other = std::getenv("NOCTURNE_STILL_COMPARE")) {
        const std::optional<azl::Image> before = loadPng(other);
        REQUIRE(before);
        REQUIRE(before->rgba.size() == still.rgba.size());
        size_t differing = 0, over8 = 0;
        int largest = 0;
        double sum = 0.0;
        for (size_t i = 0; i < still.rgba.size(); i += 4) {
            int worst = 0;
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(int(still.rgba[i + c]) - int(before->rgba[i + c])));
            differing += worst > 0;
            over8 += worst > 8;
            largest = std::max(largest, worst);
            sum += worst;
        }
        const size_t pixels = still.rgba.size() / 4;
        MESSAGE("still differs in " << differing << " of " << pixels << " pixels (" << 100.0 * differing / pixels
                                    << " %), " << over8 << " by more than 8 levels, largest " << largest
                                    << ", mean " << sum / pixels);
    }
}
