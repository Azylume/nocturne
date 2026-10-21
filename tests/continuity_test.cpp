#include "clips.hpp"
#include "engine/modulation_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The continuity harness: every continuous control is ramped across its range over a second at 60 fps, and a frame
// whose change from the last one stands out from the ramp is a jump. By default it sweeps a fast subset;
// NOCTURNE_CONTINUITY_FULL=1 sweeps everything and prints each sweep.
namespace {

constexpr int kRampFrames = 60;
constexpr int kHeldFrames = 15;
constexpr azl::EngineConfig kSmall{.width = 320, .height = 180};
constexpr azl::EngineConfig kClipSize{.width = 960, .height = 540};

bool fullSweep()
{
    return std::getenv("NOCTURNE_CONTINUITY_FULL") != nullptr;
}

// Steps that belong to the control, not to a fault.
const std::map<std::string, const char*> kInherent = {
    {"stripper.movement", "a choice of movement"},
    {"stripper.audioBand", "a choice of audio feature"},
    {"reveal.gap", "a hard-edged border grows a pixel line at a time"},
    {"reveal.seed", "each seed is its own order; evolve moves between them"},
    {"classicStripper.number", "a whole number of strips"},
    {"classicStripper.mode", "a choice of mode"},
    {"classicStripper.movementType", "a choice of movement"},
    {"classicStripper.blendMode", "a choice of mixer"},
    {"classicColorize.mode", "a choice of mode"},
    {"classicColorize.weighted", "a switch"},
    {"classicColorize.greysKeep", "a switch"},
    {"classicColorize.blendMode", "a choice of mixer"},
    {"classicShiftRgb.mode", "a choice of mode"},
    {"classicShiftRgb.blackBG", "a switch"},
    {"classicShiftRgb.blendMode", "a choice of mixer"},
    {"classicMirror.flipX", "a switch"},
    {"classicMirror.flipY", "a switch"},
    {"classicMirror.blackBG", "a switch"},
    {"classicMirror.blendMode", "a choice of mixer"},
    {"classicKaleidoscope.angles", "a whole number of wedges"},
    {"classicKaleidoscope.blackBG", "a switch"},
    {"classicKaleidoscope.blendMode", "a choice of mixer"},
    {"classicTwitch.blackBG", "a switch"},
    {"classicTwitch.blendMode", "a choice of mixer"},
    {"classicAutoMask.channel", "a choice of channel"},
    {"classicAutoMask.invert", "a switch"},
    {"classicBrightnessContrast.blendMode", "a choice of mixer"},
    {"classicLevels.blendMode", "a choice of mixer"},
    {"classicThreshold.threshold", "a hard cut while Smoothness is 0"},
    {"classicThreshold.smoothness", "from 0 the step softens at once for a grey that sits on the threshold"},
    {"classicThreshold.alpha", "a choice of alpha"},
    {"classicThreshold.blendMode", "a choice of mixer"},
    {"classicHueRotate.blendMode", "a choice of mixer"},
    {"classicMirrorQuad.flipX", "a switch"},
    {"classicMirrorQuad.flipY", "a switch"},
    {"classicMirrorQuad.blendMode", "a choice of mixer"},
    {"classicPolarKaleido.rings", "a whole number of rings"},
    {"classicPolarKaleido.parts", "a whole number of parts"},
    {"classicPolarKaleido.blendMode", "a choice of mixer"},
    {"classicStrobe.frequency", "the flash re-locks to the beat grid of its new period, and 0 never flashes"},
    {"classicStrobe.fade", "a switch between a flash and a ramp"},
    {"classicStrobe.syncToBeat", "a switch between the beat clock and seconds"},
    {"classicShiftGlitch.syncToBeat", "a switch between the beat clock and seconds"},
    {"classicEdgeDetection.mode", "a choice of mode"},
    {"classicEdgeDetection.useColor", "a switch"},
    {"classicEdgeDetection.blendMode", "a choice of mixer"},
    {"strobe.rate", "the strobe re-locks to the beat grid of its new period"},
    {"strobe.duty", "the strobe is on or off"},
};

// A sweep jumps where one frame changes by more than three times as much as a frame next to it, plus the change between
// frames with the control held at either end of its range and `floor`. A control whose effect varies across its
// range changes smoothly from frame to frame; a jump stands out from its neighbours.
struct Sweep {
    std::string name;
    double floor = 1.0;
    double baseline = 0.0;
    std::vector<double> steps;

