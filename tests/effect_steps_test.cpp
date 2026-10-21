#include "document/cue_edits.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/effect_stepper.hpp"
#include "engine/modulation_plan.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::Composition;
using azl::Effect;
using azl::EffectStep;
using azl::ParamRef;

constexpr uint64_t kStripper = 7;
// classicStripper's parameters in shader order.
constexpr size_t kMode = 1, kDirection = 2, kWidth = 7, kRed = 11, kGreen = 12, kBlue = 13;

// One layer whose Stripper (effect 7) has three steps: wide red Strips, narrow green Inverted, then wide blue.
Composition steppedComposition(AutopilotMode mode = AutopilotMode::Next, double transition = 4.0)
{
    Effect stripper{kStripper, "classicStripper", {{"globalWidth", 0.5}, {"red", 1.0}}};
    stripper.steps = {{"wide", {{"globalWidth", 1.0}, {"mode", 0.0}, {"red", 1.0}, {"green", 0.0}, {"blue", 0.0}}},
                      {"narrow", {{"globalWidth", 0.2}, {"mode", 2.0 / 6.0}, {"red", 0.0}, {"green", 1.0}, {"blue", 0.0}}},
                      {"blue", {{"globalWidth", 0.8}, {"red", 0.0}, {"green", 0.0}, {"blue", 1.0}}}};
    stripper.autopilot = {mode, 1, 2};
    stripper.transition = transition;
    Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {stripper}});
    return composition;
}

Effect& stripperOf(Composition& composition)
{
    return composition.layers[0].effects[0];
}

struct Rig {
    azl::BeatClock clock;
    azl::CueState cues;
    std::atomic<uint32_t> seed{7};
    azl::EffectStepper stepper{clock, cues, seed};

    std::optional<size_t> stepAt(const Composition& composition, double beat)
    {
        azl::cueStateAt(composition, beat, cues);
        stepper.step(composition, beat);
        for (const azl::StepPosition& p : stepper.positions())
            if (p.effect == kStripper)
                return p.step;
        return std::nullopt;
    }
};

// The Stripper's shader values as the plan draws them at `beat`, after the stepper moved it.
struct Drawn {
    Composition composition;
    Rig rig;
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

    const std::vector<float>& at(double beat)
    {
        azl::cueStateAt(composition, beat, rig.cues);
        plan->cue(rig.cues.macros);
        rig.stepper.step(composition, beat);
        plan->step(rig.stepper.positions());
        plan->evaluate(beat / 2.0, beat, {});
        const azl::EffectList effects = azl::effectList(composition.layers[0].effects);
        azl::paramsOf(pass, effects, values);
        plan->apply(pass, effects, values);
        return values;
    }
};

Message edit(Client& client, const azl::Edit& e)
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

