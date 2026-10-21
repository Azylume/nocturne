#include "document/cue_edits.hpp"
#include "document/palette.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/effect_stepper.hpp"
#include "engine/modulation_plan.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::Composition;
using azl::Effect;
using azl::ParamRef;
using azl::Rgba;

constexpr uint64_t kStripper = 7;
// classicStripper's colour in shader order.
constexpr size_t kRed = 11, kGreen = 12, kBlue = 13;
const Rgba kRedColour{1, 0, 0, 1}, kGreenColour{0, 1, 0, 1}, kBlueColour{0, 0, 1, 1};

azl::ColourPalette paletteOf(uint64_t id, const std::string& name, Rgba colour)
{
    return {id, name, {{"Main", {colour.r, colour.g, colour.b, colour.a}, id * 10}}};
}

// Three one-colour palettes (red, green, blue) and a Stripper on layer 1 whose colour follows the first slot.
Composition paletteComposition(AutopilotMode mode = AutopilotMode::Next, double transition = 4.0)
{
    Composition composition;
    composition.palettes = {paletteOf(1, "Red", kRedColour), paletteOf(2, "Green", kGreenColour),
                            paletteOf(3, "Blue", kBlueColour)};
    composition.palettePilot = {mode, 1, 2};
    composition.paletteTransition = transition;
    Effect stripper{kStripper, "classicStripper", {{"red", 1.0}, {"green", 0.0}, {"blue", 0.0}}};
    stripper.links = {{"red", 10}};
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {stripper}});
    return composition;
}

Effect& stripperOf(Composition& composition)
{
    return composition.layers[0].effects[0];
}

double distance(const Rgba& a, const Rgba& b)
{
    const auto x = azl::toOklab(a), y = azl::toOklab(b);
    return std::hypot(x[0] - y[0], x[1] - y[1], x[2] - y[2]);
}

// The Stripper's colour as the plan draws it at `beat`, after the stepper moved the palettes.
struct Drawn {
    Composition composition;
    azl::BeatClock clock;
    azl::CueState cues;
    std::atomic<uint32_t> seed{7};
    azl::EffectStepper stepper{clock, cues, seed};
    std::unique_ptr<azl::ModulationPlan> plan = std::make_unique<azl::ModulationPlan>(composition);
    azl::PlannedPass pass = azl::planChain(azl::SourceKind::Yuv, {azl::findEffect("classicStripper")})[0];
    std::vector<float> values;

    explicit Drawn(Composition c) : composition(std::move(c)) {}

    void rebuild()
    {
        auto next = std::make_unique<azl::ModulationPlan>(composition);
        next->carry(*plan);
        plan = std::move(next);
    }

    std::optional<size_t> paletteAt(double beat)
    {
        azl::cueStateAt(composition, beat, cues);
        stepper.step(composition, beat);
        const auto shown = stepper.shownPalette(beat);
        return shown ? shown->step : std::nullopt;
    }

    Rgba at(double beat)
    {
        azl::cueStateAt(composition, beat, cues);
        plan->cue(cues.macros);
        stepper.step(composition, beat);
        plan->step(stepper.positions());
        plan->evaluate(beat / 2.0, beat, {});
        const azl::EffectList effects = azl::effectList(composition.layers[0].effects);
        azl::paramsOf(pass, effects, values);
        plan->apply(pass, effects, values);
        return {values[kRed], values[kGreen], values[kBlue], 1.0};
    }
};

void apply(Composition& composition, azl::Edit edit)
{
    azl::applyEdit(composition, edit);
}

Message send(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

std::string ctl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    CHECK(pclose(pipe) == 0);
    return out;
}

} // namespace