    double around(size_t i) const
    {
        return std::max(i > 0 ? steps[i - 1] : 0.0, i + 1 < steps.size() ? steps[i + 1] : 0.0);
    }
    double excess(size_t i) const { return steps[i] - 3.0 * around(i) - baseline - floor; }
    size_t worst() const
    {
        size_t w = 0;
        for (size_t i = 1; i < steps.size(); ++i)
            if (excess(i) > excess(w))
                w = i;
        return w;
    }
    bool jumps() const { return excess(worst()) > 0.0; }
};

void report(const std::vector<Sweep>& sweeps)
{
    for (const Sweep& s : sweeps) {
        const bool inherent = kInherent.contains(s.name.substr(0, s.name.find(' '))) || kInherent.contains(s.name);
        if (fullSweep() || s.jumps())
            std::printf("continuity %-40s held %7.3f worst %7.3f at %3zu next to %7.3f%s\n", s.name.c_str(),
                        s.baseline, s.steps[s.worst()], s.worst(), s.around(s.worst()),
                        s.jumps() ? (inherent ? "  inherent" : "  JUMP") : "");
        if (s.jumps())
            CHECK_MESSAGE(inherent, s.name << " jumps by " << s.steps[s.worst()] << " at ramp frame "
                                                             << s.worst() << " next to " << s.around(s.worst()));
    }
}

// Mean absolute difference of the pictures' RGB, in 8-bit levels.
double difference(const azl::Image& a, const azl::Image& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        sum += std::abs(a.rgba[i] - b.rgba[i]) + std::abs(a.rgba[i + 1] - b.rgba[i + 1]) +
               std::abs(a.rgba[i + 2] - b.rgba[i + 2]);
    return sum / double(a.rgba.size() / 4 * 3);
}

// Renders a frame per `measure` call: `set(t)` puts the control at t (0..1 of its range) and `measure` returns the
// change from the last frame. The baseline holds it at each end; the ramp moves it every `every` frames, and then the
// first value of the drag, which lands at once, is not judged.
Sweep sweep(std::string name, double floor, int every, const std::function<void(double)>& set,
            const std::function<double()>& measure)
{
    Sweep s{std::move(name), floor, 0.0, {}};
    for (const double end : {1.0, 0.0}) {
        set(end);
        measure();
        for (int i = 0; i < kHeldFrames; ++i)
            s.baseline = std::max(s.baseline, measure());
    }
    for (int i = 1; i <= kRampFrames; ++i) {
        if (i % every == 0)
            set(double(i) / kRampFrames);
        const double step = measure();
        if (every == 1 || i > every)
            s.steps.push_back(step);
    }
    return s;
}

// Each parameter of an effect on the test card (a generator as its clip's source) ramped from its minimum to its
// maximum, the others at their defaults or as `held` sets them (named `label`), as a UI drag that sends a value every
// `every` frames.
std::vector<Sweep> sweepEffect(const std::string& type, const std::vector<std::string>& only = {}, int every = 1,
                               const std::map<std::string, double>& held = {}, const std::string& label = {})
{
    const azl::EffectDef* def = azl::findEffect(type);
    REQUIRE(def);
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    uint64_t effect = 0;
    if (def->generator) {
        apply(client, azl::SetQuantise{0});
        const Message set = apply(client, azl::SetSlot{1, 1, azl::ClipRef{.name = type, .effects = {{0, type, {}}}}});
        effect = set["clip"]["effects"][0]["id"];
        client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 1}});
        REQUIRE(client.next()["op"] == "triggerClip");
    } else {
        effect = apply(client, azl::AddEffect{1, std::nullopt, {0, type, {}}})["effect"]["id"];
    }
    std::map<std::string, double> defaults;
    for (const azl::ParamDef& p : def->params)
        defaults[p.name] = held.contains(p.name) ? held.at(p.name) : p.defaultValue;
    harness.step();
    azl::Image last = harness.readScreen();
    auto measure = [&] {
        harness.step();
        azl::Image image = harness.readScreen();
        const double d = difference(image, last);
        last = std::move(image);
        return d;
    };
    std::vector<Sweep> sweeps;
    for (const azl::ParamDef& p : def->params) {
        if ((!only.empty() && std::find(only.begin(), only.end(), p.name) == only.end()) || held.contains(p.name))
            continue;
        apply(client, azl::SetEffectParams{effect, defaults});
        auto set = [&](double t) { apply(client, azl::SetEffectParam{effect, p.name, p.min + (p.max - p.min) * t}); };
        sweeps.push_back(sweep(type + "." + p.name + label + (every > 1 ? " at 30 Hz" : ""), 1.0, every, set, measure));
    }
    return sweeps;
}

