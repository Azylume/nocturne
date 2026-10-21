#include "document/composition_file.hpp"
#include "document/cue_edits.hpp"
#include "document/wall_clock.hpp"
#include "engine/cue_service.hpp"
#include "harness/engine_harness.hpp"
#include "test_client.hpp"
#include "venues/gig.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr int64_t kHour = 3600;
constexpr azl::EngineConfig kSize{.width = 320, .height = 180};
constexpr double kFps = 15.0;

int64_t utc(const char* date, int hour, int minute = 0, int second = 0)
{
    return *azl::dateDays(date) * 86400 + hour * kHour + minute * 60 + second;
}

// Central European time: +1, and +2 from 2026-03-29 01:00 UTC to 2026-10-25 01:00 UTC.
int cet(int64_t t)
{
    return t >= utc("2026-03-29", 1) && t < utc("2026-10-25", 1) ? int(2 * kHour) : int(kHour);
}

azl::Composition twoDecks()
{
    azl::Composition c = azl::defaultComposition();
    c.decks.push_back({2, "Headliner", {{11, "H 1"}, {12, "H 2"}}});
    return c;
}

azl::Gig night()
{
    return {"mini", "2026-10-16", 22 * 60, 5 * 60, {{"Opener", 23 * 60, 1}, {"Headliner", 3 * 60, 2}, {"Closer", 4 * 60, 1}}};
}

uint64_t shownDeck(azl::EngineHarness& h)
{
    return h.document().document().deck;
}

struct Firing {
    double wall = 0.0;
    // The beat clock at the poll that fired it, and the bar the column landed on.
    double beat = 0.0;
    double bar = 0.0;
};

// Steps for up to `seconds` until `deck` shows.
std::optional<Firing> stepUntilDeck(azl::EngineHarness& h, uint64_t deck, double seconds)
{
    for (int i = 0; i < int(seconds * kFps); ++i) {
        const double wall = h.wallTime();
        const double beat = azl::beatAt(h.engine(), h.time());
        h.step();
        if (shownDeck(h) != deck)
            continue;
        const std::optional<azl::Play> playing = h.document().document().layers[0].playing;
        REQUIRE(playing);
        REQUIRE(playing->beat);
        return Firing{wall, beat, *playing->beat};
    }
    return std::nullopt;
}

void checkOnNextBar(const Firing& f)
{
    CHECK(std::fmod(f.bar, 4.0) == doctest::Approx(0.0));
    CHECK(f.bar >= f.beat - 1e-6);
    CHECK(f.bar - f.beat < 4.0);
}

} // namespace

TEST_CASE("gig: clock times, dates and the venue's local time across midnight and DST")
{
    CHECK(azl::clockMinutes("03:00") == 180);
    CHECK(azl::clockMinutes("23:59") == 23 * 60 + 59);
    CHECK_FALSE(azl::clockMinutes("24:00"));
    CHECK_FALSE(azl::clockMinutes("3:00"));
    CHECK(azl::clockText(185) == "03:05");
    CHECK(azl::dateDays("1970-01-02") == 1);
    CHECK(azl::dateText(*azl::dateDays("2026-10-16")) == "2026-10-16");
    CHECK_FALSE(azl::dateDays("2026-02-30"));
    CHECK_FALSE(azl::dateDays("2026-1-01"));

    // Summer time: 03:00 local is 01:00 UTC.
    CHECK(azl::unixAt(*azl::dateDays("2026-10-17"), 180, cet) == utc("2026-10-17", 1));
    // DST starts: 02:30 never shows; the clock passes it when it jumps from 02:00 to 03:00.
    CHECK(azl::unixAt(*azl::dateDays("2026-03-29"), 150, cet) == utc("2026-03-29", 1));
    CHECK(azl::unixAt(*azl::dateDays("2026-03-29"), 180, cet) == utc("2026-03-29", 1));
    // DST ends: 02:30 shows twice; the first counts.
    CHECK(azl::unixAt(*azl::dateDays("2026-10-25"), 150, cet) == utc("2026-10-25", 0, 30));

    const azl::CueAt daily{150};
    CHECK(azl::nextAt(daily, utc("2026-10-25", 0, 30), cet) == utc("2026-10-26", 1, 30));
    CHECK(azl::nextAt(daily, utc("2026-10-25", 0, 29, 59), cet) == utc("2026-10-25", 0, 30));
    // Past midnight on its own date; once passed, never again.
    const azl::CueAt dated{30, "2026-10-17"};
    CHECK(azl::nextAt(dated, utc("2026-10-16", 21, 50), cet) == utc("2026-10-16", 22, 30));
    CHECK_FALSE(azl::nextAt(dated, utc("2026-10-16", 22, 30), cet));
}

