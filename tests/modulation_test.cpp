#include "audio/audio_analysis.hpp"
#include "document/composition_file.hpp"
#include "document/modulation_edits.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/modulation_plan.hpp"
#include "net/websocket.hpp"
#include "protocol/document_mirror.hpp"
#include "techno_loop.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <vector>

namespace {

using azl::Curve;
using azl::LfoShape;
using azl::Macro;
using azl::MacroTarget;
using azl::Modulator;
using azl::ParamRef;

constexpr double kFps = 60.0;

// One layer showing the test card through a Reveal (effect 1) and a Colorize (effect 2).
azl::Composition revealComposition()
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1,
                                  .name = "Layer 1",
                                  .effects = {{1, "reveal", {{"progress", 1.0}, {"evolve", 0.0}}},
                                              {2, "colorize", {{"mix", 0.0}}}}});
    return composition;
}

ParamRef revealEvolve()
{
    return {0, 1, "evolve"};
}

ParamRef opacity(uint64_t layer = 1)
{
    return {layer, 0, "opacity"};
}

Modulator lfo(LfoShape shape, double beats, azl::ModTarget target, double min = 0.0, double max = 1.0)
{
    Modulator m;
    m.shape = shape;
    m.beats = beats;
    m.target = std::move(target);
    m.min = min;
    m.max = max;
    return m;
}

Modulator audio(const std::string& feature, azl::ModTarget target, double min = 0.0, double max = 1.0)
{
    Modulator m;
    m.source = azl::ModulatorSource::Audio;
    m.feature = feature;
    m.target = std::move(target);
    m.min = min;
    m.max = max;
    return m;
}

Message state(Client& client)
{
    client.send({{"type", "modulation"}, {"op", "state"}});
    const Message reply = client.next();
    REQUIRE_MESSAGE(reply["op"] == "state", reply.dump());
    return reply;
}

double valueOf(const Message& list, const char* key, uint64_t id)
{
    for (const Message& entry : list)
        if (entry[key] == id)
            return entry["value"].get<double>();
    FAIL("no entry " << id << " in " << list.dump());
    return 0.0;
}

double paramValue(const Message& state, const ParamRef& param)
{
    for (const Message& p : state["params"])
        if (p.get<ParamRef>() == param)
            return p["value"].get<double>();
    FAIL("no param " << param.param << " in " << state.dump());
    return 0.0;
}

double luminance(const azl::Image& image)
{
    double sum = 0.0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        sum += 0.2126 * image.rgba[i] + 0.7152 * image.rgba[i + 1] + 0.0722 * image.rgba[i + 2];
    return sum / double(image.rgba.size() / 4);
}

uint64_t added(const Message& change, const char* key)
{
    return change[key]["id"].get<uint64_t>();
}

} // namespace

TEST_CASE("beat LFOs peak on the beat and curves bend values")
{
    CHECK(azl::lfoAt(LfoShape::Sine, 0.0) == doctest::Approx(1.0));
    CHECK(azl::lfoAt(LfoShape::Sine, 0.5) == doctest::Approx(0.0));
    CHECK(azl::lfoAt(LfoShape::Sine, 0.25) == doctest::Approx(0.5));
    CHECK(azl::lfoAt(LfoShape::Saw, 0.0) == doctest::Approx(1.0));
    CHECK(azl::lfoAt(LfoShape::Saw, 0.75) == doctest::Approx(0.25));
    CHECK(azl::lfoAt(LfoShape::Ramp, 0.75) == doctest::Approx(0.75));
    CHECK(azl::lfoAt(LfoShape::Ramp, 3.25) == doctest::Approx(0.25));
    CHECK(azl::lfoAt(LfoShape::Square, 0.49) == 1.0);
    CHECK(azl::lfoAt(LfoShape::Square, 0.51) == 0.0);

    const Curve linear;
    const Curve easeIn{{0.42, 0.0, 1.0, 1.0}};
    const Curve easeOut{{0.0, 0.0, 0.58, 1.0}};
    for (double x : {0.1, 0.3, 0.5, 0.7, 0.9}) {
        CHECK(azl::curveAt(linear, x) == doctest::Approx(x).epsilon(1e-5));
        CHECK(azl::curveAt(easeIn, x) < x);
        CHECK(azl::curveAt(easeOut, x) > x);
    }
    CHECK(azl::curveAt(easeIn, 0.0) == doctest::Approx(0.0));
    CHECK(azl::curveAt(easeIn, 1.0) == doctest::Approx(1.0));
    CHECK(azl::curveAt(easeIn, 0.5) == doctest::Approx(0.3153).epsilon(0.01));
}

TEST_CASE("macros and modulators are undoable edits that never touch the base value")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());

    const uint64_t macro = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Energy", .value = 0.5}}), "macro");
    CHECK(macro == 1);
    apply(client, azl::AddMacroTarget{macro, std::nullopt, {revealEvolve(), {}, 0.0, 2.0}});
    const Message change = apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Square, 1.0, {macro, {}})});
    CHECK(change["modulator"]["name"] == "Modulator 1");
    CHECK(change["modulator"]["target"]["macro"] == macro);

    harness.step(5);
    const Message s = state(client);
    CHECK(valueOf(s["macros"], "id", macro) == doctest::Approx(1.0));
    CHECK(paramValue(s, revealEvolve()) == doctest::Approx(2.0));
    const azl::Composition document = harness.document().document();
    CHECK(document.layers[0].effects[0].params.at("evolve") == 0.0);
    CHECK(document.macros[0].value == 0.5);

    client.send(msg::undo());
    CHECK(client.next()["op"] == "removeModulator");
    harness.step();
    CHECK(paramValue(state(client), revealEvolve()) == doctest::Approx(1.0));
    client.send(msg::undo());
    client.next();
    client.send(msg::undo());
    CHECK(client.next()["op"] == "removeMacro");
    CHECK(harness.document().document().macros.empty());
    client.send(msg::redo());
    CHECK(client.next()["macro"]["name"] == "Energy");
}

TEST_CASE("modulation edits are validated")
{
    azl::Composition composition = revealComposition();
    composition.layers.push_back({.id = 5, .name = "Group", .group = true, .layers = {{.id = 6, .name = "Inner"}}});
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, composition);
    Client client(harness.document());
    auto error = [&](const azl::Edit& edit) { return command(client, edit)["type"] == "error"; };

    CHECK(error(azl::AddMacro{std::nullopt, {.value = 1.5}}));
    CHECK(error(azl::AddMacro{std::nullopt, {.group = 1}}));
    CHECK(error(azl::AddMacro{std::nullopt, {.targets = {{{0, 1, "wobble"}, {}, 0.0, 1.0}}}}));
    CHECK(error(azl::AddMacro{std::nullopt, {.targets = {{revealEvolve(), {}, 0.0, 9.0}}}}));
    CHECK(error(azl::AddMacro{std::nullopt, {.targets = {{revealEvolve(), {{1.5, 0.0, 1.0, 1.0}}, 0.0, 1.0}}}}));
    CHECK(error(azl::AddMacro{std::nullopt, {.group = 5, .targets = {{opacity(1), {}, 0.0, 1.0}}}}));
    apply(client, azl::AddMacro{std::nullopt, {.group = 5, .targets = {{opacity(6), {}, -1.0, 0.0}}}});
    for (int i = 0; i < 8; ++i)
        apply(client, azl::AddMacro{std::nullopt, {}});
    CHECK(error(azl::AddMacro{std::nullopt, {}}));

    CHECK(error(azl::AddModulator{std::nullopt, lfo(LfoShape::Sine, 0.01, {1, {}})}));
    CHECK(error(azl::AddModulator{std::nullopt, lfo(LfoShape::Sine, 1.0, {1, {}}, 0.0, 2.0)}));
    CHECK(error(azl::AddModulator{std::nullopt, audio("wobble", {0, revealEvolve()})}));
    CHECK(error(azl::AddModulator{std::nullopt, lfo(LfoShape::Sine, 1.0, {0, {1, 0, "scale"}})}));
    CHECK(error(azl::SetModulator{lfo(LfoShape::Sine, 1.0, {1, {}})}));
    CHECK(command(client, azl::AddModulator{std::nullopt, audio("kick", {0, revealEvolve()}, 0.0, 4.0)})["type"] ==
          "change");
    Modulator slow = audio("kick", {0, revealEvolve()});
    slow.release = 20000.0;
    CHECK(error(azl::AddModulator{std::nullopt, slow}));
}

TEST_CASE("an LFO follows the beat clock: phase, tempo and resync")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    apply(client, azl::SetTempo{120.0});
    const uint64_t ramp = added(apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Ramp, 4.0, {0, revealEvolve()})}), "modulator");
    Modulator square = lfo(LfoShape::Square, 1.0, {0, opacity()}, 0.0, -1.0);
    square.phase = 0.5;
    const uint64_t gate = added(apply(client, azl::AddModulator{std::nullopt, square}), "modulator");

    // 120 BPM: a beat every 30 frames. The ramp climbs over 4 beats; the square, half a period late, is 0 in the
    // first half of each beat (opacity 1) and 1 in the second (opacity 0).
    for (int frame = 0; frame < 150; ++frame) {
        harness.step();
        const double beat = double(frame) / kFps * 2.0;
        const Message s = state(client);
        CAPTURE(frame);
        CHECK(valueOf(s["modulators"], "id", ramp) == doctest::Approx(std::fmod(beat / 4.0, 1.0)).epsilon(1e-4));
        if (std::fmod(beat, 1.0) > 0.05 && std::fmod(beat, 1.0) < 0.45)
            CHECK(paramValue(s, opacity()) == 1.0);
        if (std::fmod(beat, 1.0) > 0.55 && std::fmod(beat, 1.0) < 0.95) {
            CHECK(paramValue(s, opacity()) == 0.0);
            CHECK(luminance(harness.readScreen()) < 1.0);
        }
    }

    apply(client, azl::SetTempo{60.0});
    harness.step();
    const double before = valueOf(state(client)["modulators"], "id", ramp);
    harness.step(30);
    CHECK(valueOf(state(client)["modulators"], "id", ramp) - before == doctest::Approx(0.5 / 4.0).epsilon(0.01));

    client.send({{"type", "clock"}, {"op", "resync"}});
    client.next();
    harness.step();
    CHECK(valueOf(state(client)["modulators"], "id", ramp) == doctest::Approx(0.0).epsilon(0.01));
    CHECK(valueOf(state(client)["modulators"], "id", gate) == 0.0);
}

