#include "document/cue_edits.hpp"
#include "document/lineup.hpp"
#include "harness/engine_harness.hpp"
#include "test_client.hpp"
#include "venues/gig.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

constexpr int64_t kHour = 3600;
constexpr azl::EngineConfig kSize{.width = 320, .height = 180};
constexpr double kFps = 15.0;

int64_t utc(const char* date, int hour, int minute = 0, int second = 0)
{
    return *azl::dateDays(date) * 86400 + hour * kHour + minute * 60 + second;
}

int cest(int64_t) { return int(2 * kHour); }

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

void switchDeck(azl::DocumentServer& server, uint64_t deck, const char* by)
{
    Client hand(server);
    hand.send({{"type", "trigger"}, {"op", "deck"}, {"deck", deck}, {"by", by}});
    Message m = hand.next();
    while (m["type"] == "switches")
        m = hand.next();
    REQUIRE(m["op"] == "selectDeck");
}

// Captures what the process prints while it lives.
class Stdout {
public:
    Stdout() : saved_(dup(STDOUT_FILENO))
    {
        std::fflush(stdout);
        FILE* f = std::freopen(path_.c_str(), "w", stdout);
        REQUIRE(f);
    }
    std::string text()
    {
        std::fflush(stdout);
        std::ifstream in(path_);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
    ~Stdout()
    {
        std::fflush(stdout);
        dup2(saved_, STDOUT_FILENO);
        close(saved_);
        fs::remove(path_);
    }

private:
    int saved_;
    fs::path path_ = fs::temp_directory_path() / ("nocturne-stdout-" + std::to_string(getpid()) + ".txt");
};

} // namespace

