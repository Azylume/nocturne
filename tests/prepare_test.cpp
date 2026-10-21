#include "protocol/prepare.hpp"
#include "test_client.hpp"

#include <algorithm>

namespace {

using azl::PreparedCopy;

azl::Composition threeLayers()
{
    azl::Composition c = azl::defaultComposition();
    for (uint64_t id : {2, 3}) {
        azl::Edit add = azl::AddLayer{0, std::nullopt, azl::Layer{.id = id, .name = "Layer " + std::to_string(id)}};
        azl::applyEdit(c, add);
    }
    c.layers[1].clips[1] = azl::ClipRef{"aaaa", "one"};
    return c;
}

void edit(azl::Composition& c, azl::Edit e)
{
    azl::applyEdit(c, e);
}

std::vector<uint64_t> layerIds(const azl::Composition& c)
{
    std::vector<uint64_t> ids;
    for (const azl::Layer& layer : c.layers)
        ids.push_back(layer.id);
    return ids;
}

Message sendPrepared(Client& client, const PreparedCopy& copy, const char* at)
{
    client.send({{"type", "prepare"}, {"op", "send"}, {"at", at}, {"edits", azl::preparedToJson(copy.edits())}});
    for (;;) {
        Message reply = client.next();
        if (reply["type"] == "prepare" || reply["type"] == "error")
            return reply;
    }
}

} // namespace

TEST_CASE("prepare: the copy follows the venue; Prepare's edits stay on top unless the venue changed their ground")
{
    azl::Composition venue = threeLayers();
    PreparedCopy copy(venue);
    copy.edit(azl::SetLayerOpacity{1, 0.5});
    copy.edit(azl::SetLayerBlend{2, azl::BlendMode::Add});
    copy.edit(azl::SetQuantise{8});
    CHECK(copy.undoSteps() == 3);

    edit(venue, azl::SetLayerOpacity{1, 0.9});
    edit(venue, azl::SetLayerOpacity{3, 0.1});
    edit(venue, azl::TriggerClip{2, 1, 0.0});
    copy.follow(venue);
    const azl::Composition& shown = copy.composition();
    CHECK(shown.layers[0].opacity == doctest::Approx(0.9));
    CHECK(shown.layers[2].opacity == doctest::Approx(0.1));
    CHECK(shown.layers[1].blend == azl::BlendMode::Add);
    CHECK(shown.quantise == 8);
    REQUIRE(copy.skipped().size() == 1);
    CHECK(copy.skipped()[0] == "setLayerOpacity (layers/#1/opacity): the venue changed it meanwhile");

    // An edit made after the venue's own change builds on it, so it goes out.
    copy.edit(azl::SetLayerOpacity{1, 0.2});
    const azl::Rebased sent = azl::rebase(venue, copy.edits());
    CHECK(sent.composition.layers[0].opacity == doctest::Approx(0.2));
    CHECK(sent.applied.size() == 3);
    CHECK(sent.skipped.size() == 1);
}

TEST_CASE("prepare: an added layer and edits on it go out together, or not at all when the venue took its id")
{
    azl::Composition venue = azl::defaultComposition();
    PreparedCopy copy(venue);
    copy.edit(azl::AddLayer{});
    const uint64_t added = copy.composition().layers.back().id;
    copy.edit(azl::SetLayerOpacity{added, 0.4});

    azl::Rebased clean = azl::rebase(venue, copy.edits());
    CHECK(clean.skipped.empty());
    REQUIRE(clean.composition.layers.size() == 2);
    CHECK(clean.composition.layers[1].opacity == doctest::Approx(0.4));

    edit(venue, azl::AddLayer{0, std::nullopt, azl::Layer{.id = added, .name = "Theirs"}});
    azl::Rebased clash = azl::rebase(venue, copy.edits());
    CHECK(clash.applied.empty());
    CHECK(clash.skipped.size() == 2);
    REQUIRE(clash.composition.layers.size() == 2);
    CHECK(clash.composition.layers[1].name == "Theirs");
    CHECK(clash.composition.layers[1].opacity == doctest::Approx(1.0));
}

