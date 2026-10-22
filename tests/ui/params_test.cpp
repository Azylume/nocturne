#include "document/modulation_edits.hpp"
#include "ui/app/params.hpp"

#include <doctest.h>

namespace {

using namespace azl;
using namespace azl::ui;

// Layer 1 with a Transform (effect 3) at scale 2; its clip in column 1 has a Colorize (effect 4).
Composition panelComposition()
{
    Composition composition = defaultComposition();
    composition.layers[0].opacity = 0.25;
    composition.layers[0].effects = {{3, "transform", {{"scale", 2.0}, {"mix", 1.0}}}};
    composition.layers[0].clips[1] = ClipRef{std::string(64, 'a'), "one", {}, 0, std::nullopt,
                                             {{4, "colorize", {{"hue", 0.0}, {"saturation", 1.0}, {"mix", 1.0}}}}};
    return composition;
}

} // namespace

TEST_CASE("parameters are found with their range and value in layer, clip and composition chains")
{
    const Composition composition = panelComposition();
    const auto opacity = paramInfo(composition, {1, 0, "opacity"});
    REQUIRE(opacity);
    CHECK(opacity->value == 0.25);
    const auto scale = paramInfo(composition, {0, 3, "scale"});
    REQUIRE(scale);
    CHECK(scale->max == 8.0);
    CHECK(scale->fraction(2.0) == doctest::Approx(0.25));
    CHECK(paramInfo(composition, {0, 3, "mix"})->label == "Dry/wet");
    CHECK_FALSE(paramInfo(composition, {0, 3, "wobble"}));
    CHECK_FALSE(paramInfo(composition, {9, 0, "opacity"}));

    CHECK(paramLabel(composition, {0, 3, "scale"}) == "Layer 1 / transform / scale");
    CHECK(paramLabel(composition, {0, 4, "hue"}) == "Layer 1 / one / colorize / hue");
    CHECK(ownerOf(composition, {0, 4, "hue"}) == ChainOwner{1, 1});
    CHECK(ownerOf(composition, {0, 3, "scale"}) == ChainOwner{1});
    CHECK(chainOf(composition, {1, 1})->at(0).type == "colorize");
    CHECK(chainOf(composition, {})->empty());
    CHECK(std::holds_alternative<SetLayerOpacity>(setParam({1, 0, "opacity"}, 0.5)));
    CHECK(std::get<SetEffectParam>(setParam({0, 3, "scale"}, 3.0)).value == 3.0);
}

TEST_CASE("an animation sweeps the parameter's range as offsets from its value")
{
    Composition composition = panelComposition();
    const ParamInfo scale = *paramInfo(composition, {0, 3, "scale"});
    const Modulator beat = newAnimation(scale, "beat");
    CHECK(beat.source == ModulatorSource::Lfo);
    CHECK(beat.beats == 4.0);
    CHECK(beat.min == doctest::Approx(-1.0));
    CHECK(beat.max == doctest::Approx(1.0));
    const Range range = rangeOf(scale, beat.min, beat.max);
    CHECK(range.from == doctest::Approx(0.125));
    CHECK(range.to == doctest::Approx(0.375));
    const auto [min, max] = offsetsOf(scale, {0.5, 0.75});
    CHECK(min == doctest::Approx(2.0));
    CHECK(max == doctest::Approx(4.0));

    const Modulator kick = newAnimation(scale, "kick");
    CHECK(kick.source == ModulatorSource::Audio);
    CHECK(kick.feature == "kick");
    CHECK(kick.envelope == azl::EnvelopePreset::Meter);
    CHECK(kick.release == 500.0);
    CHECK(Modulator{}.attack == 0.0);
    CHECK(Modulator{}.envelope == azl::EnvelopePreset::None);
    CHECK(kick.min == doctest::Approx(0.0));
    CHECK(kick.max == doctest::Approx(1.0));
    CHECK(animationSource(withSource(kick, scale, "beat")) == "beat");

    composition.modulators.push_back(kick);
    composition.modulators.back().id = 7;
    CHECK(animationOf(composition, scale.ref)->id == 7);
    CHECK_FALSE(animationOf(composition, {1, 0, "opacity"}));
}