TEST_CASE("gig: the gig window, past midnight, for every channel alike")
{
    const std::vector<azl::GigWindow> gigs{{"2026-10-16", 22 * 60, 5 * 60}};
    CHECK_FALSE(azl::inGigWindow(gigs, double(utc("2026-10-16", 19, 59, 59)), {}, cet));
    CHECK(azl::inGigWindow(gigs, double(utc("2026-10-16", 20)), {}, cet));
    CHECK(azl::inGigWindow(gigs, double(utc("2026-10-17", 2, 59, 59)), "update", cet));
    CHECK_FALSE(azl::inGigWindow(gigs, double(utc("2026-10-17", 3)), "sync", cet));
    CHECK_FALSE(azl::inGigWindow({}, double(utc("2026-10-16", 23)), {}, cet));
}

TEST_CASE("gig: a Cue at a time of day and a deck switch are document state; old files read and write unchanged")
{
    azl::Composition c = twoDecks();
    azl::Cue old{.id = 1, .name = "Drop", .section = azl::Section::Drop, .column = azl::CueColumn{0, 0}};
    c.cues = {old};
    c.timeline.sections = {{azl::Section::Drop, 8.0}};
    const std::string before = nlohmann::json(c).dump();
    CHECK(before.find("\"gigs\"") == std::string::npos);
    CHECK(before.find("\"at\"") == std::string::npos);
    CHECK(before.find("\"gig\"") == std::string::npos);
    CHECK(nlohmann::json(nlohmann::json::parse(before).get<azl::Composition>()).dump() == before);

    const azl::Cue at{.name = "Headliner", .column = azl::CueColumn{0, 0}, .at = azl::CueAt{180, "2026-10-17"}, .deck = 2,
                      .gig = "2026-10-16"};
    const nlohmann::json j = at;
    CHECK(j["at"] == nlohmann::json{{"time", "03:00"}, {"date", "2026-10-17"}});
    CHECK(j["deck"] == 2);
    CHECK(j["gig"] == "2026-10-16");
    CHECK(j.get<azl::Cue>() == at);

    azl::Edit add = azl::AddCue{std::nullopt, at};
    azl::Edit undo = azl::applyEdit(c, add);
    CHECK(c.cues.size() == 2);
    CHECK(azl::editFromJson(azl::editToJson(add)).index() == add.index());
    azl::applyEdit(c, undo);
    CHECK(c.cues.size() == 1);

    // A time of day never fires on the beat clock.
    c.cues = {azl::Cue{.id = 5, .column = azl::CueColumn{0, 0}, .at = azl::CueAt{180}}};
    CHECK(azl::upcomingCues(c, 0.0).empty());

    auto refused = [&](azl::Cue cue) {
        azl::Edit edit = azl::AddCue{std::nullopt, cue};
        CHECK_THROWS_AS(azl::applyEdit(c, edit), azl::ProtocolError);
    };
    refused({.deck = 2});
    refused({.section = azl::Section::Drop, .at = azl::CueAt{180}});
    refused({.at = azl::CueAt{180}, .deck = 9});
    refused({.at = azl::CueAt{180, "2026-13-01"}});
    refused({.macros = {{1, 1.0, 0}}, .at = azl::CueAt{180}});

    azl::Edit window = azl::SetGigWindow{"2026-10-16", azl::GigWindow{"2026-10-16", 22 * 60, 5 * 60}};
    azl::Edit back = azl::applyEdit(c, window);
    CHECK(c.gigs.size() == 1);
    CHECK(nlohmann::json(c).dump().find("\"gigs\":[{\"date\":\"2026-10-16\",\"end\":\"05:00\",\"start\":\"22:00\"}]") !=
          std::string::npos);
    CHECK(azl::editToJson(window)["op"] == "setGigWindow");
    azl::applyEdit(c, back);
    CHECK(c.gigs.empty());
    azl::Edit bad = azl::SetGigWindow{"2026-10-16", azl::GigWindow{"2026-10-16", 60, 60}};
    CHECK_THROWS_AS(azl::applyEdit(c, bad), azl::ProtocolError);
}