TEST_CASE("effect steps and their Autopilot are undoable edits, checked on the way in")
{
    Composition composition = steppedComposition();
    const Effect before = stripperOf(composition);

    azl::Edit setSteps = azl::SetEffectSteps{kStripper, {{"one", {{"globalWidth", 0.3}}}}};
    azl::Edit undoSteps = azl::applyEdit(composition, setSteps);
    CHECK(stripperOf(composition).steps.size() == 1);
    azl::applyEdit(composition, undoSteps);
    CHECK(stripperOf(composition) == before);

    azl::Edit pilot = azl::SetEffectAutopilot{kStripper, AutopilotMode::Random, 8, std::nullopt, 2.0};
    azl::Edit undoPilot = azl::applyEdit(composition, pilot);
    CHECK(stripperOf(composition).autopilot == azl::Autopilot{AutopilotMode::Random, 8, 2});
    CHECK(stripperOf(composition).transition == 2.0);
    azl::applyEdit(composition, undoPilot);
    CHECK(stripperOf(composition) == before);

    azl::Edit unknown = azl::SetEffectSteps{kStripper, {{"bad", {{"nope", 0.5}}}}};
    CHECK_THROWS_AS(azl::applyEdit(composition, unknown), azl::ProtocolError);
    azl::Edit outOfRange = azl::SetEffectSteps{kStripper, {{"bad", {{"globalWidth", 2.0}}}}};
    CHECK_THROWS_AS(azl::applyEdit(composition, outOfRange), azl::ProtocolError);
    azl::Edit bars = azl::SetEffectAutopilot{kStripper, std::nullopt, 0};
    CHECK_THROWS_AS(azl::applyEdit(composition, bars), azl::ProtocolError);
    azl::Edit transition = azl::SetEffectAutopilot{kStripper, std::nullopt, std::nullopt, std::nullopt, 65.0};
    CHECK_THROWS_AS(azl::applyEdit(composition, transition), azl::ProtocolError);
    CHECK(stripperOf(composition) == before);

    // A trigger is live: it lands on the composition's quantisation and has no undo.
    azl::Edit trigger = azl::TriggerStep{kStripper, 2, 5.0};
    CHECK(azl::isLive(trigger));
    azl::applyEdit(composition, trigger);
    CHECK(stripperOf(composition).playing == azl::StepPlay{2, 8.0});
    azl::Edit missing = azl::TriggerStep{kStripper, 3, 5.0};
    CHECK_THROWS_AS(azl::applyEdit(composition, missing), azl::ProtocolError);
    azl::Edit batch = azl::Batch{{azl::TriggerStep{kStripper, 0, 0.0}}};
    CHECK_THROWS_AS(azl::applyEdit(composition, batch), azl::ProtocolError);

    for (const azl::Edit& e : {setSteps, pilot, trigger})
        CHECK(azl::editToJson(azl::editFromJson(azl::editToJson(e))) == azl::editToJson(e));
}

TEST_CASE("effect steps save and load with the show; an effect without them saves as before")
{
    Composition composition = steppedComposition(AutopilotMode::Random, 2.5);
    stripperOf(composition).playing = azl::StepPlay{1, 12.0};
    const nlohmann::json saved = composition;
    CHECK(saved.get<Composition>().layers[0].effects[0] == stripperOf(composition));

    const Effect plain{3, "colorize", {{"hue", 0.5}}};
    const nlohmann::json plainJson = plain;
    CHECK(plainJson == nlohmann::json{{"id", 3}, {"type", "colorize"}, {"params", {{"hue", 0.5}}}});
    CHECK(plainJson.get<Effect>() == plain);

    // Added through an edit, an effect keeps its steps and Autopilot.
    Composition other;
    other.layers.push_back({.id = 1, .name = "Layer 1"});
    azl::Edit add = azl::editFromJson(azl::editToJson(azl::AddEffect{1, std::nullopt, stripperOf(composition)}));
    azl::applyEdit(other, add);
    CHECK(other.layers[0].effects[0].steps == stripperOf(composition).steps);
    CHECK(other.layers[0].effects[0].autopilot == stripperOf(composition).autopilot);
}

TEST_CASE("an effect Autopilot steps on the bar every N bars, paced, and random never repeats a step")
{
    Rig rig;
    Composition composition = steppedComposition();
    CHECK(rig.stepAt(composition, 0.5) == std::nullopt);
    CHECK(rig.stepAt(composition, 3.9) == std::nullopt);
    CHECK(rig.stepAt(composition, 4.0) == 0);
    CHECK(rig.stepAt(composition, 9.0) == 1);
    CHECK(rig.stepAt(composition, 12.0) == 2);
    CHECK(rig.stepAt(composition, 16.0) == 0);
    for (const azl::StepPosition& p : rig.stepper.positions())
        CHECK(p.beat == 16.0);

    // At twice the pace a step lasts two bars; the running one is rescaled from its progress.
    composition.pace = 2.0;
    CHECK(rig.stepAt(composition, 20.0) == 1);
    CHECK(rig.stepAt(composition, 24.0) == 1);
    CHECK(rig.stepAt(composition, 28.0) == 2);

    stripperOf(composition).autopilot.mode = AutopilotMode::Off;
    CHECK(rig.stepAt(composition, 64.0) == 2);

    Rig random;
    Composition shuffled = steppedComposition(AutopilotMode::Random);
    std::vector<size_t> steps;
    random.stepAt(shuffled, 0.0);
    for (int bar = 1; bar <= 32; ++bar)
        steps.push_back(*random.stepAt(shuffled, bar * 4.0));
    std::set<size_t> seen(steps.begin(), steps.end());
    CHECK(seen.size() == 3);
    for (size_t i = 1; i < steps.size(); ++i)
        CHECK(steps[i] != steps[i - 1]);
}