// Each control of a beat LFO, an audio modulator and a macro driving layer 1's opacity, ten minutes into a show at
// 120 BPM, with a kick that swells twice a second. Every value is an edit, so every frame rebuilds the plan as the
// engine does.
std::vector<Sweep> sweepModulation()
{
    azl::Composition lfo;
    lfo.layers.push_back({.id = 1, .name = "Layer 1", .opacity = 0.0});
    azl::Composition audio = lfo;
    azl::Composition macro = lfo;
    const azl::ParamRef opacity{1, 0, "opacity"};
    lfo.modulators.push_back({.id = 1, .beats = 2.0, .target = {0, opacity}});
    audio.modulators.push_back({.id = 1, .source = azl::ModulatorSource::Audio, .attack = 50.0, .release = 200.0,
                                .target = {0, opacity}});
    macro.macros.push_back({.id = 1, .value = 0.5, .targets = {{opacity, {}, 0.0, 1.0}}});

    struct Control {
        const char* name;
        const azl::Composition& composition;
        std::function<void(azl::Composition&, double)> set;
    };
    const std::vector<Control> controls = {
        {"lfo.beats", lfo, [](azl::Composition& c, double t) { c.modulators[0].beats = 1.0 + 15.0 * t; }},
        {"lfo.phase", lfo, [](azl::Composition& c, double t) { c.modulators[0].phase = t; }},
        {"lfo.min", lfo, [](azl::Composition& c, double t) { c.modulators[0].min = t; }},
        {"lfo.max", lfo, [](azl::Composition& c, double t) { c.modulators[0].max = t; }},
        {"audio.attack", audio, [](azl::Composition& c, double t) { c.modulators[0].attack = 1000.0 * t; }},
        {"audio.release", audio, [](azl::Composition& c, double t) { c.modulators[0].release = 1000.0 * t; }},
        {"audio.sensitivity", audio, [](azl::Composition& c, double t) { c.sensitivity["kick"] = 4.0 * t; }},
        {"audio.min", audio, [](azl::Composition& c, double t) { c.modulators[0].min = t; }},
        {"audio.max", audio, [](azl::Composition& c, double t) { c.modulators[0].max = t; }},
        {"macro.value", macro, [](azl::Composition& c, double t) { c.macros[0].value = t; }},
        {"macro.min", macro, [](azl::Composition& c, double t) { c.macros[0].targets[0].min = t; }},
        {"macro.max", macro, [](azl::Composition& c, double t) { c.macros[0].targets[0].max = t; }},
    };
    std::vector<Sweep> sweeps;
    for (const Control& control : controls) {
        azl::Composition composition = control.composition;
        std::unique_ptr<azl::ModulationPlan> plan;
        int frame = 0;
        double last = 0.0;
        auto set = [&](double t) {
            control.set(composition, t);
            auto next = std::make_unique<azl::ModulationPlan>(composition);
            if (plan)
                next->carry(*plan);
            plan = std::move(next);
        };
        auto measure = [&] {
            const double seconds = 600.0 + frame++ / 60.0;
            azl::AudioFrame frame;
            frame.time = seconds;
            frame.values[size_t(azl::AudioFeature::Kick)] = float(0.5 + 0.5 * std::sin(seconds * 4.0 * M_PI));
            plan->evaluate(seconds, seconds * 2.0, frame);
            const double value = plan->opacity(1, 1.0f);
            return std::abs(value - std::exchange(last, value));
        };
        sweeps.push_back(sweep(control.name, 0.02, 1, set, measure));
    }
    return sweeps;
}