TEST_CASE("macros map through per-target curves and modulators combine on one target")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const Curve easeIn{{0.42, 0.0, 1.0, 1.0}};
    const uint64_t macro = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Energy", .targets = {
        {revealEvolve(), easeIn, 0.0, 4.0},
        {{0, 2, "mix"}, {}, 0.0, 1.0},
        {opacity(), {}, 0.0, -0.5}}}}), "macro");
    harness.step();
    const uint64_t pipelines = harness.compileStats().pipelines;

    for (double v : {0.0, 0.25, 0.5, 0.8, 1.0}) {
        apply(client, azl::SetMacroValue{macro, v});
        harness.step();
        const Message s = state(client);
        CAPTURE(v);
        CHECK(paramValue(s, revealEvolve()) == doctest::Approx(4.0 * azl::curveAt(easeIn, v)).epsilon(0.002));
        CHECK(paramValue(s, {0, 2, "mix"}) == doctest::Approx(v).epsilon(0.002));
        CHECK(paramValue(s, opacity()) == doctest::Approx(1.0 - 0.5 * v).epsilon(0.002));
    }

    // Energy at 0.2, plus a ramp over 4 beats scaled to 0..0.5, times a square gate that is 1 then 0.5.
    apply(client, azl::SetTempo{120.0});
    apply(client, azl::SetMacroValue{macro, 0.2});
    const uint64_t ramp = added(apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Ramp, 4.0, {macro, {}}, 0.0, 0.5)}), "modulator");
    Modulator gate = lfo(LfoShape::Square, 2.0, {macro, {}}, 0.5, 1.0);
    gate.combine = azl::Combine::Multiply;
    const uint64_t square = added(apply(client, azl::AddModulator{std::nullopt, gate}), "modulator");
    const uint64_t direct = added(apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Saw, 1.0, {0, revealEvolve()}, 0.0, 1.0)}), "modulator");
    for (int i = 0; i < 120; ++i) {
        harness.step();
        const Message s = state(client);
        const double r = valueOf(s["modulators"], "id", ramp);
        const double g = valueOf(s["modulators"], "id", square);
        const double saw = valueOf(s["modulators"], "id", direct);
        const double energy = std::clamp((0.2 + 0.5 * r) * (0.5 + 0.5 * g), 0.0, 1.0);
        CAPTURE(i);
        CHECK(valueOf(s["macros"], "id", macro) == doctest::Approx(energy).epsilon(1e-4));
        CHECK(paramValue(s, revealEvolve()) ==
              doctest::Approx(std::min(4.0, 4.0 * azl::curveAt(easeIn, energy) + saw)).epsilon(0.003));
    }
    CHECK(harness.compileStats().pipelines == pipelines);
}

TEST_CASE("a kick-driven macro pulses a brightness on the beat")
{
    azl::Composition composition;
    composition.layers.push_back(
        {.id = 1, .name = "Layer 1", .effects = {{1, "classicBrightnessContrast", {{"brightness", 0.0}}}}});
    const ParamRef brightness{0, 1, "brightness"};
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, composition);
    Client client(harness.document());
    azl::AudioAnalysis analysis([&] { return harness.time(); }, {.thread = false});
    harness.setAudio(&analysis);
    apply(client, azl::SetTempo{128.0});
    const uint64_t pump = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Pump", .targets = {
        {brightness, {}, 0.0, 1.0}}}}), "macro");
    apply(client, azl::AddModulator{std::nullopt, audio("kick", {pump, {}})});

    const TechnoLoop loop{.bpm = 128.0, .bars = 2, .start = 0.25};
    const std::vector<float> samples = loop.render();
    size_t pushed = 0;
    std::vector<double> amount;
    std::vector<double> lit;
    const int frames = int((loop.kick(loop.beats() - 1) + 0.4) * kFps);
    for (int f = 0; f < frames; ++f) {
        const size_t until = std::min(samples.size(), size_t(harness.time() * TechnoLoop::kRate));
        for (; pushed < until; pushed += std::min<size_t>(800, until - pushed))
            analysis.push(samples.data() + pushed, std::min<size_t>(800, until - pushed),
                          double(pushed) / TechnoLoop::kRate, TechnoLoop::kRate);
        analysis.pump();
        harness.step();
        amount.push_back(paramValue(state(client), brightness));
        lit.push_back(luminance(harness.readScreen()));
    }

    // After the first beat, which lets auto gain settle: shortly after every kick the amount is high and the picture
    // bright; just before the next kick both have fallen back.
    int kicks = 0;
    for (int i = 1; i < loop.beats(); ++i) {
        const auto on = size_t(std::lround((loop.kick(i) + 0.03) * kFps));
        const auto off = size_t(std::lround((loop.kick(i) + 0.42) * kFps));
        if (off >= amount.size())
            break;
        CAPTURE(i);
        CHECK(amount[on] > 0.67);
        CHECK(amount[off] < 0.2);
        CHECK(lit[on] > lit[off] + 15.0);
        ++kicks;
    }
    CHECK(kicks >= 6);
    CHECK(harness.document().document().layers[0].effects[0].params.at("brightness") == 0.0);
    harness.setAudio(nullptr);
}

TEST_CASE("held at start-up, a composition driven by audio starts once auto gain has had a second of audio")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1"});
    Modulator driven = audio("level", {0, opacity()});
    driven.id = 1;
    composition.modulators.push_back(driven);
    azl::EngineHarness harness({.width = 320, .height = 180, .holdStart = true}, kFps, {}, composition);
    azl::AudioAnalysis analysis([&] { return harness.time(); }, {.thread = false});
    harness.setAudio(&analysis);

    int held = 0;
    for (; held < 2 * int(kFps); ++held) {
        analysis.pump(harness.time());
        harness.step();
        if (!harness.stats().starting)
            break;
    }
    CHECK(held == int(azl::Engine::kAudioSettle * kFps));
    harness.setAudio(nullptr);
}

namespace {

// Saves "techno" from a show with Energy (0.3, on Reveal evolve) driven by a kick and a 4-beat sine, beside a bass
// modulator on the layer's opacity.
void saveTechno(const fs::path& library)
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library, revealComposition());
    Client client(harness.document());
    const uint64_t energy = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Energy", .value = 0.3, .targets = {
        {revealEvolve(), {}, 0.0, 2.0}}}}), "macro");
    apply(client, azl::AddModulator{std::nullopt, audio("kick", {energy, {}})});
    apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Sine, 4.0, {energy, {}})});
    apply(client, azl::AddModulator{std::nullopt, audio("bass", {0, opacity()}, -0.5, 0.0)});
    client.send({{"type", "modulation"}, {"op", "save"}, {"name", "techno"}});
    const Message saved = client.next();
    REQUIRE_MESSAGE(saved["op"] == "save", saved.dump());
    CHECK(saved["macros"] == 1);
    CHECK(saved["modulators"] == 2);
}

Message savedSets(const fs::path& library)
{
    std::ifstream in(library / "macro-sets.json");
    return Message::parse(in)["sets"];
}

Message load(Client& client, const std::string& name, const Message& drop = Message::array())
{
    client.send({{"type", "modulation"}, {"op", "load"}, {"name", name}, {"drop", drop}});
    const Message loaded = client.next();
    REQUIRE_MESSAGE(loaded["op"] == "load", loaded.dump());
    const Message change = client.next();
    REQUIRE_MESSAGE(change["op"] == "setMacros", change.dump());
    return loaded;
}

std::vector<uint64_t> drivenMacros(const azl::Composition& composition)
{
    std::vector<uint64_t> macros;
    for (const Modulator& modulator : composition.modulators)
        macros.push_back(modulator.target.macro);
    return macros;
}

bool drives(const Message& state, const ParamRef& param)
{
    return std::any_of(state["params"].begin(), state["params"].end(),
                       [&](const Message& p) { return p.get<ParamRef>() == param; });
}

} // namespace

TEST_CASE("a Macro Set saves names, values and modulators, never targets")
{
    TempDir library;
    saveTechno(library.path);
    const Message set = savedSets(library.path)["techno"];
    REQUIRE(set["macros"].size() == 1);
    CHECK(set["macros"][0] == Message{{"id", 1}, {"name", "Energy"}, {"value", 0.3}});
    REQUIRE(set["modulators"].size() == 2);
    for (const Message& modulator : set["modulators"]) {
        CHECK(modulator["target"] == Message{{"macro", 1}});
        CHECK_FALSE(modulator.contains("id"));
    }
    CHECK(set["modulators"][0]["feature"] == "kick");
    CHECK(set["modulators"][1]["beats"] == 4.0);
    CHECK(set.dump().find("targets") == std::string::npos);
}

TEST_CASE("a Macro Set loaded into two shows drives each show's own targets, as one undo step")
{
    TempDir library;
    saveTechno(library.path);

    SUBCASE("a show with other effect ids")
    {
        azl::Composition show;
        show.layers.push_back({.id = 7, .name = "Main", .effects = {{3, "transform", {}}, {4, "reveal", {}}}});
        show.macros.push_back({.id = 5, .name = "Old", .value = 0.9, .targets = {{{0, 4, "evolve"}, {}, 0.0, 1.5}}});
        Modulator ramp = lfo(LfoShape::Ramp, 8.0, {5, {}});
        ramp.id = 1;
        ramp.name = "Ramp";
        Modulator onParam = audio("snare", {0, opacity(7)});
        onParam.id = 2;
        onParam.name = "Snare";
        show.modulators = {ramp, onParam};
        azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, show);
        Client client(harness.document());
        show = harness.document().document();

        const Message loaded = load(client, "techno");
        CHECK(loaded["created"].empty());
        CHECK(loaded["notes"].empty());
        const azl::Composition document = harness.document().document();
        REQUIRE(document.macros.size() == 1);
        CHECK(document.macros[0].id == 5);
        CHECK(document.macros[0].name == "Energy");
        CHECK(document.macros[0].value == 0.3);
        CHECK(document.macros[0].targets == show.macros[0].targets);
        REQUIRE(document.modulators.size() == 3);
        CHECK(document.modulators[0] == show.modulators[1]);
        CHECK(drivenMacros(document) == std::vector<uint64_t>{0, 5, 5});
        CHECK(document.modulators[1].feature == "kick");
        CHECK(document.modulators[2].beats == 4.0);
        harness.step();
        CHECK(drives(state(client), {0, 4, "evolve"}));

        client.send(msg::undo());
        REQUIRE(client.next()["op"] == "setMacros");
        CHECK(harness.document().document().macros == show.macros);
        CHECK(harness.document().document().modulators == show.modulators);
        client.send(msg::redo());
        REQUIRE(client.next()["op"] == "setMacros");
        CHECK(harness.document().document().macros == document.macros);
        CHECK(harness.document().document().modulators == document.modulators);
    }

    SUBCASE("a show whose macro drives other parameters")
    {
        azl::Composition show = revealComposition();
        show.macros.push_back({.id = 2, .name = "Fade", .targets = {{opacity(), {}, -1.0, 0.0}, {{0, 2, "mix"}, {}, 0.0, 1.0}}});
        azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, show);
        Client client(harness.document());

        load(client, "techno");
        const azl::Composition document = harness.document().document();
        REQUIRE(document.macros.size() == 1);
        CHECK(document.macros[0].name == "Energy");
        CHECK(document.macros[0].targets == show.macros[0].targets);
        CHECK(drivenMacros(document) == std::vector<uint64_t>{2, 2});
        harness.step();
        const Message live = state(client);
        CHECK(drives(live, opacity()));
        CHECK(drives(live, {0, 2, "mix"}));
        CHECK_FALSE(drives(live, revealEvolve()));
    }
}

