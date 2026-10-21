#include <doctest.h>

#include "harness/engine_harness.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/bottom_panes.hpp"
#include "ui/app/look.hpp"

#include <functional>

namespace {

using azl::ui::CuesView;
using Part = CuesView::Part;

bool runUntil(azl::EngineHarness& harness, const std::function<bool()>& done, double seconds = 20.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if (done())
            return true;
    }
    return false;
}

void run(const azl::ui::Menu& menu, const juce::String& path)
{
    const azl::ui::MenuEntry* entry = azl::ui::menuEntry(menu, path);
    REQUIRE_MESSAGE(entry, "no menu entry " << path);
    REQUIRE(entry->enabled);
    REQUIRE(entry->run);
    entry->run();
}

void click(juce::Component& component, juce::Point<int> at)
{
    const juce::Time now = juce::Time::getCurrentTime();
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(),
                             juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component,
                             now, at.toFloat(), now, 1, false);
    component.mouseDown(e);
    component.mouseUp(e);
}

juce::String arrow()
{
    return juce::String::fromUTF8(" \xe2\x86\x92 ");
}

// Every level of the menu: its entry count, and no entry naming a deck and a column at once.
void checkLevels(const azl::ui::Menu& menu, int itemHeight, int screenHeight, int& levels)
{
    ++levels;
    CHECK(menu.size() <= CuesView::kMenuItems);
    CHECK(int(menu.size()) * itemHeight <= screenHeight);
    for (const azl::ui::MenuEntry& entry : menu) {
        CHECK_FALSE(entry.label.contains(" / "));
        if (!entry.sub.empty())
            checkLevels(entry.sub, itemHeight, screenHeight, levels);
    }
}

azl::Composition decks(int count, int columns)
{
    azl::Composition c;
    c.decks.clear();
    uint64_t id = 1;
    for (int d = 0; d < count; ++d) {
        azl::Deck deck{uint64_t(d + 1), "Deck " + std::to_string(d + 1), {}};
        for (int i = 0; i < columns; ++i, ++id)
            deck.columns.push_back({id, "Clip " + std::to_string(id)});
        c.decks.push_back(deck);
    }
    c.deck = 2;
    return c;
}

struct Engine {
    TempDir media;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    Client client{harness.document()};
    azl::WebSocketServer server{harness.document(), 0};
};

} // namespace