TEST_CASE("palette stepping: selecting a palette recolours linked colours by slot, live; the pilot and deck palettes "
          "are undoable edits")
{
    Composition composition = paletteComposition(AutopilotMode::Off);
    composition.palettes[1].colours.push_back({"Extra", {1, 1, 1, 1}, 21});
    CHECK(azl::activePalette(composition)->id == 1);

    azl::Edit select = azl::SelectPalette{2, 5.0};
    CHECK(azl::isLive(select));
    azl::applyEdit(composition, select);
    CHECK(composition.palette == 2);
    CHECK(composition.palettePlaying == azl::StepPlay{1, 8.0});
    CHECK(azl::activePalette(composition)->id == 2);
    CHECK(stripperOf(composition).params.at("green") == 1.0);
    CHECK(stripperOf(composition).params.at("red") == 0.0);
    // The link still names the red palette's colour; it follows the slot.
    CHECK(stripperOf(composition).links.at("red") == 10);
    CHECK(azl::paletteColour(composition, 10)->name == "Main");
    azl::Edit missing = azl::SelectPalette{9, 0.0};
    CHECK_THROWS_AS(azl::applyEdit(composition, missing), azl::ProtocolError);

    // A palette edit of the active palette recolours by slot; one of another palette does not.
    azl::ColourPalette blue = composition.palettes[2];
    blue.colours[0].colour = {0.5, 0.5, 0.5, 1.0};
    apply(composition, azl::SetPalette{blue});
    CHECK(stripperOf(composition).params.at("green") == 1.0);

    // Removing the active palette moves the links to the same slot of the palette active after it.
    const Composition before = composition;
    azl::Edit remove = azl::RemovePalette{2};
    azl::Edit undoRemove = azl::applyEdit(composition, remove);
    CHECK(stripperOf(composition).params.at("red") == 1.0);
    CHECK(stripperOf(composition).links.at("red") == 10);
    azl::applyEdit(composition, undoRemove);
    CHECK(composition.palettes == before.palettes);
    CHECK(stripperOf(composition) == before.layers[0].effects[0]);

    azl::Edit pilot = azl::SetPaletteAutopilot{AutopilotMode::Random, 8, 2, 3.0};
    azl::Edit undoPilot = azl::applyEdit(composition, pilot);
    CHECK(composition.palettePilot == azl::Autopilot{AutopilotMode::Random, 8, 2});
    CHECK(composition.paletteTransition == 3.0);
    azl::applyEdit(composition, undoPilot);
    CHECK(composition.palettePilot == before.palettePilot);
    CHECK(composition.paletteTransition == before.paletteTransition);
    azl::Edit badPilot = azl::SetPaletteAutopilot{std::nullopt, 0};
    CHECK_THROWS_AS(azl::applyEdit(composition, badPilot), azl::ProtocolError);

    azl::Edit deck = azl::SetDeckPalette{1, 3};
    azl::Edit undoDeck = azl::applyEdit(composition, deck);
    CHECK(composition.decks[0].palette == 3);
    azl::applyEdit(composition, undoDeck);
    CHECK(composition.decks[0].palette == 0);
    azl::Edit noPalette = azl::SetDeckPalette{1, 9};
    CHECK_THROWS_AS(azl::applyEdit(composition, noPalette), azl::ProtocolError);

    for (const azl::Edit& e : {select, pilot, deck, azl::Edit(azl::SelectDeck{1, 4.0})})
        CHECK(azl::editToJson(azl::editFromJson(azl::editToJson(e))) == azl::editToJson(e));
}

TEST_CASE("palette stepping: the palettes, the selection, the pilot and deck palettes save and load; old files load "
          "and save unchanged")
{
    Composition composition = paletteComposition(AutopilotMode::Random, 2.5);
    composition.palette = 2;
    composition.palettePlaying = azl::StepPlay{1, 12.0};
    composition.decks[0].palette = 3;
    const nlohmann::json saved = composition;
    const Composition back = saved.get<Composition>();
    CHECK(back.palette == 2);
    CHECK(back.palettePilot == composition.palettePilot);
    CHECK(back.paletteTransition == 2.5);
    CHECK(back.palettePlaying == composition.palettePlaying);
    CHECK(back.decks == composition.decks);

    const nlohmann::json plain = Composition{};
    for (const char* key : {"palette", "palettePilot", "paletteTransition", "palettePlaying"})
        CHECK_FALSE(plain.contains(key));
    CHECK_FALSE(plain["decks"][0].contains("palette"));
    CHECK(nlohmann::json(plain.get<Composition>()) == plain);
}

