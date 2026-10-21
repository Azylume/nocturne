#include "clips.hpp"
#include "document/composition_file.hpp"
#include "document/cue_edits.hpp"

#include <doctest.h>

#include <string>
#include <vector>

namespace {

using azl::ClipRef;
using azl::DeckLayer;
using azl::Effect;

Effect colorize(double hue)
{
    return {0, "colorize", {{"hue", hue}, {"saturation", 1.0}}};
}

// Deck 1 holds columns 1 and 2, deck 2 columns 3 and 4; the layer has a clip in columns 1 and 3.
azl::Composition twoDecks()
{
    azl::Composition c = azl::defaultComposition();
    c.decks = {{1, "Deck 1", {{1, "A"}, {2, "B"}}}, {2, "Deck 2", {{3, "C"}, {4, "D"}}}};
    c.layers[0].clips[1] = ClipRef{.hash = kUnknownHash, .name = "one"};
    c.layers[0].clips[3] = ClipRef{.hash = kUnknownHash, .name = "three"};
    c.layers[0].effects = {{1, "colorize", {{"hue", 0.0}}}};
    return c;
}

azl::Edit apply(azl::Composition& c, azl::Edit edit)
{
    return azl::applyEdit(c, edit);
}

} // namespace

TEST_CASE("a layer's settings per deck fall back to the layer's, field by field")
{
    azl::Composition c = twoDecks();
    azl::Layer& layer = c.layers[0];
    layer.blend = azl::BlendMode::Screen;
    layer.transition = {azl::TransitionMode::Wipe, 2.0};
    layer.autopilot = {azl::AutopilotMode::Next, 4, 8};
    layer.decks[2] = DeckLayer{.effects = std::vector<Effect>{{2, "colorize", {{"hue", 0.6}}}},
                               .blend = azl::BlendMode::Add, .transitionBeats = 8.0,
                               .autopilotMode = azl::AutopilotMode::Random};

    CHECK(azl::columnDeck(c, 3) == 2);
    CHECK(azl::columnDeck(c, 9) == 0);
    CHECK(&azl::deckEffects(layer, 1) == &layer.effects);
    CHECK(azl::deckEffects(layer, 2)[0].id == 2);
    CHECK(azl::deckBlend(layer, 1) == azl::BlendMode::Screen);
    CHECK(azl::deckBlend(layer, 2) == azl::BlendMode::Add);
    CHECK(azl::deckTransition(layer, 2) == azl::Transition{azl::TransitionMode::Wipe, 8.0});
    CHECK(azl::deckAutopilot(layer, 2) == azl::Autopilot{azl::AutopilotMode::Random, 4, 8});
    CHECK(azl::cuedAutopilot(layer, 2, {{1, std::nullopt, 16}}) == azl::Autopilot{azl::AutopilotMode::Random, 16, 8});
}

TEST_CASE("an effect moves between any two chains in one edit, keeping its id, and moves back on undo")
{
    azl::Composition c = twoDecks();
    const azl::Layer& layer = c.layers[0];
    apply(c, azl::SetDeckLayer{1, 2, {.effects = std::vector<Effect>{{5, "colorize", {}}}}});

    // Into the clip in column 3, then into deck 2's own chain behind its effect, then the composition's.
    azl::Edit undoClip = apply(c, azl::MoveEffect{1, 0, 1, 3});
    CHECK(layer.effects.empty());
    REQUIRE(layer.clips.at(3).effects.size() == 1);
    CHECK(layer.clips.at(3).effects[0].id == 1);
    azl::Edit undoDeck = apply(c, azl::MoveEffect{1, 1, 1, std::nullopt, 2});
    CHECK(layer.clips.at(3).effects.empty());
    CHECK(layer.decks.at(2).effects->at(1).id == 1);
    azl::Edit undoComposition = apply(c, azl::MoveEffect{1, 0, 0});
    CHECK(c.effects.at(0).id == 1);

    apply(c, undoComposition);
    CHECK(layer.decks.at(2).effects->at(1).id == 1);
    apply(c, undoDeck);
    CHECK(layer.clips.at(3).effects[0].id == 1);
    apply(c, undoClip);
    CHECK(layer.effects.at(0).id == 1);
    CHECK(azl::editToJson(azl::editFromJson(azl::editToJson(undoDeck))) == azl::editToJson(undoDeck));

    // Past the end of the destination, or into a chain that does not exist, is refused.
    azl::Edit far = azl::MoveEffect{1, 2, 1, 3};
    CHECK_THROWS_AS(azl::applyEdit(c, far), azl::ProtocolError);
    azl::Edit noClip = azl::MoveEffect{1, 0, 1, 2};
    CHECK_THROWS_AS(azl::applyEdit(c, noClip), azl::ProtocolError);
    CHECK(layer.effects.at(0).id == 1);
}