TEST_CASE("gig: a deck switches only at a time of day; a musical Cue naming a deck is refused, and loads switched off")
{
    azl::Composition c = twoDecks();
    auto refused = [&](azl::Edit edit) { CHECK_THROWS_AS(azl::applyEdit(c, edit), azl::ProtocolError); };
    refused(azl::AddCue{std::nullopt, {.section = azl::Section::Drop, .deck = 2}});
    refused(azl::AddCue{std::nullopt, {.bar = 9, .deck = 2}});
    refused(azl::AddCue{std::nullopt, {.section = azl::Section::Drop, .column = azl::CueColumn{0, 0}, .every = 16, .deck = 2}});
    refused(azl::AddMacro{std::nullopt, {.name = "Deck", .targets = {{azl::ParamRef{0, 0, "deck"}}}}});
    CHECK(c.cues.empty());

    nlohmann::json file = azl::toJson(azl::CompositionFile{twoDecks()});
    file["composition"]["cues"] = nlohmann::json::parse(R"([
        {"id": 1, "name": "Peak at the drop", "section": "drop", "deck": 2},
        {"id": 2, "name": "Bar 9", "bar": 9, "deck": 2, "enabled": false},
        {"id": 3, "name": "Headliner", "at": {"time": "03:00"}, "deck": 2},
        {"id": 4, "name": "Drop", "section": "drop", "column": {"index": 0}}])");
    std::vector<std::string> warnings;
    azl::Composition loaded = azl::compositionFileFromJson(file, warnings).composition;
    REQUIRE(loaded.cues.size() == 4);
    CHECK_FALSE(loaded.cues[0].enabled);
    CHECK(loaded.cues[0].deck == 2u);
    CHECK(loaded.cues[0].section == azl::Section::Drop);
    CHECK_FALSE(loaded.cues[1].enabled);
    CHECK(loaded.cues[1].deck == 2u);
    CHECK(loaded.cues[2].enabled);
    CHECK(loaded.cues[3].enabled);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0] == "cue 1 \"Peak at the drop\": a deck switches only at a time of day; loaded switched off");
    CHECK(warnings[1].rfind("cue 2 \"Bar 9\"", 0) == 0);

    // Switched on again it is refused with the reason; a time-of-day Cue switches off and on.
    azl::Edit on = azl::SetCueEnabled{1, true};
    CHECK_THROWS_WITH_AS(azl::applyEdit(loaded, on), "a cue switches decks only at a time of day", azl::ProtocolError);
    azl::Edit off = azl::SetCueEnabled{3, false};
    azl::Edit back = azl::applyEdit(loaded, off);
    azl::applyEdit(loaded, back);
    CHECK(loaded.cues[2].enabled);

    nlohmann::json& cues = file["composition"]["cues"];
    cues.erase(0);
    cues.erase(0);
    warnings.clear();
    CHECK(azl::compositionFileFromJson(file, warnings).composition.cues == cues.get<std::vector<azl::Cue>>());
    CHECK(warnings.empty());
}