TEST_CASE("prepare: moving a layer is left out when the venue reordered or added layers meanwhile")
{
    azl::Composition venue = threeLayers();
    PreparedCopy copy(venue);
    copy.edit(azl::MoveLayer{1, 0, 2});
    CHECK(layerIds(copy.composition()) == std::vector<uint64_t>{2, 3, 1});

    azl::Composition renamed = venue;
    edit(renamed, azl::Rename{"layer", 2, "Still fine"});
    CHECK(azl::rebase(renamed, copy.edits()).skipped.empty());

    edit(venue, azl::MoveLayer{3, 0, 0});
    const azl::Rebased moved = azl::rebase(venue, copy.edits());
    CHECK(layerIds(moved.composition) == std::vector<uint64_t>{3, 1, 2});
    CHECK(moved.skipped.size() == 1);
}

TEST_CASE("prepare: one gesture is one undo step; a repeated value keeps the last edit; undo and redo replay the copy")
{
    const azl::Composition venue = azl::defaultComposition();
    PreparedCopy copy(venue);
    for (double v : {0.9, 0.6, 0.3})
        copy.edit(azl::SetLayerOpacity{1, v}, 7);
    copy.edit(azl::SetQuantise{8}, 7);
    CHECK(copy.undoSteps() == 1);
    CHECK(copy.edits().size() == 2);
    CHECK(copy.ops(true) == std::vector<std::string>{"setLayerOpacity"});

    copy.edit(azl::SetTempo{150.0});
    REQUIRE(copy.undo());
    CHECK(copy.composition().bpm == doctest::Approx(120.0));
    CHECK(copy.redoSteps() == 1);
    REQUIRE(copy.redo());
    CHECK(copy.composition().bpm == doctest::Approx(150.0));
    REQUIRE(copy.undo());
    REQUIRE(copy.undo());
    CHECK(copy.empty());
    CHECK(copy.composition().layers[0].opacity == doctest::Approx(1.0));
    CHECK_FALSE(copy.undo());

    CHECK_THROWS_AS(copy.edit(azl::SetLayerOpacity{99, 0.5}), azl::ProtocolError);
    CHECK_THROWS_AS(copy.edit(azl::SelectDeck{1}), azl::ProtocolError);
    CHECK(copy.empty());
}

TEST_CASE("prepare: the copy and its edits survive JSON; a live action is refused")
{
    const azl::Composition venue = threeLayers();
    PreparedCopy copy(venue);
    copy.edit(azl::SetLayerOpacity{2, 0.25});
    copy.edit(azl::RemoveLayer{3});
    copy.edit(azl::SetQuantise{2});
    copy.undo();
    const Message json = Message::parse(copy.toJson().dump());
    const PreparedCopy back = PreparedCopy::fromJson(json, venue);
    CHECK(back.undoSteps() == 2);
    CHECK(back.redoSteps() == 1);
    CHECK(azl::documentJson(back.composition()) == azl::documentJson(copy.composition()));

    Message live = azl::preparedToJson(copy.edits());
    live.push_back(azl::editToJson(azl::TriggerColumn{1}));
    CHECK_THROWS_AS(azl::preparedFromJson(live), azl::ProtocolError);
}

TEST_CASE("prepare: the document server applies a Send now as one change and one undo step")
{
    azl::EngineHarness h({.width = 320, .height = 180}, 15.0, {}, threeLayers());
    Client director(h.document());
    PreparedCopy copy(h.document().document());
    copy.edit(azl::SetLayerOpacity{1, 0.5});
    copy.edit(azl::SetLayerBypass{2, true});
    const uint64_t change = h.document().change();

    const Message reply = sendPrepared(director, copy, "now");
    REQUIRE_MESSAGE(reply["op"] == "sent", reply.dump());
    CHECK(reply["applied"] == 2);
    CHECK(reply["skipped"].empty());
    CHECK(h.document().change() == change + 1);
    CHECK(h.document().document().layers[1].bypass);

    director.send(msg::undo());
    director.next();
    const azl::Composition undone = h.document().document();
    CHECK(undone.layers[0].opacity == doctest::Approx(1.0));
    CHECK_FALSE(undone.layers[1].bypass);
}