TEST_CASE("cues: the target menu goes by deck, this deck first, and fits the screen")
{
    std::optional<azl::CueColumn> picked;
    const auto pick = [&](const azl::CueColumn& c) { picked = c; };

    const azl::Composition small = decks(3, 4);
    const azl::ui::Menu menu = CuesView::columnMenu(small, azl::CueColumn{6, std::nullopt}, pick);
    REQUIRE(menu.size() == 5);
    CHECK(menu[0].label == CuesView::kThisDeck);
    CHECK(menu[0].sub.size() == 4);
    CHECK(!menu[1].run);
    CHECK(menu[1].sub.empty());
    CHECK(menu[2].label == "Deck 1");
    CHECK(menu[3].label == "Deck 2");
    CHECK(menu[3].ticked);
    REQUIRE(menu[3].sub.size() == 4);
    CHECK(menu[3].sub[0].label == "Clip 5");
    CHECK(menu[3].sub[1].ticked);
    CHECK(menu[0].sub[0].tip == "Now: Clip 5 of Deck 2");
    run(menu, "Deck 3/Clip 10");
    CHECK(picked == azl::CueColumn{10, std::nullopt});
    run(menu, juce::String(CuesView::kThisDeck) + "/Column 3");
    CHECK(picked == azl::CueColumn{0, 2});

    // Many decks of many columns: pages of submenus, never one level taller than the screen.
    azl::ui::Look look;
    int width = 0, itemHeight = 0;
    look.getIdealPopupMenuItemSize("Clip 1", false, -1, width, itemHeight);
    int screenHeight = 768;
    if (const juce::Displays::Display* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        screenHeight = std::min(screenHeight, display->userArea.getHeight());
    const azl::Composition big = decks(40, 60);
    const azl::ui::Menu large = CuesView::columnMenu(big, std::nullopt, pick);
    CHECK(large[0].label == CuesView::kThisDeck);
    int levels = 0;
    checkLevels(large, itemHeight, screenHeight, levels);
    CHECK(levels > 40);
    run(large, "Deck 25 " + juce::String::fromUTF8("\xe2\x80\x93") + " Deck 40/Deck 31/Clip 1801 " +
                   juce::String::fromUTF8("\xe2\x80\x93") + " Clip 1824/Clip 1810");
    CHECK(picked == azl::CueColumn{1810, std::nullopt});
}

TEST_CASE("cues: each cue reads as one sentence, when then do then on what")
{
    azl::Composition c;
    azl::Layer base;
    base.id = 1;
    base.name = "Base";
    c.layers = {base};
    c.macros = {azl::Macro{.id = 7, .name = "Energy"}};

    const std::pair<azl::Cue, juce::String> whens[] = {
        {azl::Cue{.section = azl::Section::Drop}, "When the Drop starts"},
        {azl::Cue{.section = azl::Section::Build, .bar = 9}, "On bar 9 of the Build"},
        {azl::Cue{.bar = 17}, "On bar 17 of the beat clock"},
    };
    using Fill = std::function<void(azl::Cue&)>;
    const std::pair<Fill, juce::String> dos[] = {
        {[](azl::Cue& q) { q.column = azl::CueColumn{2, std::nullopt}; }, "fire" + arrow() + "Column 2 of Deck 1"},
        {[](azl::Cue& q) { q.column = azl::CueColumn{0, 2}; }, "fire" + arrow() + "column 3 of this deck"},
        {[](azl::Cue& q) { q.autopilot = {{1, azl::AutopilotMode::Next, 4}}; }, "Autopilot Next every 4 bars" + arrow() + "Base"},
        {[](azl::Cue& q) { q.autopilot = {{1, azl::AutopilotMode::Off, std::nullopt}}; }, "Autopilot off" + arrow() + "Base"},
        {[](azl::Cue& q) { q.macros = {{7, 0.75, 2}}; }, "macro to 75% over 2 bars" + arrow() + "Energy"},
        {[](azl::Cue& q) { q.macros = {{7, 0.0, 0}}; }, "macro to 0%" + arrow() + "Energy"},
    };
    for (const auto& [when, whenText] : whens)
        for (const auto& [fill, doText] : dos) {
            azl::Cue cue = when;
            fill(cue);
            CHECK(CuesView::describe(c, cue) == whenText + arrow() + doText);
        }

    CHECK(CuesView::describe(c, azl::Cue{.section = azl::Section::Drop, .column = azl::CueColumn{0, 2}, .every = 16}) ==
          "When the Drop starts, every 16 bars" + arrow() + "fire" + arrow() + "column 3 of this deck");
    CHECK(CuesView::describe(c, azl::Cue{.bar = 5, .every = 1}).startsWith("On bar 5 of the beat clock, every 1 bar" + arrow()));

    azl::Cue both{.section = azl::Section::Drop, .macros = {{7, 1.0, 0}}, .column = azl::CueColumn{1, std::nullopt}};
    CHECK(CuesView::describe(c, both) ==
          "When the Drop starts" + arrow() + "fire" + arrow() + "Column 1 of Deck 1; macro to 100%" + arrow() + "Energy");
}

TEST_CASE("cues: the next firing at a clock, and the highlight while one fires")
{
    azl::Composition c;
    c.cues = {azl::Cue{.id = 1, .section = azl::Section::Drop, .bar = 9, .column = azl::CueColumn{0, 0}},
              azl::Cue{.id = 2, .section = azl::Section::Build, .column = azl::CueColumn{0, 0}},
              azl::Cue{.id = 3, .bar = 17, .column = azl::CueColumn{0, 0}},
              azl::Cue{.id = 4, .section = azl::Section::Drop, .column = azl::CueColumn{0, 0}}};
    c.timeline.sections = {{azl::Section::Drop, 0.0}};
    CHECK(CuesView::nextText(c, c.cues[0], 2.0) == "next: in 8 bars");
    CHECK(CuesView::nextText(c, c.cues[0], 31.0) == "next: in 1 bar");
    CHECK(CuesView::nextText(c, c.cues[1], 2.0) == "waits for the Build");
    CHECK(CuesView::nextText(c, c.cues[2], 10.0) == "next: in 14 bars");
    CHECK(CuesView::nextText(c, c.cues[2], 70.0) == "fired");
    CHECK(CuesView::nextText(c, c.cues[3], 2.0) == "fired");
    c.timeline.sections.push_back({azl::Section::Build, 48.0});
    CHECK(CuesView::nextText(c, c.cues[1], 40.0) == "next: in 2 bars");

    c.cues[2].every = 16;
    CHECK(CuesView::nextText(c, c.cues[2], 70.0) == "next: in 15 bars");
    c.cues[3].every = 4;
    CHECK(CuesView::nextText(c, c.cues[3], 2.0) == "next: in 4 bars");
    CHECK(CuesView::firing(c, c.cues[3], 17.0));
    c.cues[3].enabled = false;
    CHECK(CuesView::nextText(c, c.cues[3], 2.0) == "off");
    CHECK_FALSE(CuesView::firing(c, c.cues[3], 0.0));
    c.cues[3] = {.id = 4, .section = azl::Section::Drop, .column = azl::CueColumn{0, 0}};

    CHECK(CuesView::firing(c, c.cues[3], 0.0));
    CHECK(CuesView::firing(c, c.cues[3], 3.5));
    CHECK_FALSE(CuesView::firing(c, c.cues[3], 4.5));
    CHECK_FALSE(CuesView::firing(c, c.cues[0], 31.0));
    CHECK(CuesView::firing(c, c.cues[0], 33.0));
}

TEST_CASE("cues: the tab lists sentences, lights the firing cue, and its builder edits in place")
{
    Engine e;
    const auto columns = e.harness.document().document().decks[0].columns;
    apply(e.client, azl::AddMacro{std::nullopt, azl::Macro{.name = "Energy"}});
    apply(e.client, azl::AddDeck{std::nullopt, azl::Deck{0, "Peak", {{0, "Strobe wall"}, {0, "Tunnel"}}}, {}});
    apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.section = azl::Section::Drop, .column = azl::CueColumn{0, 0}}});
    apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.section = azl::Section::Build, .bar = 9,
                                                       .autopilot = {{1, azl::AutopilotMode::Next, 4}}}});
    apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.section = azl::Section::Drop, .bar = 17,
                                                       .column = azl::CueColumn{columns[2].id, std::nullopt}}});
    e.client.send({{"type", "cue"}, {"op", "mark"}, {"section", "drop"}});
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MidPane mid(session);
    mid.setSize(440, 300);
    mid.showTab(2);
    CuesView& cues = mid.cues();
    REQUIRE(runUntil(e.harness, [&] {
        return session.client().synced() && session.composition().cues.size() == 3 &&
               !session.composition().timeline.sections.empty();
    }));
    const double drop = session.composition().timeline.sections[0].beat;
    double beat = drop + 2.0;
    cues.setClock([&] { return beat; });

    const azl::ui::Palette& p = azl::ui::Look::palette();
    auto rowColour = [&](size_t index) {
        const juce::Image image = cues.createComponentSnapshot(cues.getLocalBounds(), true, 1.0f);
        return image.getPixelAt(cues.rowArea(index).getX() + 2, cues.rowArea(index).getCentreY());
    };
    CHECK(rowColour(0) == p.liveRow);
    CHECK(rowColour(1) == p.cell);
    beat = drop + 6.0;
    CHECK(rowColour(0) == p.cell);
    beat = drop + 65.0;
    CHECK(rowColour(2) == p.liveRow);
    beat = drop + 2.0;

    // Clicking a cue opens the builder under it; each pick is one undo step on that cue.
    const azl::Cue first = session.composition().cues[0];
    cues.open(first.id);
    REQUIRE(cues.draft());
    CHECK(cues.builderArea().getY() > cues.rowArea(0).getBottom());
    CHECK(cues.rowArea(1).getY() > cues.builderArea().getBottom());
    CHECK(cues.pickLabel(Part::When) == "the Drop");
    CHECK(cues.pickLabel(Part::Bar) == "as it starts");
    CHECK(cues.pickLabel(Part::Target) == "column 1 of this deck");
    CHECK_FALSE(cues.shows(Part::Value));
    run(cues.pickMenu(Part::Target), "Peak/Tunnel");
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].column != first.column; }));
    CHECK(cues.pickLabel(Part::Target) == "Tunnel of Peak");
    const size_t steps = session.pastEdits().size();
    run(cues.pickMenu(Part::Bar), "Bar 9");
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].bar == 9; }));
    REQUIRE(runUntil(e.harness, [&] { return session.pastEdits().size() == steps + 1; }));
    session.client().undo();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].bar == 1; }));
    CHECK(cues.pickLabel(Part::Bar) == "as it starts");
    cues.doneButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return !cues.draft(); }));

    // A new cue waits for Add.
    cues.addButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return cues.draft().has_value(); }));
    CHECK_FALSE(cues.addButton().isVisible());
    run(cues.pickMenu(Part::Do), "Move a macro");
    run(cues.pickMenu(Part::Value), "to 75%");
    CHECK(cues.shows(Part::Ramp));
    runUntil(e.harness, [] { return false; }, 0.3);
    CHECK(session.composition().cues.size() == 3);
    cues.doneButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues.size() == 4; }));
    CHECK(session.composition().cues[3].macros == std::vector<azl::CueMacro>{{session.composition().macros[0].id, 0.75, 0}});

    // The builder's WHEN part offers a repeat, one undo step like every pick.
    cues.open(session.composition().cues[0].id);
    REQUIRE(cues.shows(Part::Repeat));
    CHECK(cues.pickLabel(Part::Repeat) == "once");
    run(cues.pickMenu(Part::Repeat), "Every 16 bars");
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].every == 16; }));
    CHECK(cues.pickLabel(Part::Repeat) == "every 16 bars");
    CHECK(CuesView::nextText(session.composition(), session.composition().cues[0], drop + 2.0) == "next: in 16 bars");
    cues.close();

    // The list's toggle switches a cue off and on, each one undo step.
    const size_t before = session.pastEdits().size();
    click(cues, cues.toggleArea(1).getCentre());
    REQUIRE(runUntil(e.harness, [&] { return !session.composition().cues[1].enabled; }));
    REQUIRE(runUntil(e.harness, [&] { return session.pastEdits().size() == before + 1; }));
    CHECK(!cues.draft());
    CHECK(cues.tipAt(cues.toggleArea(1).getCentre()) == "Switch the cue on");
    session.client().undo();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[1].enabled; }));
    click(cues, cues.toggleArea(1).getCentre());
    REQUIRE(runUntil(e.harness, [&] { return !session.composition().cues[1].enabled; }));

    cues.open(session.composition().cues[1].id);
    const juce::Image shot = mid.createComponentSnapshot(mid.getLocalBounds(), true, 1.0f);
    const auto dir = fs::path(NOCTURNE_UI_SHOTS);
    fs::create_directories(dir);
    juce::File file(juce::String((dir / "97-cues-tab.png").string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(shot, out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

TEST_CASE("cues: the builder steps an effect: its Autopilot or a move to one of its steps")
{
    Engine e;
    azl::Effect stripper{0, "classicStripper", {}};
    stripper.steps = {{"wide", {{"globalWidth", 1.0}}}, {"narrow", {{"globalWidth", 0.2}}}};
    apply(e.client, azl::AddEffect{1, std::nullopt, stripper});
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MidPane mid(session);
    mid.setSize(440, 300);
    mid.showTab(2);
    CuesView& cues = mid.cues();
    REQUIRE(runUntil(e.harness, [&] {
        return session.client().synced() && !session.composition().layers[0].effects.empty();
    }));
    const uint64_t id = session.composition().layers[0].effects[0].id;

    cues.addButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return cues.draft().has_value(); }));
    run(cues.pickMenu(Part::Do), "Step an effect");
    CHECK(cues.pickLabel(Part::Value) == "Next every 4 bars");
    CHECK(cues.pickLabel(Part::Target).endsWith("classicStripper"));
    run(cues.pickMenu(Part::Value), "Go to step/Step 2  narrow");
    CHECK(cues.pickLabel(Part::Value) == "to step 2");
    cues.doneButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues.size() == 1; }));
    const azl::Cue cue = session.composition().cues[0];
    CHECK(cue.effects == std::vector<azl::CueEffect>{{id, std::nullopt, std::nullopt, size_t(1)}});
    CHECK(CuesView::describe(session.composition(), cue).contains("effect steps to step 2"));
}