TEST_CASE("per-deck settings are undoable edits whose chains get unique ids and take effect edits")
{
    azl::Composition c = twoDecks();
    const azl::Layer& layer = c.layers[0];

    azl::Edit undo = apply(c, azl::SetDeckLayer{1, 2, {.effects = std::vector<Effect>{colorize(0.6)},
                                                       .blend = azl::BlendMode::Add}});
    REQUIRE(layer.decks.contains(2));
    const uint64_t own = layer.decks.at(2).effects->at(0).id;
    CHECK(own != 1);
    CHECK(layer.decks.at(2).effects->at(0).params.contains("mix"));

    // Effect edits find the deck's chain by effect id; removing and undoing puts the effect back into it.
    apply(c, azl::SetEffectParam{own, "hue", 0.7});
    CHECK(layer.decks.at(2).effects->at(0).params.at("hue") == 0.7);
    const uint64_t added = std::get<azl::RemoveEffect>(apply(c, azl::AddEffect{1, 0, colorize(0.1), {}, 2})).effect;
    CHECK(layer.decks.at(2).effects->at(0).id == added);
    azl::Edit back = apply(c, azl::RemoveEffect{added});
    CHECK(std::get<azl::AddEffect>(back).deck == 2);
    apply(c, back);
    CHECK(layer.decks.at(2).effects->size() == 2);
    CHECK(layer.effects.size() == 1);
    CHECK(azl::effectsOn(c, 1).size() == 3);

    // A deck without a chain of its own, an id in use or a group is refused, leaving the document as it was.
    azl::Edit noChain = azl::AddEffect{1, std::nullopt, colorize(0.1), {}, 1};
    CHECK_THROWS_AS(azl::applyEdit(c, noChain), azl::ProtocolError);
    azl::Edit taken = azl::SetDeckLayer{1, 1, {.effects = std::vector<Effect>{{1, "colorize", {}}}}};
    CHECK_THROWS_AS(azl::applyEdit(c, taken), azl::ProtocolError);
    azl::Edit badDeck = azl::SetDeckLayer{1, 9, {.blend = azl::BlendMode::Add}};
    CHECK_THROWS_AS(azl::applyEdit(c, badDeck), azl::ProtocolError);
    azl::Edit badBars = azl::SetDeckLayer{1, 1, {.autopilotBars = 0}};
    CHECK_THROWS_AS(azl::applyEdit(c, badBars), azl::ProtocolError);
    CHECK_FALSE(layer.decks.contains(1));

    // Replacing the settings keeps the deck chain's own ids; empty settings make every field the layer's again.
    DeckLayer settings = layer.decks.at(2);
    settings.blend.reset();
    apply(c, azl::SetDeckLayer{1, 2, settings});
    CHECK_FALSE(layer.decks.at(2).blend);
    CHECK(layer.decks.at(2).effects->at(1).id == own);
    azl::Edit restore = apply(c, azl::SetDeckLayer{1, 2, {}});
    CHECK_FALSE(layer.decks.contains(2));
    apply(c, restore);
    CHECK(layer.decks.at(2).effects->at(1).id == own);
    apply(c, undo);
    CHECK(layer.decks.empty());
}