TEST_CASE("gig: Autopilot, macros, modulators and musical Cues never switch the deck; a hand does")
{
    azl::Composition show = twoDecks();
    // One that got past the checks: the engine still never switches on it.
    show.cues = {{.id = 1, .name = "Peak", .section = azl::Section::Drop, .deck = 2}};
    azl::EngineHarness h(kSize, kFps, {}, show);
    Client director(h.document());
    for (uint64_t column : {1, 2, 3, 11, 12})
        ::apply(director, azl::SetSlot{1, column, azl::ClipRef{.name = "solid", .effects = {azl::Effect{0, "solid", {}}}}});
    const uint64_t energy = ::apply(director, azl::AddMacro{std::nullopt, {.name = "Energy", .targets = {{azl::ParamRef{1, 0, "opacity"}}}}})["macro"]["id"];
    ::apply(director, azl::AddModulator{std::nullopt, {.name = "Wobble", .beats = 4.0, .target = {energy, {}}}});
    ::apply(director, azl::AddCue{std::nullopt, {.name = "Fire", .section = azl::Section::Drop, .column = azl::CueColumn{0, 1}, .every = 1}});
    ::apply(director, azl::AddCue{std::nullopt, {.name = "Bar 5", .bar = 5, .macros = {{energy, 0.5, 2}}, .column = azl::CueColumn{12, std::nullopt}}});
    ::apply(director, azl::SetLayerAutopilot{1, azl::AutopilotMode::Random, 1});
    ::apply(director, azl::SetColumnAutopilot{azl::AutopilotMode::Next, 2});
    for (auto [section, bar] : {std::pair{"drop", 3}, std::pair{"breakdown", 9}}) {
        director.send({{"type", "cue"}, {"op", "mark"}, {"section", section}, {"bar", bar}});
        REQUIRE(director.next()["op"] == "setTimeline");
    }

    // Twelve bars of 30 frames: the column Autopilot for six, then the layer's.
    for (int frame = 0; frame < 12 * 30; ++frame) {
        if (frame == 6 * 30)
            ::apply(director, azl::SetColumnAutopilot{azl::AutopilotMode::Off, {}});
        h.step();
        REQUIRE(shownDeck(h) == 1);
    }

    director.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 2}});
    while (director.next()["op"] != "selectDeck") {
    }
    CHECK(shownDeck(h) == 2);
}

TEST_CASE("gig: the Gig file round-trips, and saving it writes its lineup as Cues in one undo step")
{
    const azl::Gig gig = night();
    const nlohmann::json j = gig;
    CHECK(j == nlohmann::json::parse(R"({"schema": 1, "venue": "mini", "date": "2026-10-16",
        "window": {"start": "22:00", "end": "05:00"},
        "lineup": [{"dj": "Opener", "start": "23:00", "deck": 1}, {"dj": "Headliner", "start": "03:00", "deck": 2},
                   {"dj": "Closer", "start": "04:00", "deck": 1}]})"));
    CHECK(j.get<azl::Gig>() == gig);
    CHECK_THROWS_AS(nlohmann::json::parse(R"({"schema": 2, "date": "2026-10-16"})").get<azl::Gig>(), azl::ProtocolError);

    TempDir dir;
    const auto path = azl::gigPath(dir.path, "mini", gig.date);
    azl::saveGig(path, gig);
    CHECK(path == dir.path / "mini" / "2026-10-16.json");
    CHECK(azl::loadGig(path) == gig);
    CHECK(azl::venueGigs(dir.path, "mini").size() == 1);
    CHECK(azl::currentGig(azl::venueGigs(dir.path, "mini"), double(utc("2026-10-17", 2, 59)), cet));
    CHECK_FALSE(azl::currentGig(azl::venueGigs(dir.path, "mini"), double(utc("2026-10-17", 3)), cet));

    CHECK(azl::slotDate(gig, 23 * 60) == "2026-10-16");
    CHECK(azl::slotDate(gig, 3 * 60) == "2026-10-17");
    const std::vector<azl::Cue> cues = azl::lineupCues(gig);
    REQUIRE(cues.size() == 3);
    CHECK(cues[1] == azl::Cue{.name = "Headliner", .column = azl::CueColumn{0, 0},
                              .at = azl::CueAt{180, "2026-10-17"}, .deck = 2, .gig = "2026-10-16"});

    azl::EngineHarness h(kSize, kFps, {}, twoDecks());
    Client director(h.document());
    apply(director, azl::AddCue{std::nullopt, azl::Cue{.name = "Hand made", .section = azl::Section::Drop}});
    ::apply(director, azl::gigEdit(gig, h.document().document()));
    azl::Composition c = h.document().document();
    CHECK(c.cues.size() == 4);
    CHECK(c.gigs == std::vector<azl::GigWindow>{azl::gigWindow(gig)});

    // Shifting a slot re-saves it: its Cue moves, the hand-made one stays, nothing doubles.
    azl::Gig late = gig;
    azl::shiftSlot(late, 1, 15);
    CHECK(late.lineup[1].start == 3 * 60 + 15);
    CHECK(late.lineup[2].start == 4 * 60);
    azl::shiftSlot(late, 0, 70, true);
    CHECK(late.lineup[0].start == 10);
    CHECK(late.lineup[2].start == 5 * 60 + 10);
    CHECK_THROWS(azl::shiftSlot(late, 3, 5));
    late = gig;
    azl::shiftSlot(late, 1, 15);
    ::apply(director, azl::gigEdit(late, h.document().document()));
    c = h.document().document();
    REQUIRE(c.cues.size() == 4);
    CHECK(c.cues[0].name == "Hand made");
    CHECK(std::count_if(c.cues.begin(), c.cues.end(), [](const azl::Cue& q) { return q.gig == "2026-10-16"; }) == 3);
    auto headliner = std::find_if(c.cues.begin(), c.cues.end(), [](const azl::Cue& q) { return q.name == "Headliner"; });
    REQUIRE(headliner != c.cues.end());
    CHECK(headliner->at->minute == 3 * 60 + 15);

    // One undo step takes the whole save back.
    director.send(azl::msg::undo());
    director.next();
    c = h.document().document();
    headliner = std::find_if(c.cues.begin(), c.cues.end(), [](const azl::Cue& q) { return q.name == "Headliner"; });
    REQUIRE(headliner != c.cues.end());
    CHECK(headliner->at->minute == 3 * 60);
}