TEST_CASE("cues: lineup Cues read as a time of day and a deck switch")
{
    azl::Composition c;
    c.decks.push_back({2, "Headliner", {{11, "H 1"}}});
    const azl::Cue lineup{.name = "Headliner", .column = azl::CueColumn{0, 0}, .at = azl::CueAt{180, "2026-10-17"},
                          .deck = 2, .gig = "2026-10-16"};
    CHECK(CuesView::describe(c, lineup) ==
          "At 03:00 (Headliner)" + arrow() + "show" + arrow() + "Headliner; fire" + arrow() + "column 1 of this deck");
    CHECK(CuesView::nextText(c, lineup, 0.0) == "at 03:00");
    const azl::Cue dated{.name = "Lights", .at = azl::CueAt{30, "2026-10-17"}, .deck = 1};
    CHECK(CuesView::describe(c, dated) == "At 00:30 on 2026-10-17 (Lights)" + arrow() + "show" + arrow() + "Deck 1");
    azl::Cue off = lineup;
    off.enabled = false;
    CHECK(CuesView::nextText(c, off, 0.0) == "off");
}

TEST_CASE("cues: a Gig's lineup Cues sit under its header, and its editor shifts a slot in one action")
{
    Engine e;
    TempDir gigs;
    apply(e.client, azl::AddDeck{std::nullopt, azl::Deck{0, "Headliner", {{0, "H 1"}}}, {}});
    apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.name = "Drop", .section = azl::Section::Drop,
                                                       .column = azl::CueColumn{0, 0}}});
    const uint64_t headlinerDeck = e.harness.document().document().decks[1].id;
    const azl::Gig gig{"127.0.0.1", "2026-10-16", 22 * 60, 5 * 60,
                       {{"Opener", 23 * 60, 1}, {"Headliner", 3 * 60, headlinerDeck}}};
    azl::saveGig(azl::gigPath(gigs.path, gig.venue, gig.date), gig);
    ::apply(e.client, azl::gigEdit(gig, e.harness.document().document()));
    apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.name = "Build", .section = azl::Section::Build}});

    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MidPane mid(session);
    mid.setSize(440, 300);
    mid.showTab(2);
    CuesView& cues = mid.cues();
    cues.gigEditor().setFolder(gigs.path);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && session.composition().cues.size() == 4; }));

    // Hand-made first (Drop, Build), then the Gig's header and its lineup.
    CHECK(session.composition().cues[3].name == "Build");
    CHECK(cues.rowArea(0).getY() < cues.rowArea(3).getY());
    CHECK(cues.rowArea(3).getY() < cues.gigArea("2026-10-16").getY());
    CHECK(cues.gigArea("2026-10-16").getY() < cues.rowArea(1).getY());
    CHECK(cues.rowArea(1).getY() < cues.rowArea(2).getY());
    CHECK(cues.gigArea("2026-10-17").isEmpty());
    CHECK(cues.tipAt(cues.nextArea(2).getCentre()).contains("venue's clock"));

    click(cues, cues.gigArea("2026-10-16").getCentre());
    REQUIRE(cues.showsGig());
    azl::ui::GigEditor& editor = cues.gigEditor();
    CHECK(editor.gig() == gig);
    REQUIRE(editor.rowCount() == 2);
    CHECK(editor.row(1).start.getText() == "03:00");
    CHECK(editor.row(1).deck.getButtonText() == "Headliner");

    const size_t steps = session.pastEdits().size();
    editor.shift(1, 5);
    CHECK(editor.problem().isEmpty());
    REQUIRE(runUntil(e.harness, [&] {
        const auto& q = session.composition().cues;
        return std::any_of(q.begin(), q.end(), [](const azl::Cue& c) { return c.name == "Headliner" && c.at->minute == 185; });
    }));
    REQUIRE(runUntil(e.harness, [&] { return session.pastEdits().size() == steps + 1; }));
    CHECK(session.composition().cues.size() == 4);
    CHECK(azl::loadGig(azl::gigPath(gigs.path, gig.venue, gig.date)).lineup[1].start == 185);
    CHECK(editor.row(1).start.getText() == "03:05");

    // A bad time stops the save with a reason; nothing is sent.
    editor.row(0).start.setText("11pm");
    CHECK_FALSE(editor.save());
    CHECK(editor.problem().contains("Slot 1"));
    editor.closeButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return !cues.showsGig(); }));
    cues.gigButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return cues.showsGig(); }));
    CHECK(cues.gigEditor().gig().venue == "127.0.0.1");
}