TEST_CASE("switches: lineup slots, the night and ISO times from the composition")
{
    azl::Composition c = twoDecks();
    c.gigs = {azl::gigWindow(night())};
    c.cues = azl::lineupCues(night());
    for (size_t i = 0; i < c.cues.size(); ++i)
        c.cues[i].id = i + 1;
    c.cues.push_back(azl::Cue{.id = 9, .name = "Hand made", .section = azl::Section::Drop});

    CHECK_FALSE(azl::nightAt(c, double(utc("2026-10-16", 19, 59)), cest));
    const std::optional<azl::Night> n = azl::nightAt(c, double(utc("2026-10-16", 23)), cest);
    REQUIRE(n);
    CHECK(n->start == utc("2026-10-16", 20));
    CHECK(n->end == utc("2026-10-17", 3));
    REQUIRE(n->lineup.size() == 3);
    CHECK(n->lineup[1] == azl::LineupSlot{2, "Headliner", utc("2026-10-17", 1), 2, 2});

    // A deck in two slots: the latest started at or before the time, else the earliest.
    CHECK(azl::slotFor(*n, 1, double(utc("2026-10-16", 20, 30)))->slot == 1);
    CHECK(azl::slotFor(*n, 1, double(utc("2026-10-17", 1, 30)))->slot == 1);
    CHECK(azl::slotFor(*n, 1, double(utc("2026-10-17", 2, 30)))->slot == 3);
    CHECK(azl::slotFor(*n, 2, double(utc("2026-10-16", 21)))->slot == 2);
    CHECK_FALSE(azl::slotFor(*n, 7, double(utc("2026-10-16", 23))));

    // Tonight: the window holding now, else the next within 12 hours.
    CHECK(azl::tonight(c, double(utc("2026-10-16", 8, 1)), cest));
    CHECK_FALSE(azl::tonight(c, double(utc("2026-10-16", 7, 59)), cest));
    CHECK_FALSE(azl::tonight(c, double(utc("2026-10-17", 3)), cest));

    CHECK(azl::nightDate(c, double(utc("2026-10-16", 23)), cest) == "2026-10-16");
    CHECK(azl::nightDate(c, double(utc("2026-10-17", 10)), cest) == "2026-10-17");
    CHECK(azl::isoTime(double(utc("2026-10-17", 1)) + 0.25) == "2026-10-17T01:00:00.250Z");
    CHECK(azl::isoTime(double(utc("2026-10-16", 23, 30)), 2 * 3600) == "2026-10-17T01:30:00.000+02:00");
    CHECK(azl::isoTime(double(utc("2026-10-16", 23, 30)), -5 * 3600 - 1800) == "2026-10-16T18:00:00.000-05:30");

    CHECK(azl::switchText(Message::parse(R"({"utc": "2026-10-17T01:00:00.250Z", "local": "2026-10-17T03:00:00.250+02:00",
        "deck": 2, "deckName": "Peak", "dj": "Headliner", "slot": 2, "cause": "cue",
        "recording": {"file": "/m/night.mov", "mediaTime": 12.5}})")) ==
          "03:00:00  deck 2 Peak  slot 2 Headliner  cue  at 12.500 s in /m/night.mov");
    CHECK(azl::switchText(Message::parse(R"({"local": "2026-10-17T12:00:00.000+02:00", "deck": 1, "deckName": "",
        "dj": null, "slot": null, "cause": "ctl", "recording": null})")) == "12:00:00  deck 1  ctl");
}

TEST_CASE("switches: a wall Cue, the UI and ctl each log one line with its cause, DJ, slot and recording time")
{
    TempDir dir;
    azl::EngineHarness h(kSize, kFps, {}, twoDecks());
    h.setWallClock(double(utc("2026-10-17", 0, 58)), cest);
    Client watcher(h.document());
    watcher.send({{"type", "switches"}, {"op", "list"}, {"watch", true}});
    REQUIRE(watcher.next()["switches"].empty());
    {
        Client director(h.document());
        ::apply(director, azl::gigEdit(night(), h.document().document()));
        director.send({{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", dir.path.string()}});
        for (Message m = director.next(); !(m["type"] == "record" && m["op"] == "start"); m = director.next())
            REQUIRE(m["type"] != "error");
    }
    const double origin = h.time();
    h.step(int(kFps));

    const double byUi = h.time();
    switchDeck(h.document(), 2, "ui");
    h.step(int(kFps));
    const double byCtl = h.time();
    switchDeck(h.document(), 1, "ctl");
    h.step(int(kFps));
    // Switching to the deck already shown logs nothing.
    switchDeck(h.document(), 1, "ctl");

    h.setWallClock(double(utc("2026-10-17", 0, 59, 59)), cest);
    double byCue = 0.0;
    for (int i = 0; i < int(3 * kFps) && h.document().document().deck != 2; ++i) {
        byCue = h.time();
        h.step();
    }
    REQUIRE(h.document().document().deck == 2);
    h.step(int(kFps));
    h.recorder().waitUntilWritten();
    h.switchLog().waitIdle();

    const fs::path file = dir.path / "Sets" / "2026-10-16" / "switches.jsonl";
    const std::vector<Message> lines = azl::SwitchLog::read(file);
    REQUIRE(lines.size() == 3);
    const auto check = [&](const Message& line, const char* cause, uint64_t deck, const char* dj, int slot, double at) {
        CAPTURE(line.dump());
        CHECK(line["cause"] == cause);
        CHECK(line["deck"] == deck);
        CHECK(line["dj"] == dj);
        CHECK(line["slot"] == slot);
        REQUIRE(line["recording"].is_object());
        CHECK(fs::path(line["recording"]["file"].get<std::string>()).parent_path() == dir.path);
        CHECK(std::abs(line["recording"]["mediaTime"].get<double>() - (at - origin)) < 1.0 / kFps);
    };
    check(lines[0], "ui", 2, "Headliner", 2, byUi);
    check(lines[1], "ctl", 1, "Opener", 1, byCtl);
    check(lines[2], "cue", 2, "Headliner", 2, byCue);
    CHECK(lines[0]["deckName"] == "Headliner");
    CHECK(lines[2]["local"].get<std::string>().starts_with("2026-10-17T03:00:00"));
    CHECK(lines[2]["utc"].get<std::string>().starts_with("2026-10-17T01:00:00"));

    // The protocol lists the night's log, and every line written was broadcast.
    watcher.send({{"type", "switches"}, {"op", "list"}});
    int added = 0;
    for (Message m = watcher.next();; m = watcher.next()) {
        if (m["type"] == "switches" && m["op"] == "added") {
            CHECK(m["date"] == "2026-10-16");
            ++added;
        }
        if (m["type"] == "switches" && m["op"] == "list") {
            CHECK(m["date"] == "2026-10-16");
            CHECK(m["switches"] == Message(lines));
            break;
        }
    }
    CHECK(added == 3);
    watcher.send({{"type", "switches"}, {"op", "list"}, {"date", "2026-10-15"}});
    for (Message m = watcher.next();; m = watcher.next())
        if (m["type"] == "switches" && m["op"] == "list") {
            CHECK(m["switches"].empty());
            break;
        }
    watcher.send({{"type", "switches"}, {"op", "list"}, {"date", "yesterday"}});
    for (Message m = watcher.next();; m = watcher.next())
        if (m["type"] == "error")
            break;
    Client bad(h.document());
    bad.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 1}, {"by", "desk"}});
    CHECK(bad.next()["type"] == "error");
}

TEST_CASE("switches: a night past midnight logs into its start date's folder, with no recording")
{
    TempDir dir;
    azl::EngineHarness h(kSize, kFps, dir.path, twoDecks());
    h.setWallClock(double(utc("2026-10-16", 23)), cest);
    {
        Client director(h.document());
        ::apply(director, azl::gigEdit(night(), h.document().document()));
    }
    switchDeck(h.document(), 2, "ui");
    h.setWallClock(double(utc("2026-10-17", 10)), cest);
    switchDeck(h.document(), 1, "ui");
    h.switchLog().waitIdle();

    const std::vector<Message> night = azl::SwitchLog::read(dir.path / "Sets" / "2026-10-16" / "switches.jsonl");
    REQUIRE(night.size() == 1);
    CHECK(night[0]["local"].get<std::string>().starts_with("2026-10-17T01:00:00"));
    CHECK(night[0]["recording"].is_null());
    const std::vector<Message> day = azl::SwitchLog::read(dir.path / "Sets" / "2026-10-17" / "switches.jsonl");
    REQUIRE(day.size() == 1);
    CHECK(day[0]["dj"].is_null());
    CHECK(day[0]["slot"].is_null());
}

TEST_CASE("switches: an unwritable folder is logged once and the deck still switches")
{
    TempDir dir;
    const fs::path locked = dir.path / "locked";
    fs::create_directories(locked);
    azl::EngineHarness h(kSize, kFps, {}, twoDecks());
    {
        Client director(h.document());
        director.send({{"type", "record"}, {"op", "set"}, {"path", (locked / "night.mov").string()}});
        director.next();
    }
    REQUIRE(chmod(locked.c_str(), 0500) == 0);
    std::string printed;
    {
        Stdout out;
        for (uint64_t deck : {2, 1, 2}) {
            switchDeck(h.document(), deck, "ctl");
            CHECK(h.document().document().deck == deck);
        }
        h.switchLog().waitIdle();
        printed = out.text();
    }
    chmod(locked.c_str(), 0700);
    CHECK_FALSE(fs::exists(locked / "Sets"));
    size_t count = 0;
    for (size_t at = printed.find("switches: cannot write"); at != std::string::npos;
         at = printed.find("switches: cannot write", at + 1))
        ++count;
    CHECK(count == 1);
}