TEST_CASE("prepare: a Send at the next deck switch waits as a Cue that survives the file and fires on any switch")
{
    azl::Composition show = threeLayers();
    show.decks.push_back({2, "Deck 2", {{11, "Column 11"}}});
    azl::EngineHarness h({.width = 320, .height = 180}, 15.0, {}, show);
    PreparedCopy copy(h.document().document());
    copy.edit(azl::SetLayerOpacity{2, 0.5});
    copy.edit(azl::SetLayerOpacity{3, 0.6});
    {
        Client director(h.document());
        const Message reply = sendPrepared(director, copy, "switch");
        REQUIRE_MESSAGE(reply["op"] == "scheduled", reply.dump());
    }
    azl::Composition saved = h.document().document();
    REQUIRE(saved.cues.size() == 1);
    const azl::Composition reloaded = Message(saved).get<azl::Composition>();
    CHECK(reloaded.cues == saved.cues);
    CHECK(saved.layers[1].opacity == doctest::Approx(1.0));

    Client watcher(h.document());
    {
        Client other(h.document());
        ::apply(other, azl::SetLayerOpacity{3, 0.9});
    }
    h.document().perform(azl::SelectDeck{2}, "trigger");
    const azl::Composition fired = h.document().document();
    CHECK(fired.layers[1].opacity == doctest::Approx(0.5));
    CHECK(fired.layers[2].opacity == doctest::Approx(0.9));
    CHECK(fired.cues.empty());
    Message sent;
    do
        sent = watcher.next();
    while (sent["type"] != "prepare");
    CHECK(sent["op"] == "sent");
    CHECK(sent["applied"] == 1);
    CHECK(sent["skipped"].size() == 1);

    // A Cue with a prepared change carries nothing else.
    azl::Cue mixed{.id = 5, .section = azl::Section::Drop, .prepared = Message::array()};
    azl::Edit add = azl::AddCue{std::nullopt, mixed};
    CHECK_THROWS_AS(azl::applyEdit(saved, add), azl::ProtocolError);
}

TEST_CASE("prepare: a change sent at the next deck switch never switches the deck itself")
{
    azl::Composition show = threeLayers();
    show.decks.push_back({2, "Deck 2", {{11, "Column 11"}}});
    show.decks.push_back({3, "Deck 3", {{21, "Column 21"}}});
    azl::EngineHarness h({.width = 320, .height = 180}, 15.0, {}, show);
    PreparedCopy copy(h.document().document());
    copy.edit(azl::RemoveDeck{2});
    copy.edit(azl::SetLayerOpacity{2, 0.5});
    CHECK(copy.composition().decks.size() == 2);
    {
        Client director(h.document());
        const Message reply = sendPrepared(director, copy, "switch");
        REQUIRE_MESSAGE(reply["op"] == "scheduled", reply.dump());
    }

    // The switch to Deck 2 fires the change; removing the deck now shown would switch again, so it is left out.
    Client watcher(h.document());
    h.document().perform(azl::SelectDeck{2}, "trigger");
    const azl::Composition fired = h.document().document();
    CHECK(fired.deck == 2);
    CHECK(fired.decks.size() == 3);
    CHECK(fired.layers[1].opacity == doctest::Approx(0.5));
    CHECK(fired.cues.empty());
    Message sent;
    do
        sent = watcher.next();
    while (sent["type"] != "prepare");
    CHECK(sent["applied"] == 1);
    REQUIRE(sent["skipped"].size() == 1);
    CHECK(sent["skipped"][0].get<std::string>().find("switch the deck") != std::string::npos);

    // Sent now, by hand, removing a deck that is not shown applies.
    PreparedCopy now(h.document().document());
    now.edit(azl::RemoveDeck{3});
    Client director(h.document());
    CHECK(sendPrepared(director, now, "now")["applied"] == 1);
    CHECK(h.document().document().deck == 2);
    CHECK(h.document().document().decks.size() == 2);
}