TEST_CASE("gig: the venue switches decks on the next bar after each slot's time, with the Director gone")
{
    azl::Composition saved;
    {
        azl::EngineHarness h(kSize, kFps, {}, twoDecks());
        h.setWallClock(double(utc("2026-10-16", 20)), cet);
        {
            Client director(h.document());
            ::apply(director, azl::gigEdit(night(), h.document().document()));
        }
        h.step();
        CHECK(shownDeck(h) == 1);

        // 02:59:57 local, summer time.
        h.setWallClock(double(utc("2026-10-17", 0, 59, 57)), cet);
        const std::optional<Firing> headliner = stepUntilDeck(h, 2, 8.0);
        REQUIRE(headliner);
        CHECK(headliner->wall >= double(utc("2026-10-17", 1)));
        CHECK(headliner->wall < double(utc("2026-10-17", 1)) + 2.0 / kFps);
        checkOnNextBar(*headliner);
        CHECK(h.document().document().layers[0].playing->column == 11);

        // Slot 3, shifted by 15 minutes, does not fire at 04:00 but at 04:15.
        azl::Gig late = night();
        azl::shiftSlot(late, 2, 15);
        {
            Client director(h.document());
            ::apply(director, azl::gigEdit(late, h.document().document()));
        }
        h.setWallClock(double(utc("2026-10-17", 1, 59, 58)), cet);
        CHECK_FALSE(stepUntilDeck(h, 1, 5.0));
        h.setWallClock(double(utc("2026-10-17", 2, 14, 58)), cet);
        const std::optional<Firing> closer = stepUntilDeck(h, 1, 8.0);
        REQUIRE(closer);
        CHECK(closer->wall >= double(utc("2026-10-17", 2, 15)));
        checkOnNextBar(*closer);
        CHECK(h.document().document().layers[0].playing->column == 1);
        saved = h.document().document();
    }

    // After a restart the Cues are still there: the headliner's switch fires again on a fresh engine.
    saved.deck = 1;
    saved.layers[0].playing.reset();
    {
        azl::EngineHarness h(kSize, kFps, {}, saved);
        h.setWallClock(double(utc("2026-10-17", 0, 59, 58)), cet);
        REQUIRE(stepUntilDeck(h, 2, 6.0));
    }
    // An engine that was down at 03:00 and is back a minute later catches the switch up, once.
    {
        azl::EngineHarness h(kSize, kFps, {}, saved);
        h.setWallClock(double(utc("2026-10-17", 1, 1)), cet);
        REQUIRE(stepUntilDeck(h, 2, 1.0));
    }
    saved.deck = 2;
    {
        azl::EngineHarness h(kSize, kFps, {}, saved);
        h.setWallClock(double(utc("2026-10-17", 1, 1)), cet);
        h.step(15);
        CHECK_FALSE(h.document().document().layers[0].playing);
    }
}