TEST_CASE("parameters read in their units and animate over sensible ranges in them")
{
    Composition composition = panelComposition();
    composition.layers[0].effects.push_back({5, "stripper", {{"direction", -45.0}, {"speed", 2.3}}});
    const ParamInfo direction = *paramInfo(composition, {0, 5, "direction"});
    CHECK(direction.min == -180.0);
    CHECK(direction.max == 180.0);
    CHECK(direction.wraps);
    CHECK(direction.format(-45.0) == "-45\xc2\xb0");
    CHECK(direction.format(-225.8) == "134\xc2\xb0");
    CHECK(direction.format(-0.2) == "0\xc2\xb0");
    CHECK(paramInfo(composition, {0, 5, "speed"})->format(2.3) == "2.30 strips/s");
    CHECK(paramInfo(composition, {0, 5, "number"})->format(8.0) == "8.0 strips");
    CHECK(paramInfo(composition, {0, 5, "width"})->format(0.5) == "50%");
    CHECK(paramInfo(composition, {0, 4, "hue"})->format(0.25) == "90\xc2\xb0");
    CHECK(paramInfo(composition, {0, 3, "scale"})->format(1.0) == "1.00x");
    CHECK(paramInfo(composition, {1, 0, "opacity"})->format(0.25) == "25%");

    // Direction swings a quarter turn either way, a share sweeps its whole range, speed moves around its value.
    const Modulator swing = newAnimation(direction, "beat");
    CHECK(swing.min == doctest::Approx(-90.0));
    CHECK(swing.max == doctest::Approx(90.0));
    const Ends ends = endsOf(direction, swing.min, swing.max);
    CHECK(ends.from == doctest::Approx(-135.0));
    CHECK(ends.to == doctest::Approx(45.0));
    const Modulator width = newAnimation(*paramInfo(composition, {0, 5, "width"}), "beat");
    CHECK(width.min == doctest::Approx(-0.5));
    CHECK(width.max == doctest::Approx(0.5));
    const Modulator speed = newAnimation(*paramInfo(composition, {0, 5, "speed"}), "beat");
    CHECK(speed.min == doctest::Approx(-1.0));
    CHECK(speed.max == doctest::Approx(1.0));
    const Modulator kick = newAnimation(direction, "kick");
    CHECK(kick.min == 0.0);
    CHECK(kick.max == doctest::Approx(90.0));

    // An end set past the range's end goes on round for a wrapping parameter and stops for another.
    CHECK(offsetTo(direction, 170.0, 90.0) == doctest::Approx(215.0));
    CHECK(offsetTo(direction, -170.0, 215.0) == doctest::Approx(235.0));
    CHECK(offsetTo(direction, -170.0, 0.0) == doctest::Approx(-125.0));
    const ParamInfo scale = *paramInfo(composition, {0, 3, "scale"});
    CHECK(offsetTo(scale, 9.0, 0.0) == doctest::Approx(6.0));
    CHECK(offsetTo(scale, 0.5, 0.0) == doctest::Approx(-1.5));
}

TEST_CASE("macros fill eight slots and a missing one is created with those before it")
{
    Composition composition = panelComposition();
    CHECK(macroSlots(composition) == std::vector<const Macro*>(8, nullptr));
    auto [edits, id] = ensureMacro(composition, 2);
    REQUIRE(edits.size() == 3);
    CHECK(id == 3);
    for (Edit& edit : edits)
        applyEdit(composition, edit);
    CHECK(macroSlots(composition)[2]->name == "Macro 3");
    CHECK(ensureMacro(composition, 1).first.empty());
    CHECK(ensureMacro(composition, 1).second == 2);

    const MacroTarget up = newTarget(*paramInfo(composition, {1, 0, "opacity"}));
    CHECK(up.min == 0.0);
    CHECK(up.max == doctest::Approx(0.75));
    composition.layers[0].opacity = 0.9;
    const MacroTarget down = newTarget(*paramInfo(composition, {1, 0, "opacity"}));
    CHECK(down.max == doctest::Approx(-0.9));
    CHECK(curveName(Curve{}) == "Linear");
    CHECK(curveName(Curve{{0.1, 0.2, 0.3, 0.4}}) == "Custom");
}

TEST_CASE("a modulator places its target within its from..to range through its curve")
{
    const Composition composition = panelComposition();
    Modulator m;
    m.target.param = {1, 0, "opacity"};
    m.min = -0.25;
    m.max = 0.5;
    CHECK(modulatedAt(composition, m, 0.0) == doctest::Approx(0.0));
    CHECK(modulatedAt(composition, m, 0.5) == doctest::Approx(0.375));
    CHECK(modulatedAt(composition, m, 1.0) == doctest::Approx(0.75));
    m.curve = Curve{{0.0, 0.5, 0.5, 1.0}};
    CHECK(modulatedAt(composition, m, 0.5) == doctest::Approx(0.75 * curveAt(m.curve, 0.5)));
    m.max = 2.0;
    CHECK(modulatedAt(composition, m, 1.0) == 1.0);
}