// The Low/Mid edge under a midBand modulator on layer 1's opacity, ramped from 150 Hz to 1.5 kHz across a steady
// 400 Hz tone that it hands from Mid to Low. Each value is an edit that rebuilds the plan and moves the analyser's edge.
Sweep sweepBandEdge()
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .opacity = 0.0});
    composition.modulators.push_back(
        {.id = 1, .source = azl::ModulatorSource::Audio, .feature = "midBand", .target = {0, {1, 0, "opacity"}}});
    azl::AudioSettings manual;
    manual.autoGain = false;
    azl::AudioAnalyzer analyzer(48000.0, manual);
    std::unique_ptr<azl::ModulationPlan> plan;
    std::vector<float> samples(800);
    double phase = 0.0;
    int frame = 0;
    double last = 0.0;
    auto set = [&](double t) {
        composition.bandEdges = {150.0 * std::pow(10.0, t), 3000.0};
        analyzer.setBandEdges(composition.bandEdges);
        auto next = std::make_unique<azl::ModulationPlan>(composition);
        if (plan)
            next->carry(*plan);
        plan = std::move(next);
    };
    auto measure = [&] {
        for (float& s : samples) {
            s = float(0.25 * M_SQRT2 * std::sin(phase));
            phase += 2.0 * M_PI * 400.0 / 48000.0;
        }
        azl::AudioFrame latest;
        analyzer.process(samples.data(), samples.size(), frame++ / 60.0, true,
                         [&](const azl::AudioFrame& f) { latest = f; });
        plan->evaluate(latest.time, 0.0, latest);
        const double value = plan->opacity(1, 0.0f);
        return std::abs(value - std::exchange(last, value));
    };
    return sweep("audio.bandEdge", 0.02, 1, set, measure);
}

// Each continuous transport control of a looping count_240 clip (16 beats at 120 BPM: half a clip frame per render
// frame) ramped across its range, judged by how many clip frames the picture moves per render frame; wrapping from the
// end of the loop to its start counts as moving on.
std::vector<Sweep> sweepTransport(const std::vector<std::string>& only = {})
{
    struct Control {
        const char* name;
        std::function<void(azl::SetClipTransport&, double)> set;
    };
    const std::vector<Control> controls = {
        {"transport.speed", [](azl::SetClipTransport& e, double t) { e.speed = 4.0 * t; }},
        {"transport.in", [](azl::SetClipTransport& e, double t) { e.in = 0.9 * t; }},
        {"transport.out", [](azl::SetClipTransport& e, double t) { e.out = 0.1 + 0.9 * t; }},
    };
    std::vector<Sweep> sweeps;
    for (const Control& control : controls) {
        if (!only.empty() && std::find(only.begin(), only.end(), control.name) == only.end())
            continue;
        TempDir media;
        azl::EngineHarness harness(kClipSize);
        Client client(harness.document());
        playMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
        harness.step();
        azl::Transport transport;
        int last = shownFrame(harness.readScreen());
        auto set = [&](double t) {
            azl::SetClipTransport edit{.layer = 1};
            control.set(edit, t);
            apply(client, edit);
            transport.in = edit.in.value_or(transport.in);
            transport.out = edit.out.value_or(transport.out);
        };
        auto measure = [&] {
            harness.step();
            const int frame = shownFrame(harness.readScreen());
            const int first = int(transport.in * 240), end = int(std::ceil(transport.out * 240));
            const int wrapped = frame < last ? end - last + frame - first : 240;
            return double(std::min(std::abs(frame - std::exchange(last, frame)), wrapped));
        };
        sweeps.push_back(sweep(control.name, 1.0, 1, set, measure));
    }
    return sweeps;
}

} // namespace