TEST_CASE("Macro Set loads: show macros beyond the set lose their modulators, set macros beyond the show arrive without targets")
{
    TempDir library;
    {
        azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, revealComposition());
        Client client(harness.document());
        for (const char* name : {"A", "B", "C"}) {
            const uint64_t id = added(apply(client, azl::AddMacro{std::nullopt, {.name = name, .value = 0.5}}), "macro");
            apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Saw, 2.0, {id, {}})});
        }
        client.send({{"type", "modulation"}, {"op", "save"}, {"name", "three"}});
        REQUIRE(client.next()["op"] == "save");
    }

    SUBCASE("a set with more macros than the show")
    {
        azl::Composition show = revealComposition();
        show.macros.push_back({.id = 4, .name = "Mine", .targets = {{revealEvolve(), {}, 0.0, 1.0}}});
        azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, show);
        Client client(harness.document());
        client.send({{"type", "modulation"}, {"op", "preview"}, {"name", "three"}});
        const Message preview = client.next();
        REQUIRE_MESSAGE(preview["op"] == "preview", preview.dump());
        std::vector<std::string> statuses;
        for (const Message& item : preview["items"])
            if (item["kind"] == "macro")
                statuses.push_back(item["status"]);
        CHECK(statuses == std::vector<std::string>{"ok", "new", "new"});
        CHECK(preview["items"][1] == Message{{"key", "d0"}, {"kind", "modulator"}, {"label", "Beat LFO -> A"},
                                             {"status", "ok"}, {"parent", "m0"}});
        CHECK(preview["replaces"] == Message{{"macros", 1}, {"modulators", 0}});

        const Message loaded = load(client, "three", {"m2"});
        CHECK(loaded["created"] == Message::array({"B"}));
        const azl::Composition document = harness.document().document();
        REQUIRE(document.macros.size() == 2);
        CHECK(document.macros[0].name == "A");
        CHECK(document.macros[0].targets == show.macros[0].targets);
        CHECK(document.macros[1].name == "B");
        CHECK(document.macros[1].id == 5);
        CHECK(document.macros[1].targets.empty());
        CHECK(drivenMacros(document) == std::vector<uint64_t>{4, 5});
    }

    SUBCASE("a show with more macros than the set")
    {
        azl::Composition show = revealComposition();
        show.layers.push_back({.id = 3, .name = "Group", .group = true});
        for (uint64_t id = 1; id <= 4; ++id)
            show.macros.push_back({.id = id, .name = "S" + std::to_string(id), .targets = {{opacity(), {}, -1.0, 0.0}}});
        show.macros.push_back({.id = 9, .name = "On group", .group = 3});
        for (uint64_t id : {1, 4, 9}) {
            Modulator m = audio("hat", {id, {}});
            m.id = id;
            m.name = "Hat " + std::to_string(id);
            show.modulators.push_back(m);
        }
        azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, show);
        Client client(harness.document());
        show = harness.document().document();

        const Message loaded = load(client, "three");
        CHECK(loaded["created"].empty());
        REQUIRE(loaded["notes"].size() == 1);
        CHECK(loaded["notes"][0].get<std::string>().starts_with("S4 is not in the set"));
        const azl::Composition document = harness.document().document();
        REQUIRE(document.macros.size() == 5);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(document.macros[i].id == show.macros[i].id);
            CHECK(document.macros[i].targets == show.macros[i].targets);
        }
        CHECK(document.macros[2].name == "C");
        CHECK(document.macros[3].name == "S4");
        CHECK(document.macros[4] == show.macros[4]);
        CHECK(drivenMacros(document) == std::vector<uint64_t>{9, 1, 2, 3});
        CHECK(document.modulators[0] == show.modulators[2]);
    }
}

TEST_CASE("an old Macro Set file with targets loads names, values and modulators; its targets are ignored and noted")
{
    TempDir library;
    {
        const Message target = {{"layer", 1}, {"layerName", "Layer 1"}, {"param", "opacity"}, {"min", -1.0}, {"max", 0.0}};
        const Message macros = Message::array(
            {{{"id", 12}, {"name", "Energy"}, {"value", 0.4}, {"group", 0}, {"targets", {target}}},
             {{"id", 13}, {"name", "Grouped"}, {"group", 30}, {"groupName", "G"}, {"targets", Message::array()}}});
        const Message modulators = Message::array(
            {{{"id", 1}, {"name", "Kick"}, {"source", "audio"}, {"feature", "kick"}, {"target", {{"macro", 12}}}},
             {{"id", 2}, {"source", "lfo"}, {"target", {{"macro", 13}}}},
             {{"id", 3}, {"source", "lfo"}, {"target", {{"layer", 1}, {"param", "opacity"}}}}});
        std::ofstream out(library.path / "macro-sets.json");
        out << Message{{"version", 1}, {"sets", {{"old", {{"macros", macros}, {"modulators", modulators}}}}}}.dump();
    }
    azl::Composition show = revealComposition();
    show.macros.push_back({.id = 1, .name = "Mine", .targets = {{revealEvolve(), {}, 0.0, 1.0}}});
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, library.path, show);
    Client client(harness.document());

    const Message loaded = load(client, "old");
    CHECK(loaded["created"].empty());
    CHECK(loaded["notes"] == Message::array({"The set's 1 target is ignored: macros keep this show's targets",
                                             "1 group macro of the set ignored",
                                             "2 modulators on parameters or group macros ignored"}));
    const azl::Composition document = harness.document().document();
    REQUIRE(document.macros.size() == 1);
    CHECK(document.macros[0].name == "Energy");
    CHECK(document.macros[0].value == 0.4);
    CHECK(document.macros[0].targets == show.macros[0].targets);
    REQUIRE(document.modulators.size() == 1);
    CHECK(document.modulators[0].name == "Kick");
    CHECK(document.modulators[0].target.macro == 1);
}

TEST_CASE("macros and modulators survive a snapshot round trip and go and come back with their layer")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    apply(client, azl::AddLayer{0, std::nullopt, {.id = 2}});
    apply(client, azl::AddMacro{std::nullopt, {.name = "Energy", .targets = {{opacity(2), {{0.1, 0.2, 0.3, 0.9}}, -1.0, 0.0}}}});
    apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Square, 2.0, {0, opacity(2)}, -1.0, 0.0)});

    Client other(harness.document());
    const azl::Composition mirrored = other.snapshot["composition"].get<azl::Composition>();
    CHECK(mirrored.macros == harness.document().document().macros);
    CHECK(mirrored.modulators == harness.document().document().modulators);

    apply(client, azl::RemoveLayer{2});
    harness.step();
    CHECK(state(client)["params"].empty());
    CHECK(harness.document().document().modulators.empty());
    client.send(msg::undo());
    CHECK(client.next()["type"] == "change");
    harness.step();
    CHECK(state(client)["params"].size() == 1);
}

namespace {

constexpr uint64_t kGroup = 10;
constexpr uint64_t kInGroup = 11;
constexpr uint64_t kClipColumn = 1;

ParamRef mixOf(uint64_t effect)
{
    return {0, effect, "mix"};
}

// Layer 1 with Reveal 1, Colorize 2 and a clip holding Colorize 3; group 10 holding layer 11 with Colorize 4; Colorize 5
// on the composition. Macro 1 drives a parameter of each, macro 2 only the clip's effect; modulator 6 drives macro 1
// and modulator 9 an effect a saved show lost.
azl::Composition removalComposition()
{
    azl::Composition composition = revealComposition();
    composition.layers[0].clips[kClipColumn] =
        azl::ClipRef{.hash = std::string(64, 'a'), .name = "clip", .effects = {{3, "colorize", {{"mix", 0.0}}}}};
    composition.layers.push_back({.id = kGroup, .name = "Group", .group = true,
                                  .layers = {{.id = kInGroup, .name = "In group",
                                              .effects = {{4, "colorize", {{"mix", 0.0}}}}}}});
    composition.effects = {{5, "colorize", {{"mix", 0.0}}}};
    const std::vector<ParamRef> params{opacity(1), revealEvolve(), mixOf(3), opacity(kInGroup), mixOf(4), mixOf(5),
                                       opacity(kGroup)};
    Macro energy{.id = 1, .name = "Energy", .value = 0.5};
    for (const ParamRef& param : params)
        energy.targets.push_back({param, {}, 0.0, 0.5});
    composition.macros = {energy, {.id = 2, .name = "Clip", .targets = {{mixOf(3), {}, 0.0, 0.5}}}};
    for (const ParamRef& param : params)
        composition.modulators.push_back(lfo(LfoShape::Saw, 2.0, {0, param}, 0.0, 0.5));
    composition.modulators.insert(composition.modulators.begin() + 5, lfo(LfoShape::Sine, 1.0, {1, {}}));
    composition.modulators.push_back(lfo(LfoShape::Ramp, 4.0, {0, mixOf(99)}));
    for (size_t i = 0; i < composition.modulators.size(); ++i) {
        composition.modulators[i].id = i + 1;
        composition.modulators[i].name = "Modulator " + std::to_string(i + 1);
    }
    return composition;
}

// The macros and modulators with every target on `gone` left out.
azl::Composition without(azl::Composition composition, const std::vector<ParamRef>& gone)
{
    auto isGone = [&](const ParamRef& p) { return std::find(gone.begin(), gone.end(), p) != gone.end(); };
    for (Macro& macro : composition.macros)
        std::erase_if(macro.targets, [&](const MacroTarget& t) { return isGone(t.param); });
    std::erase_if(composition.modulators, [&](const Modulator& m) { return !m.target.macro && isGone(m.target.param); });
    return composition;
}

// Checks that `edit` takes along exactly the macro targets and modulators on `gone`, in one change a client's mirror
// follows, that undo restores them as they were and that redo takes them again.
void checkTakesAlong(const azl::Edit& edit, const std::vector<ParamRef>& gone)
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, removalComposition());
    Client client(harness.document());
    azl::DocumentMirror mirror;
    mirror.receive(client.snapshot);
    const azl::Composition before = harness.document().document();
    const azl::Composition expected = without(before, gone);
    auto checkModulation = [&](const azl::Composition& want) {
        const azl::Composition now = harness.document().document();
        CHECK(now.macros == want.macros);
        CHECK(now.modulators == want.modulators);
        CHECK(mirror.composition().macros == want.macros);
        CHECK(mirror.composition().modulators == want.modulators);
    };
    REQUIRE(expected.modulators.size() < before.modulators.size());

    mirror.receive(apply(client, edit));
    checkModulation(expected);
    client.send(msg::undo());
    mirror.receive(client.next());
    checkModulation(before);
    client.send(msg::redo());
    mirror.receive(client.next());
    checkModulation(expected);
}

} // namespace

TEST_CASE("removing a layer takes along the macro targets and modulators on it and on its clips' effects")
{
    checkTakesAlong(azl::RemoveLayer{1}, {opacity(1), revealEvolve(), mixOf(3)});
}

TEST_CASE("removing a group takes along the macro targets and modulators on it and on its layers")
{
    checkTakesAlong(azl::RemoveLayer{kGroup}, {opacity(kGroup), opacity(kInGroup), mixOf(4)});
}

TEST_CASE("removing an effect takes along the macro targets and modulators on it, in any chain")
{
    SUBCASE("a layer's") { checkTakesAlong(azl::RemoveEffect{1}, {revealEvolve()}); }
    SUBCASE("a clip's") { checkTakesAlong(azl::RemoveEffect{3}, {mixOf(3)}); }
    SUBCASE("the composition's") { checkTakesAlong(azl::RemoveEffect{5}, {mixOf(5)}); }
}

TEST_CASE("clearing or removing a clip takes along the macro targets and modulators on its effects")
{
    SUBCASE("an emptied cell") { checkTakesAlong(azl::SetSlot{1, kClipColumn, std::nullopt}, {mixOf(3)}); }
    SUBCASE("a removed column") { checkTakesAlong(azl::RemoveColumn{kClipColumn}, {mixOf(3)}); }
}

namespace {

// The removal composition with Cue 1 stepping Reveal 1 and Colorize 3, and Cue 2 setting a macro only.
azl::Composition removalCompositionWithCues()
{
    azl::Composition composition = removalComposition();
    composition.cues = {{.id = 1, .name = "Drop", .effects = {{1, azl::AutopilotMode::Next}, {3, azl::AutopilotMode::Next}}},
                        {.id = 2, .name = "Build", .macros = {{2, 1.0, 0}}}};
    return composition;
}

} // namespace