TEST_CASE("palette stepping: the palette Autopilot steps on the bar, a selection holds it, and Cues switch the palette")
{
    Drawn drawn(paletteComposition());
    CHECK(drawn.paletteAt(0.5) == 0);
    CHECK(drawn.paletteAt(3.9) == 0);
    CHECK(drawn.paletteAt(4.0) == 1);
    CHECK(drawn.paletteAt(7.9) == 1);
    CHECK(drawn.paletteAt(8.0) == 2);
    CHECK(drawn.paletteAt(12.0) == 0);
    for (const azl::StepPosition& p : drawn.stepper.positions())
        if (p.effect == azl::kPaletteEffect)
            CHECK(p.beat == 12.0);

    // A selection lands on the quantised beat and holds for two bars before the pilot moves on.
    apply(drawn.composition, azl::SelectPalette{3, 13.0});
    CHECK(drawn.paletteAt(13.0) == 2);
    CHECK(drawn.paletteAt(23.9) == 2);
    CHECK(drawn.paletteAt(24.0) == 0);

    Drawn cued(paletteComposition(AutopilotMode::Off));
    cued.composition.cues.push_back({.id = 1, .bar = 3, .effects = {{azl::kPaletteEffect, std::nullopt, std::nullopt, 2}}});
    CHECK(cued.paletteAt(4.0) == 0);
    CHECK(cued.paletteAt(8.0) == 2);
    Composition none;
    azl::Edit noPalette = azl::AddCue{std::nullopt, {.bar = 1, .effects = {{.effect = azl::kPaletteEffect, .mode = AutopilotMode::Next}}}};
    CHECK_THROWS_AS(azl::applyEdit(none, noPalette), azl::ProtocolError);
}

TEST_CASE("palette stepping: every linked colour crossfades in OKLab over the transition, nothing jumps, and a live "
          "edit takes over without a jump")
{
    Drawn drawn(paletteComposition(AutopilotMode::Next, 4.0));
    Rgba previous = drawn.at(0.0);
    CHECK(previous == kRedColour);
    double largest = 0.0;
    for (int frame = 1; frame <= 10 * 60; ++frame) {
        const Rgba now = drawn.at(frame / 60.0);
        largest = std::max(largest, distance(previous, now));
        previous = now;
        if (frame == 4 * 60 - 1)
            CHECK(distance(now, kRedColour) < 1e-6);
    }
    // Red to green over four beats, then green toward blue: never more than the transition's share in a frame.
    const double span = std::max(distance(kRedColour, kGreenColour), distance(kGreenColour, kBlueColour));
    CHECK(largest <= span / (4.0 * 60.0) * 1.05);
    // Halfway in OKLab, not the dark RGB midpoint.
    const Rgba mid = previous;
    CHECK(distance(mid, azl::mixOklab(kGreenColour, kBlueColour, 0.5)) < 0.005);
    CHECK(mid.b > 0.5);
    CHECK(mid.g > 0.5);

    // The live colours ride along for the UI and the text colours.
    CHECK(distance(drawn.plan->paletteColours().at(0), mid) < 1e-4);
    REQUIRE(drawn.plan->linkedColour(10));
    CHECK(distance(*drawn.plan->linkedColour(10), mid) < 1e-4);
    CHECK_FALSE(drawn.plan->linkedColour(99));
}

TEST_CASE("palette stepping: a live palette edit retargets the crossfade without a jump; a live edit of the colour "
          "itself takes it over")
{
    Drawn drawn(paletteComposition(AutopilotMode::Off, 4.0));
    for (int frame = 0; frame <= 60; ++frame)
        drawn.at(frame / 60.0);
    apply(drawn.composition, azl::SelectPalette{2, 1.0});
    drawn.rebuild();
    Rgba previous;
    for (int frame = 61; frame <= 6 * 60; ++frame)
        previous = drawn.at(frame / 60.0);
    CHECK(distance(previous, azl::mixOklab(kRedColour, kGreenColour, 0.5)) < 1e-4);

    // A drag of the green palette colour: the crossfade heads for each new colour from where it is.
    const Rgba edited{0.2, 0.9, 0.3, 1.0};
    for (int frame = 1; frame <= 30; ++frame) {
        azl::ColourPalette green = drawn.composition.palettes[1];
        const double t = frame / 30.0;
        green.colours[0].colour = {0.2 * t, 1.0 - 0.1 * t, 0.3 * t, 1.0};
        apply(drawn.composition, azl::SetPalette{green});
        drawn.rebuild();
        const Rgba now = drawn.at(6.0 + frame / 60.0);
        CHECK(distance(previous, now) < 0.01);
        previous = now;
    }
    CHECK(distance(drawn.at(8.0), edited) < 1e-6);

    // An edit of the linked colour's own parameters takes it over.
    stripperOf(drawn.composition).params["blue"] = 1.0;
    drawn.rebuild();
    drawn.at(8.5);
    CHECK(drawn.at(9.0).b == doctest::Approx(1.0));
    // The next palette moves on from there.
    apply(drawn.composition, azl::SelectPalette{3, 9.0});
    drawn.rebuild();
    CHECK(drawn.at(12.0).b == doctest::Approx(1.0));
    CHECK(distance(drawn.at(14.0), azl::mixOklab({0.2, 0.9, 1.0, 1.0}, kBlueColour, 0.5)) < 1e-4);
}