TEST_CASE("continuity: every transport control ramped over a second moves the clip on without a jump")
{
    report(fullSweep() ? sweepTransport() : sweepTransport({"transport.speed"}));
}

TEST_CASE("continuity: every transition mixes from one clip to the other without a jump or a flash")
{
    // Two still clips, count_240 held at its frame 120 and count_709_tv at its first, and a transition of two beats:
    // one second at 120 BPM. The steps run from the trigger until well after the transition ends.
    const char* names[] = {"alpha", "wipe", "dissolve", "push"};
    std::vector<Sweep> sweeps;
    for (const azl::TransitionMode mode :
         {azl::TransitionMode::Alpha, azl::TransitionMode::Wipe, azl::TransitionMode::Dissolve,
          azl::TransitionMode::Push}) {
        TempDir media;
        azl::EngineHarness harness(kSmall);
        Client client(harness.document());
        azl::Transport still;
        still.speed = 0.0;
        still.in = 0.5;
        const std::string a = addMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"))["entry"]["hash"];
        const std::string b = addMedia(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4"))["entry"]["hash"];
        apply(client, azl::SetSlot{1, 1, azl::ClipRef{a, "a", still}});
        still.in = 0.0;
        apply(client, azl::SetSlot{1, 2, azl::ClipRef{b, "b", still}});
        apply(client, azl::SetLayerTransition{1, mode, 2.0});
        auto trigger = [&](uint64_t column) {
            client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
            REQUIRE(client.next()["op"] == "triggerColumn");
        };
        trigger(1);
        harness.step(130);
        azl::Image last = harness.readScreen();
        auto measure = [&] {
            harness.step();
            azl::Image image = harness.readScreen();
            const double d = difference(image, last);
            last = std::move(image);
            return d;
        };
        Sweep s{std::string("transition.") + names[int(mode)], 1.0, 0.0, {}};
        for (int i = 0; i < kHeldFrames; ++i)
            s.baseline = std::max(s.baseline, measure());
        trigger(2);
        for (int i = 0; i < 240; ++i)
            s.steps.push_back(measure());
        sweeps.push_back(s);
    }
    report(sweeps);
}

TEST_CASE("continuity: Trails history survives adding, moving, bypassing and removing another effect and plan rebuilds")
{
    // The test card moves away and leaves its white left edge at x = 0 as history, held at feedback 1.
    azl::EngineHarness harness(kClipSize);
    Client client(harness.document());
    auto add = [&](const std::string& type, std::map<std::string, double> params) -> uint64_t {
        return apply(client, azl::AddEffect{1, std::nullopt, {0, type, std::move(params)}})["effect"]["id"];
    };
    const uint64_t transform = add("transform", {});
    add("trails", {{"feedback", 1.0}});
    harness.step();
    apply(client, azl::SetEffectParam{transform, "positionX", 0.5});
    auto history = [&] {
        harness.step(2);
        return int(pixel(harness.readScreen(), 0, 270)[0]);
    };
    const int held = history();
    CHECK(held > 200);
    const uint64_t colorize = add("colorize", {{"saturation", 0.0}});
    CHECK(std::abs(history() - held) <= 3);
    apply(client, azl::MoveEffect{colorize, 0});
    CHECK(std::abs(history() - held) <= 3);
    apply(client, azl::SetEffectBypass{colorize, true});
    CHECK(std::abs(history() - held) <= 3);
    apply(client, azl::RemoveEffect{colorize});
    CHECK(std::abs(history() - held) <= 3);
    apply(client, azl::AddModulator{std::nullopt, {.target = {0, {0, transform, "rotation"}}, .max = 0.0}});
    CHECK(std::abs(history() - held) <= 3);
}

TEST_CASE("continuity: a speed change on a Random Jump clip continues its jump sequence")
{
    // Doubled at render frame 125, between two jumps, the clip shows what it would at speed 1, twice as fast.
    auto run = [](bool faster) {
        TempDir media;
        azl::EngineHarness harness({.width = 960, .height = 540, .seed = 7});
        Client client(harness.document());
        playMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"),
                  {azl::SyncMode::Bpm, azl::PlayMode::RandomJump, 0, 1.0, 4.0});
        std::vector<int> frames;
        for (int i = 0; i < 360; ++i) {
            if (faster && i == 125)
                apply(client, azl::SetClipTransport{.layer = 1, .speed = 2.0});
            harness.step();
            frames.push_back(shownFrame(harness.readScreen()));
        }
        return frames;
    };
    const std::vector<int> steady = run(false);
    const std::vector<int> faster = run(true);
    for (int i = 125; i < 240; ++i) {
        CAPTURE(i);
        CHECK(std::abs(faster[i] - steady[125 + 2 * (i - 125)]) <= 1);
    }
}