TEST_CASE("a step trigger moves the effect at its beat and holds it for the hold before the Autopilot moves on")
{
    Rig rig;
    Composition composition = steppedComposition();
    rig.stepAt(composition, 0.0);
    CHECK(rig.stepAt(composition, 4.0) == 0);
    stripperOf(composition).playing = azl::StepPlay{2, 6.0};
    CHECK(rig.stepAt(composition, 5.0) == 2);
    for (const azl::StepPosition& p : rig.stepper.positions())
        CHECK(p.beat == 6.0);
    // The hold is two bars from the bar line the trigger lands on.
    CHECK(rig.stepAt(composition, 15.9) == 2);
    CHECK(rig.stepAt(composition, 16.0) == 0);
}

TEST_CASE("Cues set an effect's Autopilot and move it to a step; per-deck chains keep their own steps")
{
    Rig rig;
    Composition composition = steppedComposition(AutopilotMode::Off);
    composition.cues.push_back({.id = 1, .bar = 3, .effects = {{kStripper, AutopilotMode::Next, 1, size_t(1)}}});
    CHECK(rig.stepAt(composition, 4.0) == std::nullopt);
    CHECK(rig.stepAt(composition, 8.0) == 1);
    CHECK(rig.stepAt(composition, 12.0) == 2);

    Composition invalid = steppedComposition();
    azl::Edit noEffect = azl::AddCue{std::nullopt, {.bar = 1, .effects = {{99, AutopilotMode::Next}}}};
    CHECK_THROWS_AS(azl::applyEdit(invalid, noEffect), azl::ProtocolError);
    azl::Edit empty = azl::AddCue{std::nullopt, {.bar = 1, .effects = {{kStripper}}}};
    CHECK_THROWS_AS(azl::applyEdit(invalid, empty), azl::ProtocolError);
    const azl::Cue cue = composition.cues[0];
    CHECK(nlohmann::json(cue).get<azl::Cue>() == cue);
    CHECK_FALSE(nlohmann::json(azl::Cue{.id = 2}).contains("effects"));

    // A deck's own chain is another chain of effects with their own steps and pilots.
    Rig decks;
    Composition deck = steppedComposition(AutopilotMode::Next);
    Effect own = stripperOf(deck);
    own.id = 8;
    own.autopilot.bars = 2;
    deck.layers[0].decks[1].effects = std::vector<Effect>{own};
    auto stepsAt = [&](double beat) {
        decks.stepAt(deck, beat);
        std::map<uint64_t, size_t> at;
        for (const azl::StepPosition& p : decks.stepper.positions())
            at[p.effect] = p.step;
        return at;
    };
    stepsAt(0.0);
    CHECK(stepsAt(4.0) == std::map<uint64_t, size_t>{{kStripper, 0}});
    CHECK(stepsAt(8.0) == std::map<uint64_t, size_t>{{kStripper, 1}, {8, 0}});
}