TEST_CASE("palette stepping: a selection and a deck switch crossfade from the bar; a deck brings its palette")
{
    Drawn drawn(paletteComposition(AutopilotMode::Off, 2.0));
    drawn.composition.decks.push_back({2, "Deck 2", {{5, "Column 5"}}, 3});
    for (int frame = 0; frame <= 60; ++frame)
        CHECK(distance(drawn.at(frame / 60.0), kRedColour) < 1e-6);

    apply(drawn.composition, azl::SelectDeck{2, 1.0});
    CHECK(drawn.composition.palette == 3);
    CHECK(stripperOf(drawn.composition).params.at("blue") == 1.0);
    drawn.rebuild();
    // The document holds blue at once; the picture stays red until the bar, then crossfades over two beats.
    CHECK(distance(drawn.at(2.0), kRedColour) < 1e-6);
    CHECK(distance(drawn.at(3.9), kRedColour) < 1e-6);
    CHECK(distance(drawn.at(5.0), azl::mixOklab(kRedColour, kBlueColour, 0.5)) < 1e-4);
    CHECK(distance(drawn.at(6.0), kBlueColour) < 1e-6);

    // A deck without a palette leaves it.
    apply(drawn.composition, azl::SelectDeck{1, 8.0});
    CHECK(drawn.composition.palette == 3);
}

TEST_CASE("palette stepping: a macro drives the palette position, blending neighbours, and a slot's hue")
{
    Composition composition = paletteComposition(AutopilotMode::Off, 0.0);
    composition.macros = {{.id = 1, .value = 0.5, .targets = {{.param = {0, 0, std::string(azl::kPalettePosition)}}}},
                          {.id = 2, .value = 0.0, .targets = {{.param = {0, 0, azl::paletteHueParam(0)}}}}};
    azl::Edit check = azl::AddMacro{std::nullopt, {.id = 3, .targets = {{.param = {0, 0, azl::paletteHueParam(5)}}}}};
    Composition invalid = composition;
    CHECK_THROWS_AS(azl::applyEdit(invalid, check), azl::ProtocolError);
    Composition valid = paletteComposition();
    azl::Edit add = azl::AddMacro{std::nullopt, composition.macros[0]};
    CHECK_NOTHROW(azl::applyEdit(valid, add));
    azl::Edit target = azl::AddMacroTarget{1, std::nullopt, {.param = {0, 0, azl::paletteHueParam(0)}}};
    CHECK_NOTHROW(azl::applyEdit(valid, target));

    Drawn drawn(composition);
    CHECK(distance(drawn.at(0.0), kGreenColour) < 1e-6);
    drawn.composition.macros[0].value = 0.25;
    drawn.rebuild();
    CHECK(distance(drawn.at(1.0), azl::mixOklab(kRedColour, kGreenColour, 0.5)) < 1e-4);
    drawn.composition.macros[0].value = 1.0;
    drawn.rebuild();
    CHECK(distance(drawn.at(2.0), kBlueColour) < 1e-6);

    drawn.composition.macros[0].value = 0.0;
    drawn.composition.macros[1].value = 0.5;
    drawn.rebuild();
    const Rgba turned = drawn.at(3.0);
    CHECK(distance(turned, azl::rotateHue(kRedColour, 0.5)) < 1e-4);
    CHECK(distance(turned, kRedColour) > 0.1);
}