TEST_CASE("continuity: every macro and modulator control ramped over a second moves its target without a jump")
{
    report(sweepModulation());
    report({sweepBandEdge()});
}

TEST_CASE("continuity: every effect parameter ramped over a second changes the picture without a jump")
{
    if (!fullSweep()) {
        report(sweepEffect("transform", {"positionX", "rotation"}));
        report(sweepEffect("stripper", {"number", "direction", "speed"}));
        report(sweepEffect("trails", {"feedback"}));
        report(sweepEffect("reveal", {"evolve", "reshuffle"}));
        report(sweepEffect("transform", {"positionX"}, 2));
        report(sweepEffect("classicStripper", {"direction", "globalWidth"}, 1, {{"mode", 0.5}, {"number", 0.4}},
                           " in mode 3"));
        report(sweepEffect("lines", {"count", "angle", "speed"}));
        report(sweepEffect("noise", {"scale", "speed"}));
        return;
    }
    for (const std::string& type : azl::generatorTypes()) {
        report(sweepEffect(type));
        report(sweepEffect(type, {}, 2));
    }
    for (const char* type : {"transform", "colorize", "edgeDetection", "stripper", "trails", "reveal",
                             "classicStripper", "classicColorize", "classicShiftRgb", "classicMirror",
                             "classicKaleidoscope", "classicTwitch", "classicAutoMask", "classicBrightnessContrast",
                             "classicLevels", "classicThreshold", "classicHueRotate", "classicMirrorQuad",
                             "classicPolarKaleido", "classicTrails", "classicStrobe", "classicShiftGlitch",
                             "classicEdgeDetection"}) {
        report(sweepEffect(type));
        report(sweepEffect(type, {}, 2));
    }
    for (const auto& [label, mode] : {std::pair{" in mode 3", 0.5}, {" in mode 4", 0.67}, {" in mode 6", 1.0}})
        report(sweepEffect("classicStripper", {}, 1, {{"mode", mode}, {"number", 0.4}}, label));
}