TEST_CASE("cues: the builder edits a time-of-day cue in place: its time, a deck switch and its column")
{
    Engine e;
    apply(e.client, azl::AddDeck{std::nullopt, azl::Deck{0, "Peak", {{0, "Strobe wall"}, {0, "Tunnel"}}}, {}});
    apply(e.client, azl::AddMacro{std::nullopt, azl::Macro{.name = "Energy"}});
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MidPane mid(session);
    mid.setSize(440, 300);
    mid.showTab(2);
    CuesView& cues = mid.cues();
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && session.composition().decks.size() == 2; }));
    const uint64_t peak = session.composition().decks[1].id;

    // Neither a Section cue nor a beat clock cue offers a deck switch.
    cues.addButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return cues.draft().has_value(); }));
    CHECK_FALSE(cues.shows(Part::Time));
    const azl::ui::Menu sectionDo = cues.pickMenu(Part::Do);
    CHECK_FALSE(azl::ui::menuEntry(sectionDo, "Show a deck"));
    CHECK(azl::ui::menuEntry(sectionDo, "Fire a column"));
    run(cues.pickMenu(Part::When), "The beat clock");
    CHECK_FALSE(azl::ui::menuEntry(cues.pickMenu(Part::Do), "Show a deck"));

    // A time of day: Bar and Repeat give way to the time, and only a deck switch or a column remain.
    run(cues.pickMenu(Part::When), "A time of day");
    REQUIRE(cues.draft()->at);
    CHECK(cues.draft()->name.empty());
    CHECK(cues.pickLabel(Part::When) == "the clock reads");
    CHECK(cues.shows(Part::Time));
    CHECK_FALSE(cues.shows(Part::Bar));
    CHECK_FALSE(cues.shows(Part::Repeat));
    const azl::ui::Menu clockDo = cues.pickMenu(Part::Do);
    REQUIRE(azl::ui::menuEntry(clockDo, "Move a macro"));
    CHECK_FALSE(azl::ui::menuEntry(clockDo, "Move a macro")->enabled);
    const azl::ui::Menu hours = cues.pickMenu(Part::Time);
    CHECK(hours.size() == 24);
    for (const azl::ui::MenuEntry& hour : hours)
        CHECK(hour.sub.size() == 12);
    run(hours, "03:00/03:15");
    CHECK(cues.pickLabel(Part::Time) == "03:15");
    run(cues.pickMenu(Part::Do), "Show a deck");
    CHECK(cues.shows(Part::Also));
    CHECK_FALSE(cues.shows(Part::Value));
    run(cues.pickMenu(Part::Target), "Peak");
    CHECK(cues.pickLabel(Part::Target) == "Peak");
    CHECK(cues.pickLabel(Part::Also) == "and fire column 1 of this deck");
    const azl::ui::Menu also = cues.pickMenu(Part::Also);
    CHECK(also[0].label == CuesView::kThisDeck);
    CHECK(also.back().label == "Nothing");
    CHECK(also.size() <= CuesView::kMenuItems);
    run(also, "Peak/Tunnel");
    CHECK(CuesView::describe(session.composition(), *cues.draft()) ==
          "At 03:15" + arrow() + "show" + arrow() + "Peak; fire" + arrow() + "Tunnel of Peak");
    cues.doneButton().triggerClick();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues.size() == 1; }));
    const azl::Cue added = session.composition().cues[0];
    CHECK(added.at == azl::CueAt{195});
    CHECK(added.deck == peak);
    CHECK(added.gig.empty());

    // A click opens it again; each pick is one undo step, and back to a Section drops the deck switch.
    click(cues, cues.rowArea(0).getCentre());
    REQUIRE(cues.draft());
    CHECK(cues.tipAt(cues.rowArea(0).getCentre()).endsWith("Click to edit it in place"));
    const size_t steps = session.pastEdits().size();
    run(cues.pickMenu(Part::Time), "04:00/04:30");
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].at->minute == 270; }));
    REQUIRE(runUntil(e.harness, [&] { return session.pastEdits().size() == steps + 1; }));
    CHECK(CuesView::nextText(session.composition(), session.composition().cues[0], 0.0) == "at 04:30");

    const juce::Image shot = mid.createComponentSnapshot(mid.getLocalBounds(), true, 1.0f);
    const auto dir = fs::path(NOCTURNE_UI_SHOTS);
    fs::create_directories(dir);
    juce::File file(juce::String((dir / "97-cues-time-of-day.png").string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(shot, out));
    MESSAGE("screenshot: " << file.getFullPathName());

    run(cues.pickMenu(Part::Do), "Fire a column");
    REQUIRE(runUntil(e.harness, [&] { return !session.composition().cues[0].deck; }));
    run(cues.pickMenu(Part::When), "Drop");
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].section == azl::Section::Drop; }));
    CHECK_FALSE(session.composition().cues[0].at);
    CHECK(session.composition().cues[0].column == azl::CueColumn{session.composition().decks[1].columns[1].id, std::nullopt});
    CHECK(cues.shows(Part::Bar));
    session.client().undo();
    REQUIRE(runUntil(e.harness, [&] { return session.composition().cues[0].at.has_value(); }));
}