namespace {

// Every message the engine sends one client, read without waiting.
struct Inbox {
    explicit Inbox(azl::DocumentServer& server)
        : server(server), id(server.connect([this](const Message& m) {
              std::lock_guard lock(mutex);
              messages.push_back(m);
          }))
    {
        server.receive(id, msg::hello());
    }
    ~Inbox() { server.disconnect(id); }

    std::vector<Message> take()
    {
        std::lock_guard lock(mutex);
        return std::exchange(messages, {});
    }

    azl::DocumentServer& server;
    azl::ClientId id;
    std::mutex mutex;
    std::vector<Message> messages;
};

bool changes(const std::vector<Message>& messages, const char* op)
{
    return std::any_of(messages.begin(), messages.end(),
                       [&](const Message& m) { return m["type"] == "change" && m["op"] == op; });
}

} // namespace

TEST_CASE("gig: a timed slot whose deck a hand already showed does nothing, also after a restart")
{
    azl::Composition saved;
    {
        azl::EngineHarness h(kSize, kFps, {}, twoDecks());
        h.setWallClock(double(utc("2026-10-17", 0, 57)), cet);
        {
            Client director(h.document());
            ::apply(director, azl::gigEdit(night(), h.document().document()));
            director.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 2}});
            director.next();
            director.send({{"type", "trigger"}, {"op", "column"}, {"column", 12}});
            director.next();
        }
        h.step(int(kFps));
        CHECK(shownDeck(h) == 2);
        const azl::Play before = *h.document().document().layers[0].playing;
        CHECK(before.column == 12);

        Inbox inbox(h.document());
        inbox.take();
        const uint64_t change = h.document().change();
        h.setWallClock(double(utc("2026-10-17", 0, 59, 58)), cet);
        h.step(int(4 * kFps));
        const std::vector<Message> messages = inbox.take();
        CHECK(h.document().change() == change);
        CHECK_FALSE(changes(messages, "selectDeck"));
        CHECK_FALSE(changes(messages, "triggerColumn"));
        CHECK(h.document().document().layers[0].playing == before);
        auto skipped = std::find_if(messages.begin(), messages.end(),
                                    [](const Message& m) { return m["type"] == "cue" && m["op"] == "skipped"; });
        REQUIRE(skipped != messages.end());
        CHECK((*skipped)["name"] == "Headliner");
        CHECK((*skipped)["deck"] == 2);
        CHECK((*skipped)["reason"] == "deck 2 already live");

        // Later polls do not fire it either.
        h.step(int(2 * kFps));
        CHECK(h.document().change() == change);
        saved = h.document().document();
    }

    // A restart right after the skipped time does not fire it on start-up.
    {
        azl::EngineHarness h(kSize, kFps, {}, saved);
        const azl::Play before = *h.document().document().layers[0].playing;
        Inbox inbox(h.document());
        h.setWallClock(double(utc("2026-10-17", 1, 0, 30)), cet);
        h.step(int(2 * kFps));
        const std::vector<Message> messages = inbox.take();
        CHECK_FALSE(changes(messages, "selectDeck"));
        CHECK_FALSE(changes(messages, "triggerColumn"));
        CHECK(h.document().document().layers[0].playing == before);
    }

    // Without the hand, the slot switches as before: the deck at once, its first column on the next bar.
    {
        azl::EngineHarness h(kSize, kFps, {}, twoDecks());
        h.setWallClock(double(utc("2026-10-17", 0, 57)), cet);
        {
            Client director(h.document());
            ::apply(director, azl::gigEdit(night(), h.document().document()));
        }
        h.step();
        h.setWallClock(double(utc("2026-10-17", 0, 59, 58)), cet);
        const std::optional<Firing> headliner = stepUntilDeck(h, 2, 6.0);
        REQUIRE(headliner);
        checkOnNextBar(*headliner);
        CHECK(h.document().document().layers[0].playing->column == 11);
    }
}