TEST_CASE("removing a deck takes the layers' settings in it along, and undo brings them back")
{
    azl::Composition c = twoDecks();
    apply(c, azl::SetDeckLayer{1, 2, {.effects = std::vector<Effect>{colorize(0.6)}, .transitionBeats = 4.0}});
    const DeckLayer settings = c.layers[0].decks.at(2);
    azl::Edit undo = apply(c, azl::RemoveDeck{2});
    CHECK(c.layers[0].decks.empty());
    const azl::Edit json = azl::editFromJson(azl::editToJson(undo));
    CHECK(std::get<azl::AddDeck>(json).layers.at(1) == settings);
    apply(c, json);
    CHECK(c.layers[0].decks.at(2) == settings);
}

TEST_CASE("a deck moves to another index as one undoable edit, the shown deck kept, and the order is saved")
{
    azl::Composition c = twoDecks();
    c.decks.push_back({3, "Deck 3", {{5, "E"}}});
    c.deck = 2;
    auto order = [&] {
        std::vector<uint64_t> ids;
        for (const azl::Deck& deck : c.decks)
            ids.push_back(deck.id);
        return ids;
    };

    azl::Edit undo = apply(c, azl::editFromJson(azl::editToJson(azl::MoveDeck{3, 0})));
    CHECK(order() == std::vector<uint64_t>{3, 1, 2});
    CHECK(c.deck == 2);
    CHECK(c.decks[0].columns[0].id == 5);
    azl::Edit redo = apply(c, undo);
    CHECK(order() == std::vector<uint64_t>{1, 2, 3});
    apply(c, redo);
    CHECK(order() == std::vector<uint64_t>{3, 1, 2});
    apply(c, azl::MoveDeck{3, 2});
    CHECK(order() == std::vector<uint64_t>{1, 2, 3});

    apply(c, azl::MoveDeck{1, 2});
    const azl::Composition loaded = azl::compositionFileFromJson(azl::toJson(azl::CompositionFile{c})).composition;
    CHECK(loaded.decks == c.decks);
    CHECK(loaded.deck == 2);

    azl::Edit far = azl::MoveDeck{1, 3};
    CHECK_THROWS_AS(azl::applyEdit(c, far), azl::ProtocolError);
    azl::Edit missing = azl::MoveDeck{9, 0};
    CHECK_THROWS_AS(azl::applyEdit(c, missing), azl::ProtocolError);
}

TEST_CASE("per-deck settings are saved only when a layer has them; their edits travel as JSON")
{
    const azl::Composition plain = twoDecks();
    const Message plainFile = azl::toJson(azl::CompositionFile{plain});
    CHECK_FALSE(plainFile["composition"]["layers"][0].contains("decks"));

    azl::Composition c = twoDecks();
    c.layers[0].decks[2] = DeckLayer{.effects = std::vector<Effect>{{2, "colorize", {{"hue", 0.6}}}},
                                     .blend = azl::BlendMode::Add,
                                     .transitionMode = azl::TransitionMode::Dissolve,
                                     .autopilotBars = 8};
    const Message file = azl::toJson(azl::CompositionFile{c});
    const Message& decks = file["composition"]["layers"][0]["decks"];
    CHECK(decks["2"]["blend"] == "add");
    CHECK(decks["2"]["transition"] == Message{{"mode", "dissolve"}});
    CHECK(decks["2"]["autopilot"] == Message{{"bars", 8}});
    CHECK(decks["2"]["effects"][0]["id"] == 2);
    const azl::Composition loaded = azl::compositionFileFromJson(file).composition;
    CHECK(loaded.layers[0].decks == c.layers[0].decks);

    const azl::SetDeckLayer edit{1, 2, c.layers[0].decks[2]};
    const azl::Edit back = azl::editFromJson(azl::editToJson(edit));
    CHECK(std::get<azl::SetDeckLayer>(back).settings == edit.settings);
    const azl::Edit addEffect = azl::editFromJson(azl::editToJson(azl::AddEffect{1, 0, colorize(0.2), {}, 2}));
    CHECK(std::get<azl::AddEffect>(addEffect).deck == 2);
    CHECK_FALSE(azl::editToJson(azl::AddEffect{1, 0, colorize(0.2)}).contains("deck"));
}

