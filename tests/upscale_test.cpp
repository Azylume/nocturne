#include "golden.hpp"
#include "test_client.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
constexpr azl::EngineConfig kFsr{.width = 960, .height = 540, .fsrUpscale = true};

// A budget no frame meets, so the guard draws every layer it may at half resolution; 0 leaves the guard off.
constexpr double kOverrun = 1e-6;

azl::Image screenAfter(azl::EngineHarness& harness, double budget, int frames = 60)
{
    if (budget > 0.0)
        harness.engine().setFrameBudget(budget);
    harness.step(frames);
    return harness.readScreen();
}

double meanDifference(const azl::Image& a, const azl::Image& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        sum += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
    return sum / double(a.rgba.size());
}

Message edit(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

} // namespace

TEST_CASE("the frame guard's half-resolution test card scales up bilinearly, or with FSR 1 when set")
{
    azl::EngineHarness full(kSize);
    azl::EngineHarness fsr(kFsr);
    azl::EngineHarness bilinear(kSize);
    const azl::Image reference = screenAfter(full, 0.0);
    const azl::Image sharp = screenAfter(fsr, kOverrun);
    const azl::Image soft = screenAfter(bilinear, kOverrun);
    CHECK(fsr.stats().budgetLevel == 1);
    CHECK(bilinear.stats().budgetLevel == 1);
    checkGolden(sharp, "upscale_fsr");
    checkGolden(soft, "upscale_bilinear");
    CHECK(sharp.rgba != soft.rgba);
    const double sharpError = meanDifference(sharp, reference);
    const double softError = meanDifference(soft, reference);
    MESSAGE("mean difference from full resolution: FSR " << sharpError << ", bilinear " << softError);
    CHECK(sharpError < softError);
}

// The upscale is a step of the guard only: while it draws nothing at half resolution, FSR and bilinear engines draw
// what an engine without a budget does, byte for byte.
TEST_CASE("without the frame guard engaged the picture is the same with FSR 1, bilinear or no budget")
{
    azl::EngineHarness plain(kSize);
    azl::EngineHarness fsrInBudget(kFsr);
    azl::EngineHarness bilinearInBudget(kSize);
    azl::EngineHarness guardOff(kFsr);
    Client client(guardOff.document());
    REQUIRE(edit(client, azl::SetFrameGuard{false})["on"] == false);

    const azl::Image reference = screenAfter(plain, 0.0);
    CHECK(screenAfter(fsrInBudget, 1.0).rgba == reference.rgba);
    CHECK(screenAfter(bilinearInBudget, 1.0).rgba == reference.rgba);
    CHECK(screenAfter(guardOff, kOverrun).rgba == reference.rgba);
    CHECK(fsrInBudget.stats().budgetLevel == 0);
    CHECK(bilinearInBudget.stats().budgetLevel == 0);
    CHECK(guardOff.stats().budgetLevel == 0);
}

// Not a pass/fail check: GPU time per frame at 1080p of five layers with the reference show's chains, at full resolution
// and with the guard drawing all five at half resolution, upscaled with FSR 1 and bilinearly.
TEST_CASE("GPU time of the half-resolution upscale, FSR 1 against bilinear")
{
    const auto measure = [](azl::EngineConfig config, double budget, uint64_t& level) {
        TempDir media;
        azl::EngineHarness harness(config);
        Client client(harness.document());
        const std::string clip = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
        const std::vector<std::vector<std::string>> chains{
            {}, {"colorize"}, {"colorize", "edgeDetection", "trails"}, {"stripper"}, {"colorize"}};
        for (uint64_t layer = 1; layer <= chains.size(); ++layer) {
            if (layer > 1)
                REQUIRE(edit(client, azl::AddLayer{0, std::nullopt, {.id = layer}})["op"] == "addLayer");
            REQUIRE(edit(client, azl::SetSlot{layer, 1, azl::ClipRef{clip, "clip", {}}})["op"] == "setSlot");
            for (const std::string& type : chains[layer - 1])
                REQUIRE(edit(client, azl::AddEffect{layer, std::nullopt, {0, type, {}}})["op"] == "addEffect");
        }
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
        REQUIRE(client.next()["op"] == "triggerColumn");
        screenAfter(harness, budget, 120);
        level = harness.stats().budgetLevel;
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(240);
        return double(harness.stats().gpuMicros - before) / 240 / 1000.0;
    };
    uint64_t fullLevel = 0, fsrLevel = 0, bilinearLevel = 0;
    const double full = measure({.width = 1920, .height = 1080}, 0.0, fullLevel);
    const double fsr = measure({.width = 1920, .height = 1080, .fsrUpscale = true}, kOverrun, fsrLevel);
    const double bilinear = measure({.width = 1920, .height = 1080}, kOverrun, bilinearLevel);
    MESSAGE("GPU ms/frame at 1080p, 5 layers: full resolution " << full << "; " << fsrLevel
                                                                << " at half resolution, FSR 1 " << fsr << ", bilinear "
                                                                << bilinear << " (FSR costs " << fsr - bilinear
                                                                << " ms, " << (fsr - bilinear) / double(fsrLevel)
                                                                << " per layer)");
    CHECK(fsrLevel == 5);
    CHECK(bilinearLevel == 5);
}

// With NOCTURNE_UPSCALE_AB=DIR and NOCTURNE_UPSCALE_AB_CLIPS=FILE[:FILE...]: for each clip, DIR/NAME-full.png,
// DIR/NAME-fsr.png and DIR/NAME-bilinear.png at 1080p, the clip at full resolution and at half resolution upscaled
// both ways, for judging the picture.
TEST_CASE("A/B pictures of clips at half resolution, FSR 1 against bilinear" *
          doctest::skip(!std::getenv("NOCTURNE_UPSCALE_AB")))
{
    const fs::path out = std::getenv("NOCTURNE_UPSCALE_AB");
    const char* list = std::getenv("NOCTURNE_UPSCALE_AB_CLIPS");
    REQUIRE(list);
    fs::create_directories(out);
    std::stringstream clips(list);
    for (std::string clip; std::getline(clips, clip, ':');) {
        const auto picture = [&](azl::EngineConfig config, double budget) {
            azl::EngineHarness harness(config);
            Client client(harness.document());
            playMedia(client, clip);
            return screenAfter(harness, budget, 120);
        };
        const std::string name = fs::path(clip).stem().string();
        savePng(picture({.width = 1920, .height = 1080}, 0.0), (out / (name + "-full.png")).string());
        savePng(picture({.width = 1920, .height = 1080, .fsrUpscale = true}, kOverrun),
                (out / (name + "-fsr.png")).string());
        savePng(picture({.width = 1920, .height = 1080}, kOverrun), (out / (name + "-bilinear.png")).string());
    }
}