TEST_CASE("a move to a step glides numbers and colours over the transition and switches choices halfway")
{
    // Two-bar steps: step 0 lands on beat 8 from the document's values, step 1 on beat 16, each gliding four beats.
    Composition composition = steppedComposition(AutopilotMode::Next, 4.0);
    stripperOf(composition).autopilot.bars = 2;
    Drawn drawn(composition);
    std::vector<float> previous = drawn.at(0.0);
    CHECK(previous[kWidth] == doctest::Approx(0.5));
    double maxWidth = 0.0, maxColour = 0.0;
    auto colourOf = [](const std::vector<float>& v) { return azl::Rgba{v[kRed], v[kGreen], v[kBlue]}; };
    for (int frame = 1; frame <= 24 * 60; ++frame) {
        const double beat = frame / 60.0;
        const std::vector<float> now = drawn.at(beat);
        CAPTURE(beat);
        maxWidth = std::max(maxWidth, double(std::abs(now[kWidth] - previous[kWidth])));
        const auto a = azl::toOklab(colourOf(previous)), b = azl::toOklab(colourOf(now));
        maxColour = std::max(maxColour, std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
        if (beat < 18.0 - 1e-9)
            CHECK(now[kMode] == doctest::Approx(0.0));
        else if (beat > 18.0 + 1e-9)
            CHECK(now[kMode] == doctest::Approx(2.0 / 6.0));
        if (beat >= 20.0)
            CHECK(now[kWidth] == doctest::Approx(0.2));
        previous = now;
    }
    // 0.5 to 1, then 1 to 0.2, each over four beats: never more than the transition's share in a frame.
    CHECK(maxWidth <= 0.8 / (4.0 * 60.0) + 1e-6);
    // Red to green moves evenly in OKLab.
    const auto red = azl::toOklab({1.0, 0.0, 0.0}), green = azl::toOklab({0.0, 1.0, 0.0});
    const double span = std::hypot(green[0] - red[0], green[1] - red[1], green[2] - red[2]);
    CHECK(maxColour <= span / (4.0 * 60.0) * 1.05);
    CHECK(previous[kGreen] == doctest::Approx(1.0));
    CHECK(previous[kRed] == doctest::Approx(0.0));

    // Halfway from red to green the colour is mixed in OKLab: no dark midpoint as in straight RGB.
    Drawn colour(composition);
    colour.at(0.0);
    colour.at(8.0);
    colour.at(16.0);
    const std::vector<float> mid = colour.at(18.0);
    CHECK(mid[kRed] > 0.7f);
    CHECK(mid[kGreen] > 0.6f);
}

TEST_CASE("a live edit during a glide takes over from where the glide is; the next step moves on from there")
{
    Drawn drawn(steppedComposition(AutopilotMode::Next, 4.0));
    for (int frame = 0; frame < 10 * 60; ++frame)
        drawn.at(frame / 60.0);
    // From where the last frame before beat 8 drew it, almost 1, halfway to 0.2.
    const float halfway = drawn.at(10.0)[kWidth];
    CHECK(halfway == doctest::Approx(0.599).epsilon(0.001));

    // The UI shows the stepped value, so a drag starts from it.
    double shown = 0.0;
    for (const auto& p : drawn.plan->params())
        if (p.param == ParamRef{0, kStripper, "globalWidth"})
            shown = p.value;
    CHECK(shown == doctest::Approx(halfway));

    stripperOf(drawn.composition).params["globalWidth"] = 0.62;
    drawn.rebuild();
    float previous = halfway;
    for (int frame = 1; frame <= 30; ++frame) {
        const float now = drawn.at(10.0 + frame / 60.0)[kWidth];
        CHECK(std::abs(now - previous) < 0.03f);
        previous = now;
    }
    CHECK(previous == doctest::Approx(0.62));
    // The step to 0.8 starts from the edited value on the next bar.
    CHECK(drawn.at(12.0)[kWidth] == doctest::Approx(0.62));
    CHECK(drawn.at(14.0)[kWidth] == doctest::Approx(0.71));
}

TEST_CASE("macros and modulators drive on top of a stepped value")
{
    Composition composition = steppedComposition(AutopilotMode::Next, 0.0);
    composition.macros = {{.id = 1, .value = 0.5, .targets = {{{0, kStripper, "globalWidth"}, {}, 0.0, -0.4}}}};
    Drawn drawn(composition);
    CHECK(drawn.at(0.0)[kWidth] == doctest::Approx(0.3));
    CHECK(drawn.at(4.0)[kWidth] == doctest::Approx(0.8));
    CHECK(drawn.at(8.0)[kWidth] == doctest::Approx(0.0));
    CHECK(drawn.at(12.0)[kWidth] == doctest::Approx(0.6));
}

TEST_CASE("a stepped parameter turns on the shader variant it needs; an effect without steps keeps its shader")
{
    Composition composition = steppedComposition();
    stripperOf(composition).steps = {{"zoom", {{"mode", 3.0 / 6.0}}}};
    CHECK(azl::shaderType(composition, stripperOf(composition)) == "classicStripper_resample");
    stripperOf(composition).steps.clear();
    CHECK(azl::shaderType(composition, stripperOf(composition)) == "classicStripper");
}

TEST_CASE("effect steps over the protocol: the clock shows the step, a step trigger lands quantised, nothing jumps")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0);
    Client client(harness.document());
    const Message added = edit(client, azl::AddEffect{0, std::nullopt, Effect{0, "classicStripper", {{"globalWidth", 0.5}}}});
    REQUIRE(added["op"] == "addEffect");
    const uint64_t id = added["effect"]["id"].get<uint64_t>();
    REQUIRE(edit(client, azl::SetEffectSteps{id, {{"a", {{"globalWidth", 1.0}}}, {"b", {{"globalWidth", 0.0}}}}})["op"] ==
            "setEffectSteps");
    REQUIRE(edit(client, azl::SetEffectAutopilot{id, AutopilotMode::Next, 1, std::nullopt, 2.0})["op"] ==
            "setEffectAutopilot");

    auto width = [&] {
        client.send({{"type", "modulation"}, {"op", "state"}});
        const Message state = client.next();
        for (const Message& p : state["params"])
            if (p["effect"] == id && p["param"] == "globalWidth")
                return p["value"].get<double>();
        FAIL("no stepped value in " << state.dump());
        return 0.0;
    };
    auto shown = [&] {
        client.send({{"type", "clock"}, {"op", "state"}});
        const Message state = client.next();
        REQUIRE(state["effects"].size() == 1);
        return state["effects"][0];
    };
    harness.step(30);
    CHECK(shown()["step"].is_null());
    CHECK(shown()["next"] == 0);
    double previous = width();
    for (int frame = 0; frame < 300; ++frame) {
        harness.step();
        const double now = width();
        REQUIRE(std::abs(now - previous) < 1.0 / 60.0 + 1e-4);
        previous = now;
    }
    CHECK(shown()["step"] == 1);

    client.send({{"type", "trigger"}, {"op", "step"}, {"effect", id}, {"step", 0}});
    REQUIRE(client.next()["op"] == "triggerStep");
    harness.step(150);
    CHECK(shown()["step"] == 0);
    CHECK(shown()["held"].get<int>() > 0);
}