TEST_CASE("cues: the night's deck switches list under its Gig header, collapsed, with their cause")
{
    Engine e;
    apply(e.client, azl::AddDeck{std::nullopt, azl::Deck{0, "Headliner", {{0, "H 1"}}}, {}});
    const uint64_t headlinerDeck = e.harness.document().document().decks[1].id;
    const azl::Gig gig{"127.0.0.1", "2026-10-16", 22 * 60, 5 * 60,
                       {{"Opener", 23 * 60, 1}, {"Headliner", 3 * 60, headlinerDeck}}};
    // 01:00 on the night, venue time UTC+2.
    e.harness.setWallClock(double(*azl::dateDays("2026-10-16")) * 86400 + 23 * 3600, [](int64_t) { return 7200; });
    ::apply(e.client, azl::gigEdit(gig, e.harness.document().document()));
    e.client.send({{"type", "trigger"}, {"op", "deck"}, {"deck", headlinerDeck}, {"by", "ui"}});
    e.client.next();
    e.harness.switchLog().waitIdle();

    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MidPane mid(session);
    mid.setSize(440, 300);
    mid.showTab(2);
    CuesView& cues = mid.cues();
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && !cues.switchesArea().isEmpty(); }));
    CHECK_FALSE(cues.switchesOpen());
    CHECK(cues.shownSwitches().empty());
    CHECK(cues.gigArea("2026-10-16").getY() < cues.switchesArea().getY());
    CHECK(cues.tipAt(cues.switchesArea().getCentre()).contains("deck switch"));

    click(cues, cues.switchesArea().getCentre());
    CHECK(cues.switchesOpen());
    REQUIRE(cues.shownSwitches().size() == 1);
    CHECK(cues.shownSwitches()[0] == "01:00:00  Headliner  slot 2 Headliner  by ui");

    // A switch made while the list is open joins it.
    e.client.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 1}, {"by", "ctl"}});
    REQUIRE(runUntil(e.harness, [&] { return cues.shownSwitches().size() == 2; }));
    CHECK(cues.shownSwitches()[1].endsWith("slot 1 Opener  by ctl"));
    click(cues, cues.switchesArea().getCentre());
    CHECK(cues.shownSwitches().empty());
}
