#include "engine/static_bake.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

using Action = azl::StaticBake::Action;

const azl::EffectDef& effect(const std::string& type)
{
    const azl::EffectDef* def = azl::findEffect(type);
    REQUIRE(def);
    return *def;
}

std::vector<const azl::PlannedPass*> pointers(const std::vector<azl::PlannedPass>& passes)
{
    std::vector<const azl::PlannedPass*> list;
    for (const azl::PlannedPass& pass : passes)
        list.push_back(&pass);
    return list;
}

size_t prefixOf(azl::SourceKind source, const std::vector<std::string>& types,
                const std::function<bool(const azl::PlannedStage&)>& driven = [](const azl::PlannedStage&) {
                    return false;
                })
{
    std::vector<const azl::EffectDef*> chain;
    for (const std::string& type : types)
        chain.push_back(&effect(type));
    return azl::staticPrefix(pointers(azl::planChain(source, chain)), driven);
}

azl::Composition chainOn(std::vector<azl::Effect> effects)
{
    azl::Composition composition = azl::defaultComposition();
    composition.layers[0].effects = std::move(effects);
    return composition;
}

// Edge, Edge + Colorize: the source and the first Edge are static intermediates, the last pass blends.
std::vector<azl::Effect> edges()
{
    return {{1, "edgeDetection", {{"multiplier", 0.6}}}, {2, "edgeDetection", {{"multiplier", 0.5}}},
            {3, "colorize", {{"hue", 0.3}}}};
}

// The same with a Shift Glitch (a remap whose bands follow the beat clock) fused after the first Edge: only the source
// pass is static.
std::vector<azl::Effect> edgesWithGlitch()
{
    return {{1, "edgeDetection", {{"multiplier", 0.6}}}, {5, "classicShiftGlitch", {{"frequency", 0.5}}},
            {2, "edgeDetection", {{"multiplier", 0.5}}}, {3, "colorize", {{"hue", 0.3}}}};
}

azl::Modulator lfoOn(uint64_t effectId, const std::string& param)
{
    azl::Modulator m;
    m.shape = azl::LfoShape::Sine;
    m.beats = 1.0;
    m.target = {0, {1, effectId, param}};
    return m;
}

// The same composition on an engine that bakes and one that does not, edited alike and stepped frame by frame.
struct Pair {
    TempDir media;
    azl::EngineHarness baked;
    azl::EngineHarness live;
    Client bakedClient;
    Client liveClient;

    Pair(const azl::Composition& composition, azl::Transport transport = {})
        : baked({.width = 960, .height = 540, .bakeStatic = true}, 60.0, {}, composition),
          live({.width = 960, .height = 540}, 60.0, {}, composition), bakedClient(baked.document()),
          liveClient(live.document())
    {
        const fs::path clip = copyMedia("count_709_tv.mp4", media.path / "clip.mp4");
        playMedia(bakedClient, clip, transport);
        playMedia(liveClient, clip, transport);
    }

    // The change the edit caused on the baking engine.
    Message edit(const azl::Edit& e)
    {
        apply(liveClient, e);
        return apply(bakedClient, e);
    }

    // Steps both; the pictures must match byte for byte. Returns the passes the baking engine took from its bakes.
    uint64_t step()
    {
        baked.step();
        live.step();
        const azl::Image a = baked.readScreen();
        const azl::Image b = live.readScreen();
        CHECK(a.rgba == b.rgba);
        const uint64_t fromBakes = baked.stats().bakedPasses;
        CHECK(baked.stats().passes + fromBakes == live.stats().passes);
        CHECK(live.stats().bakedPasses == 0);
        return fromBakes;
    }
};

} // namespace

TEST_CASE("static bake: what counts as static")
{
    CHECK(azl::clockFree(effect("colorize")));
    CHECK(azl::clockFree(effect("edgeDetection")));
    CHECK_FALSE(azl::clockFree(effect("trails")));
    CHECK_FALSE(azl::clockFree(effect("classicTrails")));
    CHECK_FALSE(azl::clockFree(effect("classicStrobe")));
    CHECK_FALSE(azl::clockFree(effect("classicShiftGlitch")));

    using azl::SourceKind;
    CHECK(prefixOf(SourceKind::Yuv, {"edgeDetection", "colorize"}) == 1);
    CHECK(prefixOf(SourceKind::Yuv, {"edgeDetection", "edgeDetection", "colorize"}) == 2);
    CHECK(prefixOf(SourceKind::Yuv, {"colorize"}) == 0);
    CHECK(prefixOf(SourceKind::Text, {"edgeDetection"}) == 1);
    CHECK(prefixOf(SourceKind::TestCard, {"edgeDetection", "colorize"}) == 0);
    CHECK(prefixOf(SourceKind::Group, {"edgeDetection"}) == 0);
    CHECK(prefixOf(SourceKind::YuvFade, {"edgeDetection"}) == 0);
    CHECK(prefixOf(SourceKind::Yuv, {"edgeDetection", "trails", "edgeDetection"}) == 1);
    CHECK(prefixOf(SourceKind::Yuv, {"edgeDetection", "edgeDetection", "colorize"},
                   [](const azl::PlannedStage& stage) { return stage.chainIndex == 0; }) == 1);
}