TEST_CASE("gig: a slot past midnight fires on the next day only, and DST neither skips nor repeats a Cue")
{
    azl::Composition c = twoDecks();
    c.cues = azl::lineupCues({"mini", "2026-10-16", 22 * 60, 5 * 60, {{"Late", 30, 2}}});
    c.cues[0].id = 1;
    {
        azl::EngineHarness h(kSize, kFps, {}, c);
        h.setWallClock(double(utc("2026-10-15", 22, 29, 58)), cet);
        CHECK_FALSE(stepUntilDeck(h, 2, 4.0));
        h.setWallClock(double(utc("2026-10-16", 22, 29, 58)), cet);
        const std::optional<Firing> late = stepUntilDeck(h, 2, 6.0);
        REQUIRE(late);
        CHECK(late->wall >= double(utc("2026-10-16", 22, 30)));
        checkOnNextBar(*late);
    }

    c.cues = {azl::Cue{.id = 1, .name = "Daily", .column = azl::CueColumn{0, 0}, .at = azl::CueAt{150}, .deck = 2}};
    {
        // Spring: 02:30 never shows; the switch fires as the clock jumps from 02:00 to 03:00.
        azl::EngineHarness h(kSize, kFps, {}, c);
        h.setWallClock(double(utc("2026-03-29", 0, 59, 58)), cet);
        const std::optional<Firing> spring = stepUntilDeck(h, 2, 6.0);
        REQUIRE(spring);
        CHECK(spring->wall >= double(utc("2026-03-29", 1)));
        CHECK(spring->wall < double(utc("2026-03-29", 1)) + 0.5);
    }
    {
        // Autumn: 02:30 shows twice; it fires on the first only.
        azl::EngineHarness h(kSize, kFps, {}, c);
        h.setWallClock(double(utc("2026-10-25", 0, 29, 58)), cet);
        REQUIRE(stepUntilDeck(h, 2, 6.0));
        Client director(h.document());
        director.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 1}});
        director.next();
        h.setWallClock(double(utc("2026-10-25", 1, 29, 58)), cet);
        CHECK_FALSE(stepUntilDeck(h, 2, 5.0));
    }
}

TEST_CASE("gig: nocturne-ctl's cue at and gig commands parse")
{
    using Words = std::vector<std::string>;
    CHECK(azl::cueAtWords({"cue", "at", "03:00", "Headliner", "deck:2", "column:#1"}) ==
          azl::Cue{.name = "Headliner", .column = azl::CueColumn{0, 0}, .at = azl::CueAt{180}, .deck = 2});
    CHECK(azl::cueAtWords({"cue", "at", "00:30", "Late", "date:2026-10-17", "column:12"}) ==
          azl::Cue{.name = "Late", .column = azl::CueColumn{12, std::nullopt}, .at = azl::CueAt{30, "2026-10-17"}});
    CHECK_THROWS(azl::cueAtWords({"cue", "at", "3am", "X"}));
    CHECK_THROWS(azl::cueAtWords({"cue", "at", "03:00", "X", "macro:1=1"}));
    CHECK_THROWS(azl::cueAtWords({"cue", "at", "03:00", "X", "date:2026-10-32"}));

    CHECK(azl::gigCommandWords({"gig", "show"}) == azl::GigCommand{.op = "show"});
    CHECK(azl::gigCommandWords({"gig", "apply", "2026-10-16"}).date == "2026-10-16");
    const azl::GigCommand set =
        azl::gigCommandWords({"gig", "set", "2026-10-16", "22:00-05:00", "Opener@23:00=1", "Headliner@03:00=2"});
    CHECK(set.op == "set");
    CHECK(set.gig == azl::Gig{"", "2026-10-16", 22 * 60, 5 * 60, {{"Opener", 23 * 60, 1}, {"Headliner", 3 * 60, 2}}});
    const azl::GigCommand shift = azl::gigCommandWords({"gig", "shift", "2", "15", "following"});
    CHECK(shift.slot == 1);
    CHECK(shift.minutes == 15);
    CHECK(shift.following);
    CHECK(azl::gigCommandWords({"gig", "shift", "1", "-10", "2026-10-16"}).minutes == -10);
    for (const Words& bad : {Words{"gig"}, Words{"gig", "shift", "0", "5"}, Words{"gig", "shift", "1", "x"},
                             Words{"gig", "set", "2026-10-16", "22:00"}, Words{"gig", "set", "2026-10-16", "22:00-05:00", "Opener"},
                             Words{"gig", "show", "tonight"}, Words{"gig", "dance"}})
        CHECK_THROWS(azl::gigCommandWords(bad));
}