TEST_CASE("palette stepping over the protocol and nocturne-ctl: select, next, pilot, deck palettes, Cues; the clock "
          "shows the palette and its live colours")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0);
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    ctl(port, "palette add main '#ff0000'");
    ctl(port, "palette new Green");
    ctl(port, "palette new Blue");
    auto document = [&] { return harness.document().document(); };
    REQUIRE(document().palettes.size() == 3);
    CHECK(document().palettes[1].colours[0].id != document().palettes[0].colours[0].id);
    ctl(port, "palette select Green");
    CHECK(document().palette == document().palettes[1].id);
    ctl(port, "palette set 1 '#00ff00'");
    CHECK(document().palettes[1].colours[0].colour == std::array<double, 4>{0, 1, 0, 1});
    ctl(port, "palette select 3");
    ctl(port, "palette set main '#0000ff'");
    ctl(port, "palette select 1");
    CHECK(document().palette == document().palettes[0].id);

    Client client(harness.document());
    const Message added = send(client, azl::AddEffect{0, std::nullopt, Effect{0, "classicStripper", {}}});
    const std::string strobe = std::to_string(added["effect"]["id"].get<uint64_t>());
    ctl(port, "palette link 1 " + strobe + " red");
    ctl(port, "palette transition 2");
    ctl(port, "palette pilot next 1");
    ctl(port, "palette hold 0");
    CHECK(document().palettePilot == azl::Autopilot{AutopilotMode::Next, 1, 0});
    CHECK(document().paletteTransition == 2.0);
    CHECK(ctl(port, "palette list").find("*1 Palette 1") != std::string::npos);

    // The state reply, past the changes the ctl calls broadcast.
    auto stateOf = [&](const char* type) {
        client.send({{"type", type}, {"op", "state"}});
        for (;;) {
            const Message m = client.next();
            if (m.value("type", "") == type && m.value("op", "") == "state")
                return m;
        }
    };
    auto clock = [&] {
        const Message state = stateOf("clock");
        REQUIRE(state.contains("palette"));
        return state["palette"];
    };
    auto colour = [&] {
        const Message state = stateOf("modulation");
        std::map<std::string, double> channels;
        for (const Message& p : state["params"])
            if (std::to_string(p["effect"].get<uint64_t>()) == strobe)
                channels[p["param"].get<std::string>()] = p["value"].get<double>();
        REQUIRE_MESSAGE(channels.contains("blue"), state.dump());
        return Rgba{channels["red"], channels["green"], channels["blue"], 1.0};
    };
    harness.step(30);
    CHECK(clock()["next"] == 1);
    // Two beats of crossfade at 120 bpm and 60 frames a second: 60 frames for the widest move.
    const double span = std::max({distance(kRedColour, kGreenColour), distance(kGreenColour, kBlueColour),
                                  distance(kBlueColour, kRedColour)});
    Rgba previous = colour();
    for (int frame = 0; frame < 300; ++frame) {
        harness.step();
        const Rgba now = colour();
        REQUIRE(distance(previous, now) < span / 60.0 * 1.5);
        previous = now;
    }
    const Message shown = clock();
    CHECK(shown["palette"].is_number());
    REQUIRE(shown["colours"].size() == 1);
    CHECK(shown["colours"][0].size() == 4);

    ctl(port, "palette pilot off");
    ctl(port, "palette next");
    REQUIRE(document().palettePlaying);
    ctl(port, "palette deck 1 Blue");
    CHECK(document().decks[0].palette == document().palettes[2].id);
    ctl(port, "deck 1");
    CHECK(document().palette == document().palettes[2].id);
    ctl(port, "palette deck 1 none");
    CHECK(document().decks[0].palette == 0);

    ctl(port, "cue add drop drop palette:2 palettepilot:random/4");
    REQUIRE(document().cues.size() == 1);
    CHECK(document().cues[0].effects ==
          std::vector<azl::CueEffect>{{azl::kPaletteEffect, AutopilotMode::Random, 4, size_t(1)}});

    ctl(port, "palette delete Green");
    CHECK(document().palettes.size() == 2);
}