TEST_CASE("continuity: a beat LFO over 1% of evolve only shimmers, and a slow reshuffle never changes every frame")
{
    auto steps = [](azl::EngineHarness& harness) {
        std::vector<double> out;
        harness.step();
        azl::Image last = harness.readScreen();
        for (int i = 0; i < 60; ++i) {
            harness.step();
            azl::Image image = harness.readScreen();
            out.push_back(difference(image, last));
            last = std::move(image);
        }
        return out;
    };
    for (const char* type : {"reveal", "stripper"}) {
        CAPTURE(type);
        azl::EngineHarness harness(kSmall);
        Client client(harness.document());
        const Message added = apply(client, azl::AddEffect{1, std::nullopt, {0, type, {}}});
        azl::Modulator lfo;
        lfo.target = {0, {0, added["effect"]["id"], "evolve"}};
        lfo.max = 0.04;
        apply(client, azl::AddModulator{std::nullopt, lfo});
        for (const double step : steps(harness))
            CHECK(step < 1.0);
    }

    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    apply(client, azl::AddEffect{1, std::nullopt, {0, "reveal", {{"reshuffle", 0.1}}}});
    for (const double step : steps(harness))
        CHECK(step < 1.0);
}

namespace {

// The largest change of any pixel's RGB between two pictures, in 8-bit levels.
int largestChange(const azl::Image& a, const azl::Image& b)
{
    int largest = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            largest = std::max(largest, std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
    return largest;
}

} // namespace

TEST_CASE("continuity: Reveal's cells fade as evolve moves, by hand at any pace or by the gig show's LFO, never pop")
{
    // The gig show's Reveal on white, so a cell that pops changes by up to 255 levels. Evolve moves each cell's
    // visibility by at most 1.5 per unit (the smoothstep between two seeds' orders), 383 levels on white.
    const std::map<std::string, double> show{{"progress", 0.7}, {"columns", 5.0}, {"rows", 4.0}, {"gap", 0.0},
                                             {"softness", 0.05}, {"evolve", 1.0}};
    auto start = [&](Client& client) -> uint64_t {
        apply(client, azl::SetTempo{130.0});
        apply(client, azl::SetLayerClip{1, azl::ClipRef{.name = "solid",
                                                        .effects = {{0, "solid", {{"red", 1.0}, {"green", 1.0},
                                                                                  {"blue", 1.0}}}}}});
        return apply(client, azl::AddEffect{1, std::nullopt, {0, "reveal", show}})["effect"]["id"];
    };

    for (const double pace : {0.005, 0.02}) {
        CAPTURE(pace);
        azl::EngineHarness harness(kSmall);
        Client client(harness.document());
        const uint64_t reveal = start(client);
        apply(client, azl::SetEffectParam{reveal, "evolve", 0.0});
        harness.step();
        azl::Image last = harness.readScreen();
        int largest = 0;
        for (int i = 1; i * pace <= 4.0; ++i) {
            apply(client, azl::SetEffectParam{reveal, "evolve", i * pace});
            harness.step();
            azl::Image image = harness.readScreen();
            largest = std::max(largest, largestChange(image, last));
            last = std::move(image);
        }
        CHECK(largest <= int(383.0 * pace) + 3);
    }

    // 16 beats over 0..4 on top of evolve 1 at 130 BPM: evolve moves up to 0.028 a frame, about 11 levels; a cell
    // that pops changes by over 100.
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    const uint64_t reveal = start(client);
    azl::Modulator lfo;
    lfo.beats = 16.0;
    lfo.max = 4.0;
    lfo.target = {0, {0, reveal, "evolve"}};
    apply(client, azl::AddModulator{std::nullopt, lfo});
    harness.step();
    azl::Image last = harness.readScreen();
    int largest = 0;
    for (int i = 0; i < 443; ++i) {
        harness.step();
        azl::Image image = harness.readScreen();
        largest = std::max(largest, largestChange(image, last));
        last = std::move(image);
    }
    CHECK(largest <= 20);
}
