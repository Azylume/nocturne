#include "engine/autopilot_stepper.hpp"

#include <doctest.h>

#include <atomic>
#include <set>
#include <vector>

using azl::AutopilotMode;
using azl::AutopilotStepper;
using azl::Composition;
using azl::Layer;
using azl::Play;

namespace {

// One deck of columns 1..4 and layers 10 and 20 with a clip in every column, layer 10 triggered on column 1.
Composition deckOfFour(AutopilotMode mode = AutopilotMode::Next)
{
    Composition composition;
    composition.decks = {{.id = 1, .name = "Deck", .columns = {{1, "1"}, {2, "2"}, {3, "3"}, {4, "4"}}}};
    composition.deck = 1;
    for (const uint64_t id : {10, 20}) {
        Layer layer{.id = id, .name = "layer"};
        for (const uint64_t column : {1, 2, 3, 4})
            layer.clips[column].hash = "clip" + std::to_string(column);
        layer.autopilot = {mode, 1, 2};
        composition.layers.push_back(layer);
    }
    composition.layers[0].playing = Play{1, 0.0};
    return composition;
}

struct Rig {
    azl::BeatClock clock;
    azl::CueState cues;
    std::atomic<uint32_t> seed{7};
    std::set<uint64_t> broken;
    AutopilotStepper stepper{clock, cues, seed,
                             [this](const Layer& layer, uint64_t column) {
                                 return column && layer.clips.contains(column) && !broken.contains(column);
                             }};
};

std::optional<Play> steer(Rig& rig, const Composition& composition, double beat, size_t layer = 0)
{
    return rig.stepper.steer(composition.layers[layer], composition, beat);
}

} // namespace

TEST_CASE("autopilot stepper: steps through the deck every N bars on the bar, from the layer's trigger")
{
    Rig rig;
    const Composition composition = deckOfFour();
    CHECK(steer(rig, composition, 0.0) == Play{1, 0.0});
    CHECK(rig.stepper.pickedNext(10) == 2);
    CHECK(steer(rig, composition, 3.9) == Play{1, 0.0});
    CHECK(steer(rig, composition, 4.0) == Play{2, 4.0});
    CHECK(steer(rig, composition, 9.5) == Play{3, 8.0});
    CHECK(steer(rig, composition, 12.0) == Play{4, 12.0});
    CHECK(steer(rig, composition, 16.0) == Play{1, 16.0});
    CHECK(steer(rig, composition, 16.0, 1) == std::nullopt);
}

TEST_CASE("autopilot stepper: skips cells that cannot play")
{
    Rig rig;
    rig.broken = {2, 3};
    const Composition composition = deckOfFour();
    steer(rig, composition, 0.0);
    CHECK(steer(rig, composition, 4.0) == Play{4, 4.0});
}

TEST_CASE("autopilot stepper: random steps repeat with the seed and never play the same cell twice in a row")
{
    const Composition composition = deckOfFour(AutopilotMode::Random);
    auto run = [&](uint32_t seed) {
        Rig rig;
        rig.seed = seed;
        std::vector<uint64_t> columns;
        for (int bar = 0; bar < 32; ++bar)
            columns.push_back(steer(rig, composition, bar * 4.0)->column);
        return columns;
    };
    const std::vector<uint64_t> first = run(3);
    CHECK(run(3) == first);
    for (size_t i = 1; i < first.size(); ++i)
        CHECK(first[i] != first[i - 1]);
}

TEST_CASE("autopilot stepper: a resync restarts the running step")
{
    Rig rig;
    const Composition composition = deckOfFour();
    steer(rig, composition, 0.0);
    CHECK(steer(rig, composition, 6.0) == Play{2, 4.0});
    rig.clock.resync(10.0);
    CHECK(steer(rig, composition, 1.0) == Play{2, 4.0});
    CHECK(steer(rig, composition, 4.0) == Play{3, 4.0});
}

TEST_CASE("autopilot stepper: a performer's trigger holds the layer for its hold, then it steps on from there")
{
    Rig rig;
    Composition composition = deckOfFour();
    steer(rig, composition, 0.0);
    composition.layers[0].playing = Play{3, 6.0};
    CHECK(steer(rig, composition, 6.0) == Play{3, 6.0});
    const AutopilotStepper::Shown shown = rig.stepper.shown(composition.layers[0], composition, 6.0);
    CHECK(shown.next == 4);
    CHECK(shown.held == 2);
    CHECK(steer(rig, composition, 11.9) == Play{3, 6.0});
    CHECK(steer(rig, composition, 12.0) == Play{4, 12.0});
    CHECK(rig.stepper.shown(composition.layers[0], composition, 12.0).held == 0);
}