TEST_CASE("a change that removes an effect says what went with it, and undo brings them back together")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, removalCompositionWithCues());
    Client client(harness.document());
    const azl::Composition before = harness.document().document();
    auto document = [&] { return harness.document().document(); };
    // The effects (by id and type, a put-back effect carries its completed parameters) and what drives them.
    auto same = [&](const azl::Composition& a, const azl::Composition& b) {
        auto effects = [](const azl::Composition& c) {
            std::vector<std::pair<uint64_t, std::string>> ids;
            azl::forEachChain(c, [&](const std::vector<azl::Effect>& chain, uint64_t, std::optional<uint64_t>) {
                for (const azl::Effect& e : chain)
                    ids.emplace_back(e.id, e.type);
            });
            return ids;
        };
        return effects(a) == effects(b) && a.macros == b.macros && a.modulators == b.modulators && a.cues == b.cues;
    };

    SUBCASE("removing an effect")
    {
        // Reveal 1 is driven by macro Energy, modulator 2 and Cue Drop.
        const Message change = apply(client, azl::RemoveEffect{1});
        CHECK(change["notice"] ==
              "Gone with what the edit removed: macro Energy's target effect 1 evolve; modulator Modulator 2 on effect "
              "1 evolve; cue Drop's step of effect 1");
        CHECK(document().cues[0].effects == std::vector<azl::CueEffect>{{3, azl::AutopilotMode::Next}});
        CHECK(document().cues[1] == before.cues[1]);
        CHECK(document().modulators.size() == before.modulators.size() - 1);

        client.send(msg::undo());
        const Message undone = client.next();
        CHECK(undone["type"] == "change");
        CHECK(!undone.contains("notice"));
        CHECK(same(document(), before));

        client.send(msg::redo());
        CHECK(client.next()["notice"] == change["notice"]);
        CHECK(document().cues[0].effects == std::vector<azl::CueEffect>{{3, azl::AutopilotMode::Next}});
    }
    SUBCASE("replacing an effect by another in one batch")
    {
        const Message change = apply(
            client, azl::Batch{{azl::RemoveEffect{1}, azl::AddEffect{1, 0, {0, "colorize", {{"mix", 0.5}}}}}});
        CHECK(change["notice"] ==
              "Gone with what the edit removed: macro Energy's target effect 1 evolve; modulator Modulator 2 on effect "
              "1 evolve; cue Drop's step of effect 1");
        CHECK(document().layers[0].effects[0].type == "colorize");
        client.send(msg::undo());
        client.next();
        CHECK(same(document(), before));
    }
    SUBCASE("replacing a clip with another")
    {
        // Colorize 3 on the clip is driven by macros Energy and Clip, modulator 3 and Cue Drop.
        const Message change = apply(client, azl::SetSlot{1, kClipColumn, azl::ClipRef{.hash = std::string(64, 'b'), .name = "other"}});
        CHECK(change["notice"] ==
              "Gone with what the edit removed: macro Energy's target effect 3 mix; macro Clip's target effect 3 mix; "
              "modulator Modulator 3 on effect 3 mix; cue Drop's step of effect 3");
        CHECK(document().cues[0].effects == std::vector<azl::CueEffect>{{1, azl::AutopilotMode::Next}});
        client.send(msg::undo());
        client.next();
        CHECK(same(document(), before));
    }
    SUBCASE("an edit that removes nothing driven carries no notice")
    {
        CHECK(!apply(client, azl::RemoveEffect{2}).contains("notice"));
        CHECK(!apply(client, azl::SetEffectParam{3, "mix", 0.2}).contains("notice"));
    }
}

TEST_CASE("each LFO shape takes its value at fixed phases; a custom curve runs between its points and wraps round")
{
    struct Sample {
        LfoShape shape;
        double phase;
        double value;
    };
    const Sample samples[] = {
        {LfoShape::Sine, 0.0, 1.0},    {LfoShape::Sine, 0.25, 0.5},   {LfoShape::Sine, 0.5, 0.0},
        {LfoShape::Sine, 0.75, 0.5},   {LfoShape::Saw, 0.0, 1.0},     {LfoShape::Saw, 0.25, 0.75},
        {LfoShape::Saw, 0.5, 0.5},     {LfoShape::Square, 0.0, 1.0},  {LfoShape::Square, 0.25, 1.0},
        {LfoShape::Square, 0.75, 0.0}, {LfoShape::Ramp, 0.0, 0.0},    {LfoShape::Ramp, 0.25, 0.25},
        {LfoShape::Ramp, 0.5, 0.5},    {LfoShape::Ramp, 1.75, 0.75},
    };
    for (const Sample& s : samples)
        CHECK(azl::lfoAt(s.shape, s.phase) == doctest::Approx(s.value));

    const std::vector<azl::LfoPoint> points = {{0.25, 0.2}, {0.5, 1.0, true}, {0.75, 0.0}};
    const Sample custom[] = {
        {LfoShape::Custom, 0.25, 0.2},     {LfoShape::Custom, 0.375, 0.6}, {LfoShape::Custom, 0.5, 1.0},
        {LfoShape::Custom, 0.5625, 0.84375}, {LfoShape::Custom, 0.625, 0.5}, {LfoShape::Custom, 0.75, 0.0},
        {LfoShape::Custom, 0.875, 0.05},   {LfoShape::Custom, 0.1, 0.14},  {LfoShape::Custom, 2.375, 0.6},
    };
    for (const Sample& s : custom)
        CHECK(azl::lfoAt(s.shape, s.phase, points) == doctest::Approx(s.value));
    CHECK(azl::lfoAt(LfoShape::Custom, 0.3, {{0.6, 0.4}}) == doctest::Approx(0.4));
    CHECK(azl::lfoAt(LfoShape::Custom, 0.3) == 0.0);
    // The points stay with a modulator on another shape and change nothing there.
    CHECK(azl::lfoAt(LfoShape::Ramp, 0.3, points) == doctest::Approx(0.3));

    // The engine plays the same curve: 64 phases over a 4-beat period.
    azl::Composition composition = revealComposition();
    Modulator m = lfo(LfoShape::Custom, 4.0, {0, revealEvolve()});
    m.id = 1;
    m.points = points;
    composition.modulators = {m};
    azl::ModulationPlan plan(composition);
    for (int i = 0; i < 64; ++i) {
        plan.evaluate(0.0, 4.0 * i / 64.0, azl::AudioFrame{});
        CHECK(plan.modulators()[0].value == doctest::Approx(azl::lfoAt(LfoShape::Custom, i / 64.0, points)));
    }
}

TEST_CASE("a custom LFO curve is validated, saved with its modulator and loaded back; older modulators save as before")
{
    azl::Composition composition = revealComposition();
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, composition);
    Client client(harness.document());
    auto error = [&](std::vector<azl::LfoPoint> points, LfoShape shape = LfoShape::Custom) {
        Modulator m = lfo(shape, 1.0, {0, revealEvolve()});
        m.points = std::move(points);
        return command(client, azl::AddModulator{std::nullopt, m})["type"] == "error";
    };
    CHECK(error({}));
    CHECK(error({{0.5, 0.2}, {0.25, 0.4}}));
    CHECK(error({{0.5, 0.2}, {0.5, 0.4}}));
    CHECK(error({{1.0, 0.2}}));
    CHECK(error({{-0.1, 0.2}}));
    CHECK(error({{0.5, 1.5}}));
    CHECK(error({{0.5, 1.5}}, LfoShape::Sine));
    std::vector<azl::LfoPoint> many;
    for (size_t i = 0; i <= azl::kMaxLfoPoints; ++i)
        many.push_back({double(i) / 32.0, 0.5});
    CHECK(error(many));
    many.pop_back();
    CHECK_FALSE(error(many));

    Modulator m = lfo(LfoShape::Custom, 2.0, {0, revealEvolve()});
    m.points = {{0.0, 0.1}, {0.3, 0.9, true}, {0.7, 0.4}};
    const Message j = m;
    CHECK(j["shape"] == "custom");
    CHECK(j["points"][1] == Message{{"phase", 0.3}, {"value", 0.9}, {"smooth", true}});
    CHECK_FALSE(j["points"][0].contains("smooth"));
    CHECK(j.get<Modulator>() == m);
    const Message old = lfo(LfoShape::Saw, 2.0, {0, revealEvolve()});
    CHECK_FALSE(old.contains("points"));
    CHECK(old.get<Modulator>().points.empty());

    TempDir dir;
    m.id = 1;
    composition.modulators = {m};
    azl::saveCompositionFile(dir.path / "show.json", {composition});
    const auto loaded = azl::loadCompositionFile(dir.path / "show.json");
    REQUIRE(loaded);
    CHECK(loaded->composition.modulators == composition.modulators);
    apply(client, azl::LoadComposition{loaded->composition});
    CHECK(harness.document().document().modulators == composition.modulators);
}

namespace {

struct CtlRun {
    std::string out;
    int status = 0;
};

CtlRun ctl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    CtlRun run;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        run.out += buffer;
    const int wait = pclose(pipe);
    run.status = WEXITSTATUS(wait);
    return run;
}

} // namespace

TEST_CASE("ctl mod: points= sets a custom LFO curve; shape= switches away and keeps it")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    auto modulators = [&] { return harness.document().document().modulators; };

    CHECK(ctl(port, "mod lfo 1.evolve sine 2 points=0:0.2s,0.5:1").status == 0);
    REQUIRE(modulators().size() == 1);
    CHECK(modulators()[0].shape == LfoShape::Custom);
    CHECK(modulators()[0].points == std::vector<azl::LfoPoint>{{0.0, 0.2, true}, {0.5, 1.0}});
    const std::string id = std::to_string(modulators()[0].id);
    CHECK(ctl(port, "mod set " + id + " points=0.25:0.5").status == 0);
    CHECK(modulators()[0].points == std::vector<azl::LfoPoint>{{0.25, 0.5}});
    const CtlRun unordered = ctl(port, "mod set " + id + " points=0.5:0.2,0.25:0.4");
    CHECK(unordered.status != 0);
    CHECK(unordered.out.find("phase order") != std::string::npos);
    CHECK(ctl(port, "mod set " + id + " shape=ramp").status == 0);
    CHECK(modulators()[0].shape == LfoShape::Ramp);
    CHECK(modulators()[0].points.size() == 1);
    CHECK(ctl(port, "mod set " + id + " shape=custom").status == 0);
    CHECK(modulators()[0].shape == LfoShape::Custom);
}

TEST_CASE("a random LFO holds one value per period, the same for the same beat")
{
    std::set<double> values;
    for (int period = 0; period < 16; ++period) {
        const double v = azl::lfoAt(LfoShape::Random, period + 0.1);
        CHECK(v >= 0.0);
        CHECK(v < 1.0);
        CHECK(azl::lfoAt(LfoShape::Random, period + 0.9) == v);
        values.insert(v);
    }
    CHECK(values.size() == 16);
    CHECK(azl::lfoAt(LfoShape::Random, -2.5) == azl::lfoAt(LfoShape::Random, -2.9));

    const Message j = lfo(LfoShape::Random, 2.0, {0, opacity()});
    CHECK(j["shape"] == "random");
    CHECK(j.get<Modulator>().shape == LfoShape::Random);
}

TEST_CASE("an audio modulator follows its feature through its attack and release")
{
    azl::Composition composition = revealComposition();
    Modulator kick = audio("kick", {0, revealEvolve()});
    kick.id = 1;
    kick.attack = 10.0;
    kick.release = 200.0;
    const Message j = kick;
    CHECK(j["release"] == 200.0);
    CHECK(j.get<Modulator>().attack == 10.0);
    composition.modulators.push_back(kick);
    azl::ModulationPlan plan(composition);

    azl::AudioFrame frame;
    auto run = [&](float value, double seconds) {
        frame.values[size_t(azl::AudioFeature::Kick)] = value;
        for (int i = 0; i < int(std::lround(seconds * kFps)); ++i) {
            frame.time += 1.0 / kFps;
            plan.evaluate(0.0, 0.0, frame);
        }
        return plan.modulators()[0].value;
    };
    run(0.0f, 0.1);
    CHECK(run(1.0f, 0.05) > 0.95);
    CHECK(run(0.0f, 0.1) == doctest::Approx(std::exp(-0.1 / 0.2)).epsilon(0.05));
    CHECK(run(0.0f, 1.0) < 0.02);
}