namespace {

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
constexpr double kRed = 0.0;
constexpr double kBlue = 0.6;

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void triggerColumn(Client& client, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerColumn");
}

struct Look {
    azl::BlendMode blend = azl::BlendMode::Normal;
    double hue = kRed;
};

// Layer 1 plays count_240 in deck 1's column 1 from frame 0, over layer 2, which has no clips and shows the test card.
// Deck 2's column 5 holds count_709_tv. Deck 1 uses the layer's shared settings, deck 2 its own chain, blend and a
// two-beat wipe; the shared transition cuts. A trigger given at frame 300 lands on beat 12, frame 360.
struct Decks {
    Decks(Look shared, Look own) : harness(kSize), client(harness.document())
    {
        apply(client, azl::AddLayer{0, 0, azl::Layer{}});
        apply(client, azl::AddDeck{std::nullopt, azl::Deck{0, "Deck 2", {{5, "E"}}}, {}});
        apply(client, azl::AddEffect{1, std::nullopt, colorize(shared.hue)});
        apply(client, azl::SetLayerBlend{1, shared.blend});
        apply(client, azl::SetDeckLayer{1, 2, {.effects = std::vector<Effect>{colorize(own.hue)}, .blend = own.blend,
                                               .transitionMode = azl::TransitionMode::Wipe, .transitionBeats = 2.0}});
        apply(client, azl::SetSlot{1, 1, ClipRef{add(client, copyMedia("count_240.mp4", media.path / "a.mp4")), "a"}});
        apply(client,
              azl::SetSlot{1, 5, ClipRef{add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")), "b"}});
        triggerColumn(client, 1);
        harness.step(300);
    }

    // Halfway through the wipe into deck 2.
    azl::Image halfway()
    {
        triggerColumn(client, 5);
        harness.step(91);
        return harness.readScreen();
    }

    TempDir media;
    azl::EngineHarness harness;
    Client client;
};

// The largest channel difference between two pictures over columns [x0, x1).
int worstIn(const azl::Image& a, const azl::Image& b, uint32_t x0, uint32_t x1)
{
    int worst = 0;
    for (uint32_t y = 0; y < a.height; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(int(pixel(a, x, y)[c]) - int(pixel(b, x, y)[c])));
    return worst;
}

// Summed red minus summed blue over columns [x0, x1).
int64_t redOverBlue(const azl::Image& image, uint32_t x0, uint32_t x1)
{
    int64_t sum = 0;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            sum += int(pixel(image, x, y)[0]) - int(pixel(image, x, y)[2]);
    return sum;
}

} // namespace

TEST_CASE("a clip plays through its deck's layer chain, the shared one where its deck has none")
{
    Decks decks({}, {.hue = kBlue});
    CHECK(redOverBlue(decks.harness.readScreen(), 0, kSize.width) > 0);
    const uint64_t pipelines = decks.harness.compileStats().pipelines;
    triggerColumn(decks.client, 5);
    decks.harness.step(150);
    CHECK(decks.harness.stats().fading == 0);
    CHECK(redOverBlue(decks.harness.readScreen(), 0, kSize.width) < 0);
    CHECK(decks.harness.compileStats().pipelines == pipelines);

    // Back to deck 1: the shared transition cuts, and the shared chain is back.
    triggerColumn(decks.client, 1);
    decks.harness.step(91);
    CHECK(decks.harness.stats().fading == 0);
    CHECK(redOverBlue(decks.harness.readScreen(), 0, kSize.width) > 0);
    CHECK(decks.harness.stats().videoMisses == 0);
}

TEST_CASE("a fade between decks runs each clip with its own deck's chain and blend, in the incoming deck's transition")
{
    const uint32_t half = kSize.width / 2;
    const uint32_t margin = 8;

    // Same blend: the incoming clip left of the wipe through deck 2's chain, the outgoing one right of it through the
    // shared chain.
    Decks normal({}, {.hue = kBlue});
    const azl::Image chains = normal.halfway();
    CHECK(normal.harness.stats().fading == 1);
    CHECK(redOverBlue(chains, 0, half - margin) < 0);
    CHECK(redOverBlue(chains, half + margin, kSize.width) > 0);

    // Different blends: each side as it is when both decks blend its way.
    Decks split({}, {azl::BlendMode::Add, kBlue});
    const azl::Image mixed = split.halfway();
    Decks added({azl::BlendMode::Add, kRed}, {azl::BlendMode::Add, kBlue});
    const azl::Image allAdd = added.halfway();
    CHECK(worstIn(mixed, allAdd, 0, half - margin) <= 1);
    CHECK(worstIn(mixed, chains, half + margin, kSize.width) <= 1);
    CHECK(worstIn(chains, allAdd, 0, half - margin) > 32);
    CHECK(worstIn(chains, allAdd, half + margin, kSize.width) > 32);
    for (Decks* run : {&normal, &split, &added})
        CHECK(run->harness.stats().videoMisses == 0);
}

TEST_CASE("an alpha fade between decks sharing the layer's chain runs it once; deck chains of their own run apart")
{
    // Trails makes the chain two passes, so it cannot run both clips in one.
    const Effect trails{0, "trails", {{"feedback", 0.0}}};
    const auto passesWhileFading = [&](bool ownChain) {
        Decks decks({}, {.hue = kRed});
        apply(decks.client, azl::AddEffect{1, std::nullopt, trails});
        DeckLayer settings{.transitionMode = azl::TransitionMode::Alpha, .transitionBeats = 2.0};
        if (ownChain)
            settings.effects = std::vector<Effect>{colorize(kRed), trails};
        apply(decks.client, azl::SetDeckLayer{1, 2, settings});
        decks.harness.step(1);
        const uint64_t steady = decks.harness.stats().passes;
        triggerColumn(decks.client, 5);
        decks.harness.step(91);
        CHECK(decks.harness.stats().fading == 1);
        return int64_t(decks.harness.stats().passes) - int64_t(steady);
    };
    CHECK(passesWhileFading(false) == 0);
    CHECK(passesWhileFading(true) > 0);
}

TEST_CASE("Autopilot steps a layer through the deck it plays as that deck sets it")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    apply(client, azl::AddDeck{std::nullopt, azl::Deck{0, "Deck 2", {{5, "E"}, {6, "F"}}}, {}});
    for (uint64_t column : {1, 2, 5, 6})
        apply(client, azl::SetSlot{1, column,
                                   ClipRef{add(client, copyMedia("count_709_tv.mp4",
                                                                 media.path / (std::to_string(column) + ".mp4"))),
                                           "clip"}});
    apply(client, azl::SetDeckLayer{1, 2, {.autopilotMode = azl::AutopilotMode::Next, .autopilotBars = 1}});
    const auto playing = [&] {
        client.send({{"type", "clock"}, {"op", "state"}});
        Message state = client.next();
        REQUIRE(state["op"] == "state");
        return state["clips"].at(0)["column"].get<uint64_t>();
    };

    triggerColumn(client, 1);
    harness.step(480);
    CHECK(playing() == 1);
    triggerColumn(client, 5);
    harness.step(60);
    CHECK(playing() == 5);
    harness.step(120);
    CHECK(playing() == 6);
    harness.step(120);
    CHECK(playing() == 5);
    CHECK(harness.stats().videoMisses == 0);
}