TEST_CASE("autopilot stepper: the column Autopilot fires the next column on every layer and holds their own")
{
    Rig rig;
    Composition composition = deckOfFour();
    composition.column = 1;
    composition.columnAutopilot = {AutopilotMode::Next, 1, 2};
    rig.stepper.stepColumns(composition, 0.0);
    CHECK(rig.stepper.columnsRunning());
    CHECK(rig.stepper.columnNext() == 2);
    CHECK(rig.stepper.layerNext(composition.layers[0], composition) == 0);
    rig.stepper.stepColumns(composition, 4.0);
    CHECK(steer(rig, composition, 4.0) == Play{2, 4.0});
    CHECK(steer(rig, composition, 4.0, 1) == Play{2, 4.0});
    const AutopilotStepper::Columns columns = rig.stepper.columns(composition, 4.0);
    CHECK(columns.column == 2);
    CHECK(columns.next == 3);
    CHECK(columns.held == 0);
    composition.columnAutopilot.mode = AutopilotMode::Off;
    rig.stepper.stepColumns(composition, 8.0);
    CHECK_FALSE(rig.stepper.columnsRunning());
}

TEST_CASE("autopilot stepper: random skips empty cells, and random columns skip an empty bottom layer; next keeps them")
{
    Composition composition = deckOfFour(AutopilotMode::Random);
    composition.layers[0].clips.erase(3);
    std::set<uint64_t> landed;
    Rig rig;
    for (int bar = 0; bar < 64; ++bar)
        landed.insert(steer(rig, composition, bar * 4.0)->column);
    CHECK(landed == std::set<uint64_t>{1, 2, 4});

    composition.layers[1].clips.erase(2);
    composition.column = 1;
    for (const AutopilotMode mode : {AutopilotMode::Random, AutopilotMode::Next}) {
        Rig pilot;
        std::set<uint64_t> columns;
        composition.columnAutopilot = {mode, 1, 0};
        for (int bar = 0; bar < 64; ++bar) {
            pilot.stepper.stepColumns(composition, bar * 4.0);
            columns.insert(pilot.stepper.columns(composition, bar * 4.0).column);
        }
        const std::set<uint64_t> expected =
            mode == AutopilotMode::Random ? std::set<uint64_t>{1, 2, 4} : std::set<uint64_t>{1, 2, 3, 4};
        CHECK(columns == expected);
    }
}

TEST_CASE("autopilot stepper: a column Cue plays its column on every layer until the layer's next trigger")
{
    Rig rig;
    Composition composition = deckOfFour(AutopilotMode::Off);
    rig.cues.columns = {{.column = {.index = 2}, .beat = 8.0}};
    rig.stepper.fireColumnCues(composition, 8.0);
    CHECK(steer(rig, composition, 8.0) == Play{3, 8.0});
    CHECK(steer(rig, composition, 8.0, 1) == Play{3, 8.0});
    CHECK(rig.stepper.playOf(composition.layers[1]) == Play{3, 8.0});
    composition.layers[1].playing = Play{4, 9.0};
    CHECK(steer(rig, composition, 9.0, 1) == Play{4, 9.0});
    CHECK(azl::cueColumn(composition, {.column = 4}) == 4);
    CHECK(azl::cueColumn(composition, {.index = 9}) == 0);
}

TEST_CASE("autopilot stepper: its pilots restore into another stepper that steps on the same")
{
    Rig rig;
    Composition composition = deckOfFour();
    composition.layers[1].playing = Play{2, 0.0};
    for (const double beat : {0.0, 4.0, 8.0}) {
        steer(rig, composition, beat);
        steer(rig, composition, beat, 1);
    }
    const std::vector<azl::ResumePilot> saved = rig.stepper.pilots(composition);
    REQUIRE(saved.size() == 2);
    Rig other;
    other.stepper.restore(composition, saved);
    CHECK(other.stepper.pilots(composition) == saved);
    for (const double beat : {9.0, 12.0, 16.0}) {
        CHECK(steer(other, composition, beat) == steer(rig, composition, beat));
        CHECK(steer(other, composition, beat, 1) == steer(rig, composition, beat, 1));
    }
}