TEST_CASE("an audio-driven macro eases the same at any frame rate, through stalled audio and plan rebuilds")
{
    azl::Composition composition = revealComposition();
    composition.macros.push_back({.id = 5, .name = "Wet", .targets = {{revealEvolve(), {}, 0.0, 1.0}}});
    Modulator snare = audio("snare", {5, {}});
    snare.id = 1;
    snare.attack = 30.0;
    snare.release = 300.0;
    composition.modulators.push_back(snare);
    CHECK_FALSE(Message(Modulator{}).contains("attack"));
    CHECK(Message(Modulator{}).get<Modulator>().release == 0.0);

    // A snare hit at 105 ms whose feature then falls within 150 ms, rendered for 500 ms; audio arrives every
    // `repeats` frames.
    auto render = [&](double fps, int repeats) {
        azl::ModulationPlan plan(composition);
        azl::AudioFrame frame;
        std::vector<double> values;
        for (int i = 1; i <= int(std::lround(0.5 * fps)); ++i) {
            const double t = i / fps;
            if (i % repeats == 0) {
                frame.time = t;
                frame.values[size_t(azl::AudioFeature::Snare)] = t >= 0.105 ? float(std::exp(-(t - 0.105) / 0.15)) : 0.0f;
            }
            plan.evaluate(0.0, 0.0, frame);
            values.push_back(plan.macros()[0].value);
        }
        return values;
    };
    auto largestStep = [](const std::vector<double>& values) {
        double step = 0.0;
        for (size_t i = 1; i < values.size(); ++i)
            step = std::max(step, std::abs(values[i] - values[i - 1]));
        return step;
    };
    const std::vector<double> at60 = render(60.0, 1);
    const std::vector<double> at240 = render(240.0, 1);
    const double peak = *std::max_element(at60.begin(), at60.end());
    CHECK(peak > 0.55);
    CHECK(peak < 0.75);
    CHECK(largestStep(at60) < 0.45);
    CHECK(at60.back() > 0.3);
    CHECK(at60.back() == doctest::Approx(at240.back()).epsilon(0.05));
    // Render frames that see no new audio hold the value instead of jumping to the raw feature.
    const std::vector<double> stalled = render(240.0, 4);
    CHECK(largestStep(stalled) < 0.45);
    CHECK(stalled.back() == doctest::Approx(at60.back()));

    // A document edit rebuilds the plan; the release continues instead of restarting from zero.
    azl::ModulationPlan before(composition);
    azl::AudioFrame frame;
    frame.values[size_t(azl::AudioFeature::Snare)] = 1.0f;
    for (int i = 1; i <= 30; ++i) {
        frame.time = i / kFps;
        before.evaluate(0.0, 0.0, frame);
    }
    const double held = before.macros()[0].value;
    CHECK(held > 0.9);
    composition.macros[0].name = "Dry/wet";
    azl::ModulationPlan after(composition);
    after.carry(before);
    frame.values[size_t(azl::AudioFeature::Snare)] = 0.0f;
    frame.time += 1.0 / kFps;
    after.evaluate(0.0, 0.0, frame);
    CHECK(after.macros()[0].value == doctest::Approx(held));
    frame.time += 1.0 / kFps;
    after.evaluate(0.0, 0.0, frame);
    CHECK(after.macros()[0].value == doctest::Approx(held * std::exp(-1.0 / kFps / 0.3)).epsilon(0.01));
}

namespace {

// Renders a level-driven modulator at `fps` while `input(t)` drives the level; returns its value per frame.
std::vector<double> renderLevel(const Modulator& modulator, double fps, double seconds,
                                const std::function<double(double)>& input)
{
    azl::Composition composition = revealComposition();
    composition.modulators.push_back(modulator);
    composition.modulators.back().id = 1;
    azl::ModulationPlan plan(composition);
    azl::AudioFrame frame;
    std::vector<double> values;
    for (int i = 0; i <= int(std::lround(seconds * fps)); ++i) {
        frame.time = i / fps;
        frame.values[size_t(azl::AudioFeature::Level)] = float(input(frame.time));
        plan.evaluate(0.0, 0.0, frame);
        values.push_back(plan.modulators()[0].value);
    }
    return values;
}

// The first time at which `values` (one per frame at `fps`) reaches `level` going the way `rising` says.
double crossing(const std::vector<double>& values, double fps, double level, bool rising, double after = 0.0)
{
    for (size_t i = size_t(std::lround(after * fps)); i < values.size(); ++i)
        if (rising ? values[i] >= level : values[i] <= level)
            return double(i) / fps;
    return 1e9;
}

} // namespace

TEST_CASE("each envelope preset rises within its attack, holds, then falls linearly in dB at its release")
{
    constexpr double fps = 1000.0;
    for (const azl::EnvelopePreset preset : {azl::EnvelopePreset::Meter, azl::EnvelopePreset::Punchy,
                                             azl::EnvelopePreset::Smooth, azl::EnvelopePreset::Breathe}) {
        const std::string name = azl::envelopePresetName(preset);
        CAPTURE(name);
        Modulator m = audio("level", {0, revealEvolve()});
        azl::setEnvelope(m, preset);
        CHECK(m.attack > 0.0);
        CHECK(m.release > 0.0);
        const double attack = m.attack / 1000.0, hold = m.hold / 1000.0, release = m.release / 1000.0;
        // A step up at 0.1 s, down at 5 s.
        const std::vector<double> v = renderLevel(m, fps, 5.0 + hold + 4.0 * release,
                                                  [](double t) { return t >= 0.1 && t < 5.0 ? 1.0 : 0.0; });
        // Each frame takes in the feature as of its end, so crossings come up to a frame early.
        const double frame = 1.5 / fps;
        CHECK(std::abs(crossing(v, fps, 0.9, true) - 0.1 - attack) <= frame);
        CHECK(v[size_t(std::lround((5.0 + hold) * fps)) - 2] >= 0.999);
        const double half = crossing(v, fps, 0.5, false, 5.0) - 5.0 - hold;
        const double tenth = crossing(v, fps, 0.1, false, 5.0) - 5.0 - hold;
        CHECK(std::abs(half - release) <= frame);
        // Linear in dB: 6 dB per release all the way down, so 20 dB takes 3.32 releases.
        CHECK(std::abs(tenth - release * std::log2(10.0)) <= frame);
    }
    Modulator meter = audio("level", {0, revealEvolve()});
    azl::setEnvelope(meter, azl::EnvelopePreset::Meter);
    CHECK(meter.attack == 10.0);
    CHECK(meter.hold == 40.0);
    CHECK(meter.release == 500.0);

    // The fall stops where the feature is, instead of easing towards it.
    const std::vector<double> floor = renderLevel(meter, fps, 3.0, [](double t) { return t < 1.0 ? 1.0 : 0.4; });
    CHECK(floor[size_t(1.5 * fps)] == doctest::Approx(std::exp2(-(0.5 - 0.04) / 0.5)).epsilon(0.01));
    CHECK(floor.back() == doctest::Approx(0.4));
    CHECK(crossing(floor, fps, 0.4 + 1e-6, false, 1.0) - 1.0 < 0.04 + 0.5 * std::log2(1.0 / 0.4) + 0.002);

    // The same at 60 fps as at 1000.
    const auto step = [](double t) { return t >= 0.1 && t < 1.0 ? 1.0 : 0.0; };
    const std::vector<double> at60 = renderLevel(meter, kFps, 2.0, step);
    const std::vector<double> at1000 = renderLevel(meter, fps, 2.0, step);
    CHECK(at60.back() == doctest::Approx(at1000.back()).epsilon(0.03));
}

TEST_CASE("a Gate jumps to 1 on each hit, holds and decays to 0 in a fixed time whatever the feature does")
{
    Modulator gate = audio("kick", {0, revealEvolve()});
    azl::setEnvelope(gate, azl::EnvelopePreset::Gate);
    gate.id = 1;
    const double hold = gate.hold / 1000.0, decay = gate.release / 1000.0;
    CHECK(hold > 0.0);
    CHECK(decay > 0.0);
    azl::Composition composition = revealComposition();
    composition.modulators.push_back(gate);
    azl::ModulationPlan plan(composition);
    azl::AudioFrame frame;
    frame.onsets[0] = 7;
    constexpr double fps = 1000.0;
    std::vector<double> v;
    for (int i = 0; i <= 2000; ++i) {
        frame.time = i / fps;
        // Hits at 0.5 s and 1.5 s; the kick's own value stays up the whole time.
        frame.onsets[0] = 7 + (frame.time >= 0.5) + (frame.time >= 1.5);
        frame.values[size_t(azl::AudioFeature::Kick)] = frame.time >= 0.5 ? 1.0f : 0.0f;
        plan.evaluate(0.0, 0.0, frame);
        v.push_back(plan.modulators()[0].value);
    }
    CHECK(v[499] == 0.0);
    CHECK(v[500] == 1.0);
    CHECK(v[size_t(std::lround((0.5 + hold) * fps)) - 1] == 1.0);
    CHECK(v[size_t(std::lround((0.5 + hold + decay / 2) * fps))] == doctest::Approx(0.1).epsilon(0.02));
    CHECK(v[size_t(std::lround((0.5 + hold + decay) * fps)) - 2] > 0.0);
    CHECK(v[size_t(std::lround((0.5 + hold + decay) * fps)) + 1] == 0.0);
    CHECK(v[1499] == 0.0);
    CHECK(v[1500] == 1.0);

    // A level Gate triggers on rising past 0.6 and again only after falling below 0.3.
    Modulator level = gate;
    level.feature = "level";
    const std::vector<double> l = renderLevel(level, fps, 2.0, [](double t) {
        return t < 0.5 ? 0.2 : t < 0.6 ? 0.7 : t < 1.0 ? 0.5 : t < 1.2 ? 0.8 : t < 1.5 ? 0.1 : 0.9;
    });
    CHECK(l[499] == 0.0);
    CHECK(l[500] == 1.0);
    CHECK(l[1199] == 0.0);
    CHECK(l[1500] == 1.0);
}

namespace {

// The law from before presets, written out: what an old show's modulators must keep doing to the bit.
std::vector<float> oldLaw(double attackMs, double releaseMs, const std::vector<float>& input, double fps)
{
    std::vector<float> out;
    double v = 0.0;
    double last = -1.0;
    for (size_t i = 0; i < input.size(); ++i) {
        const double time = double(i) / fps;
        const double dt = last < 0.0 ? 0.0 : time - last;
        last = time;
        double x = std::clamp(double(input[i]) * 1.0, 0.0, 1.0);
        const double tau = (x > v ? attackMs : releaseMs) / 1000.0;
        if (tau > 0.0 && dt < 0.5)
            x = v + (x - v) * (1.0 - std::exp(-std::max(dt, 0.0) / tau));
        v = x;
        out.push_back(float(v));
    }
    return out;
}

} // namespace