TEST_CASE("nocturne-ctl adds, lists, moves and removes effect steps, sets the effect Autopilot and moves to a step")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    Client client(harness.document());
    const Message added = edit(client, azl::AddEffect{0, std::nullopt, Effect{0, "classicStripper", {{"globalWidth", 0.5}}}});
    const std::string id = std::to_string(added["effect"]["id"].get<uint64_t>());
    auto effect = [&] { return harness.document().document().effects[0]; };

    ctl(port, "fxsteps " + id + " add wide");
    ctl(port, "param " + id + " globalWidth 0.2");
    ctl(port, "fxsteps " + id + " add");
    REQUIRE(effect().steps.size() == 2);
    CHECK(effect().steps[0].name == "wide");
    CHECK(effect().steps[0].params.at("globalWidth") == 0.5);
    CHECK(effect().steps[1].name == "Step 2");
    CHECK(effect().steps[1].params.at("globalWidth") == 0.2);
    ctl(port, "fxsteps " + id + " move 2 1");
    CHECK(effect().steps[0].name == "Step 2");
    CHECK(ctl(port, "fxsteps " + id).find("1  Step 2") != std::string::npos);

    ctl(port, "fxpilot " + id + " random 8");
    ctl(port, "fxhold " + id + " 2");
    ctl(port, "fxtransition " + id + " 0.5");
    CHECK(effect().autopilot == azl::Autopilot{AutopilotMode::Random, 8, 2});
    CHECK(effect().transition == 0.5);
    ctl(port, "fxpilot " + id + " off");
    CHECK(effect().autopilot.mode == AutopilotMode::Off);

    ctl(port, "fxstep " + id + " 2");
    REQUIRE(effect().playing);
    CHECK(effect().playing->step == 1);

    ctl(port, "cue add drop drop effect:" + id + "=next/4 step:" + id + "=1");
    REQUIRE(harness.document().document().cues.size() == 1);
    CHECK(harness.document().document().cues[0].effects ==
          std::vector<azl::CueEffect>{{effect().id, AutopilotMode::Next, 4, size_t(0)}});

    ctl(port, "fxsteps " + id + " rm 1");
    CHECK(effect().steps.size() == 1);
}