TEST_CASE("static bake: bakes once its inputs settle and reuses while its picture stays the same")
{
    azl::StaticBake bake;
    const std::vector<uint32_t> inputs{1, 2, 3};
    const azl::StaticBake::Source still{&bake, nullptr, nullptr, nullptr};
    const azl::StaticBake::Source next{&inputs, nullptr, nullptr, nullptr};
    for (int i = 1; i < azl::StaticBake::kSettleFrames; ++i)
        CHECK(bake.step(1, inputs, still) == Action::Live);
    CHECK(bake.step(1, inputs, still) == Action::Bake);
    CHECK(bake.step(1, inputs, still) == Action::Reuse);
    CHECK(bake.step(1, inputs, next) == Action::Bake);
    CHECK(bake.step(1, inputs, next) == Action::Reuse);
    CHECK(bake.step(1, inputs, still) == Action::Bake);

    // Any other input (a parameter, the size, the prefix's make-up) starts the settling over.
    CHECK(bake.step(1, {1, 2, 4}, still) == Action::Live);
    for (int i = 2; i < azl::StaticBake::kSettleFrames; ++i)
        CHECK(bake.step(1, {1, 2, 4}, still) == Action::Live);
    CHECK(bake.step(1, {1, 2, 4}, still) == Action::Bake);
    CHECK(bake.step(2, {1, 2, 4}, still) == Action::Live);
    CHECK(bake.step(0, {}, still) == Action::Live);
    CHECK(bake.step(0, {}, still) == Action::Live);
}

TEST_CASE("a baked static prefix gives the same picture as drawing it, through edits and modulators coming and going")
{
    for (const bool glitch : {false, true}) {
        CAPTURE(glitch);
        Pair pair(chainOn(glitch ? edgesWithGlitch() : edges()));
        uint64_t reused = 0;
        auto run = [&](int frames) {
            uint64_t sum = 0;
            for (int i = 0; i < frames; ++i)
                sum += pair.step();
            reused += sum;
            return sum;
        };
        // A clip at 30 fps on a 60 fps engine: every other frame reads the same picture.
        CHECK(run(30) > 0);
        pair.edit(azl::SetEffectParam{1, "multiplier", 0.8});
        CHECK(pair.step() == 0);
        CHECK(run(20) > 0);
        const uint64_t modulator =
            pair.edit(azl::AddModulator{std::nullopt, lfoOn(1, "multiplier")})["modulator"]["id"].get<uint64_t>();
        // The modulated Edge and everything after it draw live; only the source pass can still bake.
        run(40);
        pair.edit(azl::RemoveModulator{modulator});
        run(5);
        CHECK(run(30) > 0);
        pair.edit(azl::SetEffectBypass{2, true});
        run(20);
        pair.edit(azl::SetEffectBypass{2, false});
        run(20);
        CHECK(reused > 0);
    }
}

TEST_CASE("a paused clip's static prefix is drawn once and read from the bake on every frame after")
{
    azl::Transport paused;
    paused.speed = 0.0;
    Pair pair(chainOn(edges()), paused);
    for (int i = 0; i < azl::StaticBake::kSettleFrames + 2; ++i)
        pair.step();
    for (int i = 0; i < 10; ++i)
        CHECK(pair.step() == 2);
    CHECK(pair.baked.stats().bakes == 1);

    // A parameter edit in the prefix draws it again until it settles; one after it does not touch the bake.
    pair.edit(azl::SetEffectParam{3, "hue", 0.6});
    CHECK(pair.step() == 2);
    pair.edit(azl::SetEffectParam{1, "multiplier", 0.9});
    for (int i = 1; i < azl::StaticBake::kSettleFrames; ++i)
        CHECK(pair.step() == 0);
    CHECK(pair.step() == 0);
    CHECK(pair.step() == 2);

    // Another picture bakes anew.
    pair.baked.engine().scrub(1, 0.5);
    pair.live.engine().scrub(1, 0.5);
    CHECK(pair.step() == 0);
    CHECK(pair.step() == 2);

    // Switched off, the bake is dropped and every pass is drawn.
    pair.baked.engine().setBakeStatic(false);
    CHECK(pair.step() == 0);
    CHECK(pair.baked.stats().bakes == 0);
}