TEST_CASE("modulators of old shows load without a preset and follow their feature bit-exactly as before")
{
    const Message file = Message::parse(R"([
        {"id": 1, "source": "audio", "feature": "level", "target": {"macro": 5}},
        {"id": 2, "source": "audio", "feature": "level", "target": {"macro": 5}, "attack": 30, "release": 300}])");
    const auto modulators = file.get<std::vector<Modulator>>();
    for (const Modulator& m : modulators) {
        CHECK(m.envelope == azl::EnvelopePreset::None);
        CHECK(m.hold == 0.0);
        CHECK_FALSE(Message(m).contains("envelope"));
        CHECK_FALSE(Message(m).contains("hold"));
    }
    std::vector<float> input;
    for (int i = 0; i < 600; ++i)
        input.push_back(float(0.5 + 0.45 * std::sin(i * 0.37) * std::cos(i * 0.011)));
    azl::Composition composition = revealComposition();
    composition.macros.push_back({.id = 5, .name = "Wet", .targets = {{revealEvolve(), {}, 0.0, 1.0}}});
    composition.modulators = modulators;
    azl::ModulationPlan plan(composition);
    azl::AudioFrame frame;
    std::vector<float> raw, eased;
    for (size_t i = 0; i < input.size(); ++i) {
        frame.time = double(i) / kFps;
        frame.values[size_t(azl::AudioFeature::Level)] = input[i];
        plan.evaluate(0.0, 0.0, frame);
        raw.push_back(float(plan.modulators()[0].value));
        eased.push_back(float(plan.modulators()[1].value));
    }
    CHECK(raw == oldLaw(0.0, 0.0, input, kFps));
    CHECK(eased == oldLaw(30.0, 300.0, input, kFps));
    CHECK(raw == input);
}

TEST_CASE("a modulator keeps its preset's name; other times make it Custom; modenv puts all on one in an undo step")
{
    Modulator m = audio("level", {0, revealEvolve()});
    azl::setEnvelope(m, azl::EnvelopePreset::Punchy);
    const Message j = m;
    CHECK(j["envelope"] == "punchy");
    CHECK(j["attack"] == 5.0);
    CHECK(j.get<Modulator>() == m);
    const Modulator gate = Message::parse(R"({"source": "audio", "feature": "kick", "envelope": "gate",
                                             "target": {"macro": 1}})").get<Modulator>();
    CHECK(gate.envelope == azl::EnvelopePreset::Gate);
    CHECK(gate.release == 180.0);
    CHECK(gate.hold == 40.0);
    CHECK(azl::envelopePresetFromName("breathe") == azl::EnvelopePreset::Breathe);
    CHECK_FALSE(azl::envelopePresetFromName("loud"));

    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const uint64_t id = added(apply(client, azl::AddModulator{std::nullopt, m}), "modulator");
    auto stored = [&]() { return harness.document().document().modulators[0]; };
    CHECK(stored().envelope == azl::EnvelopePreset::Punchy);
    Modulator edited = stored();
    edited.release = 400.0;
    CHECK(apply(client, azl::SetModulator{edited})["modulator"]["envelope"] == "custom");
    CHECK(stored().envelope == azl::EnvelopePreset::Custom);
    edited = stored();
    azl::setEnvelope(edited, azl::EnvelopePreset::Smooth);
    apply(client, azl::SetModulator{edited});
    CHECK(stored().envelope == azl::EnvelopePreset::Smooth);
    edited.hold = 20.0;
    apply(client, azl::SetModulator{edited});
    CHECK(stored().envelope == azl::EnvelopePreset::Custom);
    CHECK(stored().hold == 20.0);
    CHECK(command(client, azl::SetModulator{[&] {
                      Modulator bad = stored();
                      bad.hold = 20000.0;
                      return bad;
                  }()})["type"] == "error");

    // An old raw modulator and a beat LFO beside it: modenv all moves only the audio one.
    apply(client, azl::AddModulator{std::nullopt, audio("bass", {0, opacity()})});
    apply(client, azl::AddModulator{std::nullopt, lfo(LfoShape::Saw, 2.0, {0, opacity()})});
    const azl::Composition before = harness.document().document();
    CHECK(before.modulators[1].envelope == azl::EnvelopePreset::None);
    apply(client, azl::setEnvelopes(before, azl::EnvelopePreset::Meter));
    const azl::Composition after = harness.document().document();
    CHECK(after.modulators[0].envelope == azl::EnvelopePreset::Meter);
    CHECK(after.modulators[1].envelope == azl::EnvelopePreset::Meter);
    CHECK(after.modulators[1].release == 500.0);
    CHECK(after.modulators[2] == before.modulators[2]);
    CHECK(after.modulators[0].id == id);
    client.send(msg::undo());
    client.next();
    CHECK(harness.document().document().modulators == before.modulators);
}

TEST_CASE("a macro target is replaced in one undoable edit")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const uint64_t macro = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Energy"}}), "macro");
    apply(client, azl::AddMacroTarget{macro, std::nullopt, {revealEvolve(), {}, 0.0, 1.0}});
    const MacroTarget eased{opacity(), {{0.42, 0.0, 1.0, 1.0}}, -0.5, 0.0};
    const Message change = apply(client, azl::SetMacroTarget{macro, 0, eased});
    CHECK(change["target"]["min"] == -0.5);
    CHECK(harness.document().document().macros[0].targets[0] == eased);
    CHECK(command(client, azl::SetMacroTarget{macro, 1, eased})["type"] == "error");
    CHECK(command(client, azl::SetMacroTarget{macro, 0, {revealEvolve(), {}, 0.0, 9.0}})["type"] == "error");
    client.send(msg::undo());
    CHECK(client.next()["target"]["param"] == "evolve");
    CHECK(harness.document().document().macros[0].targets[0].param == revealEvolve());
}

TEST_CASE("layers, decks, columns, macros and screens are renamed by undoable edits")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const uint64_t macro = added(apply(client, azl::AddMacro{std::nullopt, {.name = "Energy"}}), "macro");
    apply(client, azl::Rename{"layer", 1, "Base"});
    apply(client, azl::Rename{"deck", 1, "Intro"});
    apply(client, azl::Rename{"column", 2, "Drop"});
    apply(client, azl::Rename{"macro", macro, "Heat"});
    apply(client, azl::AddScreen{std::nullopt, azl::Screen{.name = "Projector"}});
    const uint64_t screen = harness.document().document().screens[0].id;
    apply(client, azl::Rename{"screen", screen, "Left wall"});
    CHECK(harness.document().document().screens[0].name == "Left wall");
    client.send(msg::undo());
    client.next();
    CHECK(harness.document().document().screens[0].name == "Projector");
    client.send(msg::undo());
    client.next();
    azl::Composition document = harness.document().document();
    CHECK(document.layers[0].name == "Base");
    CHECK(document.decks[0].name == "Intro");
    CHECK(document.decks[0].columns[1].name == "Drop");
    CHECK(document.macros[0].name == "Heat");
    CHECK(command(client, azl::Rename{"screen", 99, "x"})["type"] == "error");
    CHECK(command(client, azl::Rename{"slice", 1, "x"})["type"] == "error");
    CHECK(command(client, azl::Rename{"layer", 99, "x"})["type"] == "error");
    CHECK(command(client, azl::Rename{"layer", 1, ""})["type"] == "error");
    client.send(msg::undo());
    CHECK(client.next()["name"] == "Energy");
    client.send(msg::undo());
    client.next();
    CHECK(harness.document().document().decks[0].columns[1].name == "Column 2");
}

TEST_CASE("slices and masks, of a screen or of a slice, are renamed by undoable edits")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    azl::Screen screen{.name = "Projector"};
    screen.slices.push_back(azl::Slice{.name = "Slice 1"});
    apply(client, azl::AddScreen{std::nullopt, screen});
    const auto screenOf = [&] { return harness.document().document().screens[0]; };
    const std::vector<azl::Point> square{{0.1, 0.1}, {0.5, 0.1}, {0.5, 0.5}, {0.1, 0.5}};
    apply(client, azl::AddMask{screenOf().id, 0, std::nullopt, {.name = "Door", .points = square}});
    apply(client, azl::AddMask{screenOf().id, screenOf().slices[0].id, std::nullopt, {.name = "Beam", .points = square}});
    apply(client, azl::Rename{"slice", screenOf().slices[0].id, "Left"});
    apply(client, azl::Rename{"mask", screenOf().masks[0].id, "Exit"});
    apply(client, azl::Rename{"mask", screenOf().slices[0].masks[0].id, "Truss"});
    CHECK(screenOf().slices[0].name == "Left");
    CHECK(screenOf().masks[0].name == "Exit");
    CHECK(screenOf().slices[0].masks[0].name == "Truss");
    CHECK(command(client, azl::Rename{"mask", 999, "x"})["type"] == "error");
    for (int i = 0; i < 3; ++i) {
        client.send(msg::undo());
        client.next();
    }
    CHECK(screenOf().slices[0].name == "Slice 1");
    CHECK(screenOf().masks[0].name == "Door");
    CHECK(screenOf().slices[0].masks[0].name == "Beam");
}

TEST_CASE("a source's sensitivity scales what it drives, as an undoable edit kept with the show")
{
    azl::Composition composition = revealComposition();
    Modulator kick = audio("kick", {0, revealEvolve()}, 0.0, 2.0);
    kick.id = 1;
    Modulator sine = lfo(LfoShape::Square, 1.0, {0, opacity()}, -1.0, 0.0);
    sine.id = 2;
    composition.modulators = {kick, sine};
    composition.sensitivity = {{"kick", 2.0}, {"lfo", 0.5}};
    azl::ModulationPlan plan(composition);
    azl::AudioFrame frame;
    frame.values[size_t(azl::AudioFeature::Kick)] = 0.3f;
    plan.evaluate(0.0, 0.25, frame);
    CHECK(plan.modulators()[0].value == doctest::Approx(0.6));
    CHECK(plan.modulators()[1].value == doctest::Approx(0.5));
    frame.values[size_t(azl::AudioFeature::Kick)] = 0.8f;
    plan.evaluate(0.0, 0.25, frame);
    CHECK(plan.modulators()[0].value == doctest::Approx(1.0));

    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const Message change = apply(client, azl::SetSensitivity{"snare", 2.5});
    CHECK(change["value"] == 2.5);
    CHECK(harness.document().document().sensitivity.at("snare") == 2.5);
    CHECK(command(client, azl::SetSensitivity{"level", 1.0})["type"] == "error");
    CHECK(command(client, azl::SetSensitivity{"kick", 5.0})["type"] == "error");
    const Message snapshot = harness.document().document();
    CHECK(snapshot["sensitivity"]["snare"] == 2.5);
    CHECK(snapshot.get<azl::Composition>().sensitivity.at("snare") == 2.5);
    CHECK_FALSE(Message(revealComposition()).contains("sensitivity"));
    client.send(msg::undo());
    CHECK(client.next()["value"] == 1.0);
}

TEST_CASE("a Stripper's speed ramped live moves its strips smoothly, across plan rebuilds and through a macro")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "stripper", {}}}});
    const azl::PlannedPass pass = azl::planChain(azl::SourceKind::Yuv, {azl::findEffect("stripper")})[0];
    const size_t speed = 2;
    std::vector<float> values;
    auto offset = [&](const azl::ModulationPlan& plan) {
        const azl::EffectList effects = azl::effectList(composition.layers[0].effects);
        azl::paramsOf(pass, effects, values);
        plan.apply(pass, effects, values);
        return double(values[speed]);
    };
    // Late in a show, where time times speed would jump by over 30 strips a frame.
    const double start = 100.0;
    const int frames = 12;

    // A constant speed gives time times speed, as before.
    composition.layers[0].effects[0].params["speed"] = 0.5;
    azl::ModulationPlan constant(composition);
    constant.evaluate(start, 0.0, {});
    CHECK(offset(constant) == doctest::Approx(0.5 * start));

    // Each edit rebuilds the plan.
    std::unique_ptr<azl::ModulationPlan> plan;
    double previous = 0.0;
    for (int i = 0; i <= frames; ++i) {
        composition.layers[0].effects[0].params["speed"] = 4.0 * i / frames;
        auto next = std::make_unique<azl::ModulationPlan>(composition);
        if (plan)
            next->carry(*plan);
        plan = std::move(next);
        plan->evaluate(start + i / kFps, 0.0, {});
        if (i > 0)
            CHECK(offset(*plan) - previous == doctest::Approx(4.0 * i / frames / kFps).epsilon(1e-3));
        previous = offset(*plan);
    }

    // A macro drives the speed within one plan.
    composition.layers[0].effects[0].params["speed"] = 0.0;
    composition.macros = {{.id = 1, .targets = {{{0, 1, "speed"}, {}, 0.0, 4.0}}}};
    azl::ModulationPlan driven(composition);
    for (int i = 0; i <= frames; ++i) {
        driven.cue({{1, double(i) / frames}});
        driven.evaluate(start + i / kFps, 0.0, {});
        if (i > 0)
            CHECK(offset(driven) - previous == doctest::Approx(4.0 * i / frames / kFps).epsilon(1e-3));
        previous = offset(driven);
    }
}

TEST_CASE("a Stripper's speed spread integrates with its speed, its reshuffle counts beats, its audio band reads levels")
{
    azl::Composition composition;
    composition.layers.push_back(
        {.id = 1,
         .name = "Layer 1",
         .effects = {{1, "stripper", {{"speed", 2.0}, {"speedSpread", 0.5}, {"reshuffle", 4.0}, {"audioBand", 7.0}}}}});
    const azl::EffectList effects = azl::effectList(composition.layers[0].effects);
    const azl::PlannedPass pass = azl::planChain(azl::SourceKind::Yuv, {azl::findEffect("stripper")})[0];
    std::vector<float> values;
    azl::ModulationPlan plan(composition);
    CHECK(plan.usesAudio());
    azl::AudioFrame frame;
    frame.values[size_t(azl::AudioFeature::Kick)] = 0.7f;
    auto at = [&](double seconds, double beat) {
        plan.evaluate(seconds, beat, frame);
        azl::paramsOf(pass, effects, values);
        plan.apply(pass, effects, values);
        return values;
    };
    CHECK(at(10.0, 3.99)[15] == doctest::Approx(10.0));
    CHECK(at(10.5, 3.99)[17] == doctest::Approx(0.9975));
    CHECK(at(11.0, 4.0)[17] == 1.0f);
    const std::vector<float> later = at(12.0, 9.5);
    CHECK(later[15] == doctest::Approx(12.0));
    CHECK(later[17] == doctest::Approx(2.375));
    CHECK(later[19] == doctest::Approx(0.7));
}

TEST_CASE("an angle driven through its end wraps round on the circle, and a hand glides the short way")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "stripper", {{"direction", 170.0}}}}});
    const azl::PlannedPass pass = azl::planChain(azl::SourceKind::Yuv, {azl::findEffect("stripper")})[0];
    std::vector<float> values;
    auto direction = [&](const azl::ModulationPlan& plan) {
        const azl::EffectList effects = azl::effectList(composition.layers[0].effects);
        azl::paramsOf(pass, effects, values);
        plan.apply(pass, effects, values);
        return double(values[1]);
    };

    // A macro adds 0..40 degrees to 170: past 180 it continues from -180.
    composition.macros = {{.id = 1, .targets = {{{0, 1, "direction"}, {}, 0.0, 40.0}}}};
    azl::ModulationPlan driven(composition);
    std::vector<double> seen;
    for (int i = 0; i <= 8; ++i) {
        driven.cue({{1, i / 8.0}});
        driven.evaluate(i / kFps, 0.0, {});
        seen.push_back(direction(driven));
    }
    CHECK(seen[0] == doctest::Approx(170.0));
    CHECK(seen[2] == doctest::Approx(180.0));
    CHECK(seen[4] == doctest::Approx(-170.0));
    CHECK(seen[8] == doctest::Approx(-150.0));
    for (size_t i = 1; i < seen.size(); ++i)
        CHECK(std::remainder(seen[i] - seen[i - 1], 360.0) == doctest::Approx(5.0));

    // Dragged at 30 Hz from 160 to -170, it glides through 180, not back through 0.
    composition.macros.clear();
    azl::ModulationPlan before(composition);
    before.evaluate(1.0, 0.0, {});
    composition.layers[0].effects[0].params["direction"] = 160.0;
    azl::ModulationPlan dragged(composition);
    dragged.carry(before);
    dragged.evaluate(1.0 + 1 / kFps, 0.0, {});
    dragged.evaluate(1.0 + 2 / kFps, 0.0, {});
    composition.layers[0].effects[0].params["direction"] = -170.0;
    azl::ModulationPlan after(composition);
    after.carry(dragged);
    after.evaluate(1.0 + 3 / kFps, 0.0, {});
    CHECK(direction(after) == doctest::Approx(175.0));
    after.evaluate(1.0 + 4 / kFps, 0.0, {});
    CHECK(direction(after) == doctest::Approx(-170.0));
}

TEST_CASE("a macro or modulator on a choice drives only its options' values, the option its value picks")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1,
                                  .name = "Layer 1",
                                  .effects = {{1, "classicStripper", {{"mode", 0.842}}}, {2, "classicShiftRgb", {}}}});
    const ParamRef mode{0, 1, "mode"}, shift{0, 2, "mode"}, movement{0, 1, "movementType"};
    composition.macros.push_back({.id = 1, .name = "Mode", .targets = {{mode, {}, 0.0, -0.842}, {shift, {}, 0.0, 5.0}}});
    composition.modulators.push_back(lfo(LfoShape::Saw, 1.0, {0, movement}));
    composition.modulators.back().id = 1;
    const azl::ParamDef& modeDef = *azl::paramDef(composition, mode);
    const azl::ParamDef& shiftDef = *azl::paramDef(composition, shift);
    const azl::ParamDef& movementDef = *azl::paramDef(composition, movement);
    auto valueOf = [](const azl::ModulationPlan& plan, const ParamRef& param) {
        for (const auto& p : plan.params())
            if (p.param == param)
                return p.value;
        FAIL("no driven value for " << param.param);
        return 0.0;
    };
    // The plan publishes single precision.
    auto exactly = [](double value) { return doctest::Approx(value).epsilon(1e-6); };
    std::set<int> modes, movements;
    // Values off the halfway points between options, where single precision may round either way.
    for (int i = 0; i <= 40; ++i) {
        const double v = (i + 0.3) / 41.0;
        composition.macros[0].value = v;
        azl::ModulationPlan plan(composition);
        plan.evaluate(0.0, v, {});
        CAPTURE(i);
        const int m = azl::optionIndex(modeDef, 0.842 * (1.0 - v));
        CHECK(valueOf(plan, mode) == exactly(azl::optionValue(modeDef, m)));
        CHECK(valueOf(plan, shift) == exactly(azl::optionValue(shiftDef, azl::optionIndex(shiftDef, 5.0 * v))));
        const int k = azl::optionIndex(movementDef, valueOf(plan, movement));
        CHECK(valueOf(plan, movement) == exactly(azl::optionValue(movementDef, k)));
        modes.insert(m);
        movements.insert(k);
    }
    CHECK(modes.size() == 6);
    CHECK(movements.size() == 3);
}

namespace {

// A sine at `hz` that a fixed 0 dB gain reads as a full band, continued across calls.
struct Tone {
    double hz;
    double phase = 0.0;

    std::vector<float> next(size_t count)
    {
        std::vector<float> samples(count);
        for (float& s : samples) {
            s = float(0.25 * M_SQRT2 * std::sin(phase));
            phase += 2.0 * M_PI * hz / 48000.0;
        }
        return samples;
    }
};

azl::AudioSettings manualGain()
{
    azl::AudioSettings settings;
    settings.autoGain = false;
    return settings;
}

} // namespace

TEST_CASE("audio bands: a tone in each band drives only that band's modulator; a Gate on a band follows its level")
{
    azl::Composition composition;
    const char* const bands[] = {"lowBand", "midBand", "highBand"};
    for (uint64_t layer = 1; layer <= 3; ++layer) {
        composition.layers.push_back({.id = layer, .name = "Layer", .opacity = 0.0});
        Modulator m = audio(bands[layer - 1], {0, opacity(layer)});
        m.id = layer;
        azl::setEnvelope(m, azl::EnvelopePreset::Meter);
        composition.modulators.push_back(m);
    }
    struct Case {
        double hz;
        uint64_t lit;
    };
    for (const Case c : {Case{50.0, 1}, Case{500.0, 2}, Case{8000.0, 3}}) {
        CAPTURE(c.hz);
        azl::AudioAnalyzer analyzer(48000.0, manualGain());
        azl::ModulationPlan plan(composition);
        Tone tone{c.hz};
        for (int f = 0; f < 60; ++f) {
            const std::vector<float> samples = tone.next(800);
            azl::AudioFrame last;
            analyzer.process(samples.data(), samples.size(), f / kFps, true,
                             [&](const azl::AudioFrame& frame) { last = frame; });
            plan.evaluate(last.time, 0.0, last);
        }
        for (uint64_t layer = 1; layer <= 3; ++layer) {
            CAPTURE(layer);
            if (layer == c.lit)
                CHECK(plan.opacity(layer, 0.0f) > 0.85f);
            else
                CHECK(plan.opacity(layer, 0.0f) < 0.1f);
        }
    }

    Modulator gate = audio("midBand", {0, opacity()});
    gate.id = 1;
    azl::setEnvelope(gate, azl::EnvelopePreset::Gate);
    azl::Composition gated;
    gated.layers.push_back({.id = 1, .name = "Layer 1", .opacity = 0.0});
    gated.modulators = {gate};
    azl::ModulationPlan plan(gated);
    azl::AudioFrame frame;
    plan.evaluate(0.0, 0.0, frame);
    CHECK(plan.modulators()[0].value == 0.0);
    frame.time = 1.0 / kFps;
    frame.values[size_t(azl::AudioFeature::MidBand)] = 0.8f;
    plan.evaluate(frame.time, 0.0, frame);
    CHECK(plan.modulators()[0].value == 1.0);
}

TEST_CASE("audio bands: the engine splits its analysis at the composition's edges, so moving one moves what a band "
          "drives")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .opacity = 0.0});
    Modulator mid = audio("midBand", {0, opacity()});
    mid.id = 1;
    composition.modulators.push_back(mid);
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, composition);
    Client client(harness.document());
    azl::AudioAnalysis analysis([&] { return harness.time(); }, {.thread = false});
    analysis.setSettings(manualGain());
    harness.setAudio(&analysis);
    CHECK(analysis.bandEdges() == azl::kDefaultBandEdges);

    Tone tone{1000.0};
    size_t pushed = 0;
    auto play = [&](double seconds) {
        for (int f = 0; f < int(seconds * kFps); ++f) {
            const auto until = size_t(harness.time() * 48000.0);
            if (until > pushed) {
                const std::vector<float> samples = tone.next(until - pushed);
                analysis.push(samples.data(), samples.size(), double(pushed) / 48000.0, 48000.0);
                pushed = until;
            }
            analysis.pump();
            harness.step();
        }
        return paramValue(state(client), opacity());
    };
    CHECK(play(1.0) > 0.9);
    apply(client, azl::SetBandEdges{{150.0, 500.0}});
    CHECK(analysis.bandEdges() == std::array<double, 2>{150.0, 500.0});
    CHECK(play(1.0) < 0.1);
    harness.setAudio(nullptr);
}

TEST_CASE("audio bands: their edges are an undoable edit kept with the show; old shows load with the defaults")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    CHECK(harness.document().document().bandEdges == azl::kDefaultBandEdges);
    const Message change = apply(client, azl::SetBandEdges{{250.0, 4000.0}});
    CHECK(change["edges"] == Message{250.0, 4000.0});
    for (const std::array<double, 2> bad :
         {std::array{30.0, 2000.0}, std::array{1000.0, 1100.0}, std::array{150.0, 20000.0}}) {
        CAPTURE(bad[0]);
        CHECK(command(client, azl::SetBandEdges{bad})["type"] == "error");
    }
    apply(client, azl::SetSensitivity{"midBand", 2.0});
    apply(client, azl::AddModulator{std::nullopt, audio("highBand", {0, revealEvolve()})});
    CHECK(command(client, azl::AddModulator{std::nullopt, audio("band", {0, revealEvolve()})})["type"] == "error");

    TempDir dir;
    const fs::path path = dir.path / "bands.json";
    azl::saveCompositionFile(path, {harness.document().document()});
    const auto loaded = azl::loadCompositionFile(path);
    REQUIRE(loaded);
    CHECK(loaded->composition.bandEdges == std::array<double, 2>{250.0, 4000.0});
    CHECK(loaded->composition.sensitivity.at("midBand") == 2.0);
    CHECK(loaded->composition.modulators.back().feature == "highBand");

    Message old = revealComposition();
    CHECK_FALSE(old.contains("bandEdges"));
    CHECK(old.get<azl::Composition>().bandEdges == azl::kDefaultBandEdges);
    old["bandEdges"] = {5000.0, 100.0};
    CHECK(old.get<azl::Composition>().bandEdges == azl::kDefaultBandEdges);

    for (int i = 0; i < 3; ++i) {
        client.send(msg::undo());
        client.next();
    }
    CHECK(harness.document().document().bandEdges == azl::kDefaultBandEdges);
}

TEST_CASE("auto range: learns the 10th and 90th percentile of the music, blends in from 0..1, ignores silence")
{
    azl::AutoRange range;
    CHECK(range.range().low == 0.0);
    CHECK(range.range().high == 1.0);
    // Silence and frames without music teach nothing.
    for (int i = 0; i < 600; ++i)
        range.learn(0.0, 1.0 / kFps, false);
    CHECK(range.heard() == 0.0);
    CHECK(range.map(0.5) == 0.5);
    // A feature uniform between 0.02 and 0.2: after a second it has moved part of the way, after a minute it holds
    // close to its 10th and 90th percentiles, 0.038 and 0.182, within the 1 dB bins.
    uint32_t seed = 1;
    auto uniform = [&] {
        seed = seed * 1664525u + 1013904223u;
        return 0.02 + 0.18 * double(seed >> 8) / double(1 << 24);
    };
    for (int i = 0; i < 60; ++i)
        range.learn(uniform(), 1.0 / kFps, true);
    CHECK(range.range().high < 0.9);
    CHECK(range.range().high > 0.5);
    for (int i = 0; i < 60 * 60; ++i)
        range.learn(uniform(), 1.0 / kFps, true);
    CHECK(range.range().low == doctest::Approx(0.038).epsilon(0.15));
    CHECK(range.range().high == doctest::Approx(0.182).epsilon(0.15));
    CHECK(range.map(0.01) == 0.0);
    CHECK(range.map(0.5) == 1.0);
    CHECK(range.map(0.11) == doctest::Approx(0.5).epsilon(0.15));
    // A long silence afterwards changes nothing; a steady feature never collapses the range to nothing.
    for (int i = 0; i < 60 * 60; ++i)
        range.learn(0.0, 1.0 / kFps, false);
    CHECK(range.range().high == doctest::Approx(0.182).epsilon(0.15));
    azl::AutoRange steady;
    for (int i = 0; i < 60 * 60; ++i)
        steady.learn(0.3, 1.0 / kFps, true);
    CHECK(steady.range().high - steady.range().low > 0.2 * steady.range().high);
}

TEST_CASE("auto range: the cost per frame is a bin and a compare, a few nanoseconds")
{
    azl::AutoRange range;
    uint32_t seed = 1;
    std::vector<float> values(1 << 16);
    for (float& v : values) {
        seed = seed * 1664525u + 1013904223u;
        v = float(seed >> 8) / float(1 << 24);
    }
    double sum = 0.0;
    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 1'000'000;
    for (int i = 0; i < kFrames; ++i) {
        range.learn(values[size_t(i) & 0xffff], 1.0 / kFps, true);
        sum += range.map(values[size_t(i * 7) & 0xffff]);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kFrames;
    MESSAGE("auto range: " << ns << " ns per frame (learn and map)");
    CHECK(sum > 0.0);
    CHECK(ns < 500.0);
}

namespace {

// The target's values under `seconds` of the techno loop at `gain` through a manual-gain analysis (so the input level
// reaches the features), after `silence` seconds of nothing, read at 60 fps.
std::vector<double> drivenUnderLoop(const azl::Composition& composition, double gain, double seconds,
                                    double silence = 0.0, const ParamRef& param = {})
{
    TechnoLoop loop;
    loop.gain = gain;
    loop.bars = int(seconds / (4.0 * loop.beat())) + 1;
    const std::vector<float> samples = loop.render();
    azl::AudioAnalyzer analyzer(48000.0, manualGain());
    azl::ModulationPlan plan(composition);
    std::vector<double> values;
    const size_t perFrame = 800;
    const std::vector<float> quiet(perFrame, 0.0f);
    const int silent = int(silence * kFps);
    for (int f = 0; f < silent + int(seconds * kFps); ++f) {
        const float* chunk = f < silent ? quiet.data() : samples.data() + size_t(f - silent) * perFrame;
        if (f >= silent && size_t(f - silent + 1) * perFrame > samples.size())
            break;
        azl::AudioFrame last;
        analyzer.process(chunk, perFrame, f / kFps, true, [&](const azl::AudioFrame& frame) { last = frame; });
        plan.evaluate(last.time, f / kFps * loop.bpm / 60.0, last);
        if (param.param.empty())
            values.push_back(plan.opacity(1, 0.0f));
        else
            for (const auto& p : plan.params())
                if (p.param == param)
                    values.push_back(p.value);
    }
    return values;
}

// The 10th to 90th percentile spread of the values from `from` on.
double swing(std::vector<double> values, size_t from)
{
    values.erase(values.begin(), values.begin() + std::ptrdiff_t(std::min(from, values.size())));
    std::sort(values.begin(), values.end());
    return values[values.size() * 9 / 10] - values[values.size() / 10];
}

} // namespace

TEST_CASE("auto range: the same beat at -20 dB and at 0 dB swings the target the same; without it ten times less")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .opacity = 0.0});
    Modulator bass = audio("bass", {0, opacity()});
    bass.id = 1;
    azl::setEnvelope(bass, azl::EnvelopePreset::Meter);
    composition.modulators = {bass};
    // Half a minute of the loop; the swing over its second half. 0 dB is the loop at half scale, where the manual
    // 0 dB gain reads its bass just below full (at full scale the bass clips at 1 and its shape changes).
    const size_t settled = size_t(15 * kFps);
    const double rawLoud = swing(drivenUnderLoop(composition, 0.5, 30.0), settled);
    const double rawQuiet = swing(drivenUnderLoop(composition, 0.05, 30.0), settled);
    composition.modulators[0].autoRange = true;
    const double loud = swing(drivenUnderLoop(composition, 0.5, 30.0), settled);
    const double quiet = swing(drivenUnderLoop(composition, 0.05, 30.0), settled);
    INFO("raw " << rawLoud << " / " << rawQuiet << ", auto range " << loud << " / " << quiet);
    CHECK(rawQuiet < rawLoud * 0.2);
    CHECK(loud > 5.0 * rawQuiet);
    CHECK(quiet == doctest::Approx(loud).epsilon(0.1));
}

TEST_CASE("auto range: kick to Reveal's progress with Animate's defaults stays within 0.35..0.65, from silence on")
{
    azl::Composition composition = revealComposition();
    const ParamRef progress{0, 1, "progress"};
    composition.layers[0].effects[0].params["progress"] = 0.5;
    const auto defaults = azl::animationRange(composition, progress);
    REQUIRE(defaults);
    CHECK(defaults->first == 0.35);
    CHECK(defaults->second == 0.65);
    // As the UI's Animate makes it: Meter envelope, Auto range, offsets from the value to the default range.
    Modulator kick = audio("kick", {0, progress}, defaults->first - 0.5, defaults->second - 0.5);
    kick.id = 1;
    kick.autoRange = true;
    azl::setEnvelope(kick, azl::EnvelopePreset::Meter);
    composition.modulators = {kick};
    for (double gain : {1.0, 0.1}) {
        CAPTURE(gain);
        // Five seconds of silence, then the music: the first seconds of it included.
        const std::vector<double> values = drivenUnderLoop(composition, gain, 20.0, 5.0, progress);
        REQUIRE(values.size() > size_t(20 * kFps));
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        CHECK(*low >= 0.35 - 1e-6);
        CHECK(*high <= 0.65 + 1e-6);
        // And it moves: once warmed up, each kick takes it near the top of that range (the Meter's release keeps it
        // from falling all the way between kicks at 128 BPM).
        const auto [settledLow, settledHigh] = std::minmax_element(values.begin() + int(10 * kFps), values.end());
        CHECK(*settledHigh > 0.6);
        CHECK(*settledHigh - *settledLow > 0.1);
    }
}

TEST_CASE("auto range: saved with the modulator, off for one saved without it, and reported with the live values")
{
    Modulator m = audio("kick", {0, opacity()});
    m.id = 3;
    CHECK_FALSE(nlohmann::json(m).contains("autoRange"));
    CHECK_FALSE(nlohmann::json(m).get<Modulator>().autoRange);
    m.autoRange = true;
    CHECK(nlohmann::json(m)["autoRange"] == true);
    CHECK(nlohmann::json(m).get<Modulator>().autoRange);

    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    Client client(harness.document());
    const Message added = apply(client, azl::AddModulator{std::nullopt, audio("bass", {0, revealEvolve()})});
    Modulator stored = added["modulator"].get<Modulator>();
    CHECK_FALSE(stored.autoRange);
    harness.step();
    CHECK_FALSE(state(client)["modulators"][0].contains("range"));
    stored.autoRange = true;
    apply(client, azl::SetModulator{stored});
    CHECK(harness.document().document().modulators[0].autoRange);
    harness.step();
    const Message live = state(client)["modulators"][0];
    REQUIRE(live.contains("range"));
    // Nothing heard yet: the range is raw 0..1.
    CHECK(live["range"][0] == 0.0);
    CHECK(live["range"][1] == 1.0);
    client.send(msg::undo());
    client.next();
    CHECK_FALSE(harness.document().document().modulators[0].autoRange);
}

TEST_CASE("auto range: nocturne-ctl makes audio modulators with it on, sets it and shows the range in mod state")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, kFps, {}, revealComposition());
    azl::WebSocketServer server(harness.document(), 0);
    const auto ctl = [&](const std::string& args) {
        const std::string command =
            "'" + ctlPath().string() + "' --port " + std::to_string(server.port()) + " " + args + " 2>&1";
        std::string out;
        FILE* pipe = popen(command.c_str(), "r");
        REQUIRE(pipe);
        char buffer[512];
        while (fgets(buffer, sizeof buffer, pipe))
            out += buffer;
        CHECK(pclose(pipe) == 0);
        return out;
    };
    ctl("mod audio fx.1.evolve bass 0 1");
    REQUIRE(harness.document().document().modulators.size() == 1);
    const uint64_t id = harness.document().document().modulators[0].id;
    CHECK(harness.document().document().modulators[0].autoRange);
    ctl("mod audio fx.1.evolve kick 0 1 autorange=off");
    CHECK_FALSE(harness.document().document().modulators[1].autoRange);
    harness.step();
    CHECK(ctl("mod state").find("\"range\"") != std::string::npos);
    ctl("mod set " + std::to_string(id) + " autorange=off");
    CHECK_FALSE(harness.document().document().modulators[0].autoRange);
    harness.step();
    CHECK(ctl("mod state").find("\"range\"") == std::string::npos);
}
