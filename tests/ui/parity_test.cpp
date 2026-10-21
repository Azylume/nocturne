#include "document/composition_file.hpp"
#include "document/wall_clock.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <functional>
#include <thread>

namespace {

using DragSource = juce::DragAndDropTarget::SourceDetails;

fs::path shotsDir()
{
    const char* dir = std::getenv("NOCTURNE_UI_SHOTS");
    return dir ? dir : NOCTURNE_UI_SHOTS;
}

void shoot(juce::Component& window, const std::string& name)
{
    fs::create_directories(shotsDir());
    juce::File file(juce::String((shotsDir() / name).string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f),
                                                      out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

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

void pump(azl::EngineHarness& harness, int ms)
{
    runUntil(harness, [] { return false; }, ms / 1000.0);
}

juce::MouseEvent mouse(juce::Component& component, juce::Point<int> at, juce::Point<int> down,
                       juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    const juce::Time now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, &component, &component, now, down.toFloat(), now, 1, at != down);
}

void drag(azl::EngineHarness& harness, juce::Component& component, juce::Point<int> from, juce::Point<int> to,
          juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    component.mouseDown(mouse(component, from, from, mods));
    for (int i = 1; i <= 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        component.mouseDrag(mouse(component, from + (to - from) * i / 10, from, mods));
    }
    component.mouseUp(mouse(component, to, from, mods));
}

void click(juce::Component& component, juce::Point<int> at)
{
    component.mouseDown(mouse(component, at, at));
    component.mouseUp(mouse(component, at, at));
}

void wheel(juce::Component& component, float deltaY)
{
    juce::MouseWheelDetails details{};
    details.deltaY = deltaY;
    component.mouseWheelMove(mouse(component, {10, 5}, {10, 5}, {}), details);
}

void run(const azl::ui::Menu& menu, const juce::String& path)
{
    const azl::ui::MenuEntry* entry = azl::ui::menuEntry(menu, path);
    REQUIRE_MESSAGE(entry, "no menu entry " << path);
    REQUIRE(entry->enabled);
    REQUIRE(entry->run);
    entry->run();
}

int countColour(const juce::Image& image, juce::Rectangle<int> area, juce::Colour colour)
{
    int count = 0;
    const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
            count += data.getPixelColour(x, y) == colour;
    return count;
}

juce::TextEditor* openEditor(juce::Component& parent)
{
    for (juce::Component* child : parent.getChildren())
        if (auto* editor = dynamic_cast<juce::TextEditor*>(child); editor && editor->isVisible())
            return editor;
    return nullptr;
}

// One layer with a clip in its first cell, playing, and Trails on it; a second clip in the library.
struct Show {
    TempDir media;
    azl::EngineHarness harness{{.width = 960, .height = 540}};
    Client client{harness.document()};
    azl::ClipRef clip, other;
    uint64_t base = 1;
    std::vector<azl::Column> columns;
    azl::WebSocketServer server{harness.document(), 0};
    azl::ui::Session session{"127.0.0.1", server.port()};
    std::unique_ptr<azl::ui::MainComponent> window;

    Show()
    {
        const Message a = addMedia(client, copyMedia("count_100.mp4", media.path / "loops" / "count_100.mp4"));
        const Message b = addMedia(client, copyMedia("count_240.mp4", media.path / "loops" / "count_240.mp4"));
        clip = {a["entry"]["hash"], a["entry"]["name"]};
        other = {b["entry"]["hash"], b["entry"]["name"]};
        columns = document().decks[0].columns;
        apply(client, azl::SetSlot{base, columns[0].id, clip});
        apply(client, azl::AddEffect{base, std::nullopt, {0, "trails", {}}});
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
        harness.step(4);
        window = std::make_unique<azl::ui::MainComponent>(session);
        window->setSize(1600, 950);
        REQUIRE(until([&] { return session.client().synced(); }));
        session.client().requestLibrary();
        REQUIRE(until([&] { return session.client().library().size() == 2; }));
    }

    bool until(const std::function<bool()>& done, double seconds = 20.0) { return runUntil(harness, done, seconds); }
    // An edit from a fresh client, so the UI's changes broadcast meanwhile are not taken for its reply.
    Message edit(const azl::Edit& e)
    {
        Client fresh(harness.document());
        return apply(fresh, e);
    }
    azl::Composition document() { return harness.document().document(); }
    const azl::Layer& layerOf(const azl::Composition& c, uint64_t id)
    {
        for (const azl::Layer& l : c.layers)
            if (l.id == id)
                return l;
        FAIL("no layer " << id);
        return c.layers.front();
    }
    size_t undoSteps() { return session.client().history().undo; }
    bool settled(size_t steps)
    {
        return until([&] { return undoSteps() == steps && session.composition().layers.size() == document().layers.size(); });
    }
};

} // namespace

TEST_CASE("parity: deck dot, deck, column, layer and cell menus, layer reorder, media on a layer, Delete and Cmd+Y")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::DeckTabs& decks = window.grid().decks();
    azl::ui::LayerStack& stack = window.grid().stack();
    azl::ui::ColumnRow& columnRow = window.grid().columns();

    // The deck playing a clip has a dot; shown in another deck's tab, it is the live colour.
    show.edit(azl::AddDeck{std::nullopt, azl::Deck{0, "Peak", {{0, "A"}}}, {}});
    REQUIRE(show.until([&] { return show.session.composition().decks.size() == 2; }));
    CHECK(azl::ui::deckPlaying(show.document(), show.document().decks[0]));
    CHECK_FALSE(azl::ui::deckPlaying(show.document(), show.document().decks[1]));
    show.session.client().selectDeck(show.document().decks[1].id);
    REQUIRE(show.until([&] { return show.session.composition().deck == show.document().decks[1].id; }));
    const juce::Image tabs = decks.createComponentSnapshot(decks.getLocalBounds(), true, 1.0f);
    CHECK(countColour(tabs, decks.tabs()[0].first, azl::ui::Look::palette().live) > 6);
    show.session.client().selectDeck(show.document().decks[0].id);
    REQUIRE(show.until([&] { return show.session.composition().deck == show.document().decks[0].id; }));

    // Duplicate copies the deck with its clips after it; Delete removes it; the last deck cannot be deleted.
    const uint64_t peak = show.document().decks[1].id;
    run(decks.deckMenu(show.document().decks[0].id), "Duplicate");
    REQUIRE(show.until([&] { return show.document().decks.size() == 3; }));
    const azl::Deck copy = show.document().decks[1];
    CHECK(copy.name == show.document().decks[0].name + " copy");
    REQUIRE(copy.columns.size() == show.columns.size());
    CHECK(copy.columns[0].id != show.columns[0].id);
    CHECK(show.layerOf(show.document(), show.base).clips.at(copy.columns[0].id).hash == show.clip.hash);
    REQUIRE(show.until([&] { return show.session.composition().decks.size() == 3; }));
    run(decks.deckMenu(copy.id), "Delete");
    REQUIRE(show.until([&] { return show.session.composition().decks.size() == 2; }));
    run(decks.deckMenu(peak), "Delete");
    REQUIRE(show.until([&] { return show.session.composition().decks.size() == 1; }));
    CHECK_FALSE(azl::ui::menuEntry(decks.deckMenu(show.document().decks[0].id), "Delete")->enabled);

    // A column's menu clears it in one undo step; double-click renames it in place.
    size_t before = show.undoSteps();
    run(columnRow.columnMenu(0), "Clear column");
    REQUIRE(show.until([&] { return show.layerOf(show.document(), show.base).clips.empty(); }));
    REQUIRE(show.settled(before + 1));
    show.session.client().undo();
    REQUIRE(show.until([&] { return show.layerOf(show.document(), show.base).clips.size() == 1; }));
    columnRow.mouseDoubleClick(mouse(columnRow, {stack.columnArea(1).getCentreX(), 8}, {stack.columnArea(1).getCentreX(), 8}));
    juce::TextEditor* editor = openEditor(columnRow);
    REQUIRE(editor);
    editor->setText("Drop");
    editor->onReturnKey();
    REQUIRE(show.until([&] { return show.document().decks[0].columns[1].name == "Drop"; }));

    // Layer menu: move up and down, into a group and out, delete; each one undo step.
    const uint64_t top = show.edit(azl::AddLayer{0, std::nullopt, azl::Layer{.name = "top"}})["layer"]["id"];
    REQUIRE(show.until([&] { return show.session.composition().layers.size() == 2; }));
    run(stack.layerMenu(top), "Move down");
    REQUIRE(show.until([&] { return show.document().layers[0].id == top; }));
    REQUIRE(show.until([&] { return show.session.composition().layers[0].id == top; }));
    CHECK_FALSE(azl::ui::menuEntry(stack.layerMenu(top), "Move down")->enabled);
    run(stack.layerMenu(top), "Move up");
    REQUIRE(show.until([&] { return show.document().layers[1].id == top; }));
    const uint64_t group = show.edit(azl::AddLayer{0, std::nullopt, azl::Layer{.name = "Ground", .group = true}})["layer"]["id"];
    REQUIRE(show.until([&] { return show.session.layer(group) != nullptr; }));
    run(stack.layerMenu(show.base), "Group/Ground");
    REQUIRE(show.until([&] { return show.layerOf(show.document(), group).layers.size() == 1; }));
    REQUIRE(show.until([&] { return show.session.composition().layers.size() == 2; }));
    run(stack.layerMenu(show.base), "Group/None");
    REQUIRE(show.until([&] { return show.document().layers.size() == 3; }));
    REQUIRE(show.until([&] { return show.session.composition().layers.size() == 3; }));
    run(stack.layerMenu(group), "Delete group");
    REQUIRE(show.until([&] { return show.document().layers.size() == 2; }));
    REQUIRE(show.until([&] { return show.session.composition().layers.size() == 2; }));

    // Dragging a layer's name onto another's control puts it just above that one.
    const auto order = [&] {
        std::vector<uint64_t> ids;
        for (const azl::Layer& l : show.document().layers)
            ids.push_back(l.id);
        return ids;
    };
    REQUIRE(order() == std::vector<uint64_t>{top, show.base});
    const DragSource layerDrag("layer:" + juce::String(int64_t(top)), &stack, stack.controlArea(show.base).getCentre());
    stack.itemDragMove(layerDrag);
    stack.itemDropped(layerDrag);
    REQUIRE(show.until([&] { return order() == std::vector<uint64_t>{show.base, top}; }));
    REQUIRE(show.until([&] { return show.session.composition().layers[1].id == top; }));

    // Two clips dropped on a layer's control fill its empty cells, in one undo step.
    before = show.undoSteps();
    std::vector<azl::ui::LibraryItem> items;
    for (const auto& item : show.session.client().library())
        items.push_back(item);
    const DragSource media(azl::ui::BrowserPane::describe(items), &window.browser(), stack.controlArea(show.base).getCentre());
    stack.itemDragMove(media);
    stack.itemDropped(media);
    REQUIRE(show.until([&] { return show.layerOf(show.document(), show.base).clips.size() == 3; }));
    REQUIRE(show.settled(before + 1));
    CHECK(show.layerOf(show.document(), show.base).clips.at(show.columns[0].id).hash == show.clip.hash);
    CHECK(show.layerOf(show.document(), show.base).clips.contains(show.columns[1].id));
    CHECK(show.layerOf(show.document(), show.base).clips.contains(show.columns[2].id));
    show.edit(azl::AddEffect{show.base, std::nullopt, {0, "colorize", {}}, show.columns[1].id});
    REQUIRE(show.until([&] { const auto& c = show.session.layer(show.base)->clips; return c.contains(show.columns[1].id) && c.at(show.columns[1].id).effects.size() == 1; }));
    shoot(window, "parity-grid.png");

    // A cell's menu selects and clears; Delete removes the selected clip; Cmd+Y redoes.
    run(stack.cellMenu(show.base, 2), "Select");
    CHECK(show.session.selectedClip() == azl::ui::Session::ClipCell{show.base, show.columns[2].id});
    run(stack.cellMenu(show.base, 2), "Clear");
    REQUIRE(show.until([&] { return !show.layerOf(show.document(), show.base).clips.contains(show.columns[2].id); }));
    show.session.selectClip({show.base, show.columns[1].id});
    window.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    REQUIRE(show.until([&] { return !show.layerOf(show.document(), show.base).clips.contains(show.columns[1].id); }));
    show.session.client().undo();
    REQUIRE(show.until([&] { return show.layerOf(show.document(), show.base).clips.contains(show.columns[1].id); }));
    window.keyPressed(juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0));
    REQUIRE(show.until([&] { return !show.layerOf(show.document(), show.base).clips.contains(show.columns[1].id); }));
}

TEST_CASE("parity: dragging a deck tab reorders the decks in one undo step, the shown deck kept")
{
    Show show;
    azl::ui::DeckTabs& decks = show.window->grid().decks();
    const uint64_t first = show.document().decks[0].id;
    const uint64_t peak = show.edit(azl::AddDeck{std::nullopt, azl::Deck{0, "Peak", {{0, "A"}}}, {}})["deck"]["id"];
    const uint64_t end = show.edit(azl::AddDeck{std::nullopt, azl::Deck{0, "End", {{0, "B"}}}, {}})["deck"]["id"];
    show.session.client().selectDeck(peak);
    REQUIRE(show.until([&] { return show.session.composition().decks.size() == 3 && show.document().deck == peak; }));
    auto tabOrder = [&] {
        std::vector<uint64_t> ids;
        for (const auto& [area, id] : decks.tabs())
            ids.push_back(id);
        return ids;
    };
    REQUIRE(tabOrder() == std::vector<uint64_t>{first, peak, end});

    // Dragged past the last tab's middle, the first deck lands last; a short drag only shows the deck.
    const size_t before = show.undoSteps();
    const auto tabs = decks.tabs();
    drag(show.harness, decks, tabs[0].first.getCentre(), {tabs[2].first.getRight() - 4, tabs[2].first.getCentreY()});
    REQUIRE(show.until([&] { return tabOrder() == std::vector<uint64_t>{peak, end, first}; }));
    REQUIRE(show.settled(before + 1));
    CHECK(show.session.pastEdits().back() == "Move deck");
    CHECK(show.document().decks[2].id == first);
    CHECK(show.document().deck == peak);
    CHECK(show.session.composition().deck == peak);

    drag(show.harness, decks, decks.tabs()[1].first.getCentre(), decks.tabs()[1].first.getCentre() + juce::Point(2, 0));
    REQUIRE(show.until([&] { return show.document().deck == end; }));
    CHECK(show.undoSteps() == before + 1);

    show.session.client().undo();
    REQUIRE(show.until([&] { return tabOrder() == std::vector<uint64_t>{first, peak, end}; }));
    CHECK(show.document().deck == end);
}

TEST_CASE("parity: transport halves, doubles, nudges, marks Sections, saves and opens; Autopilot, Cues and History tabs")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::TransportBar& transport = window.transport();

    transport.halveButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().bpm == 60.0; }));
    transport.doubleButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().bpm == 120.0; }));

    // Nudge holds while the button is down.
    juce::TextButton& faster = transport.nudgeButton(true);
    faster.setBounds(0, 0, 20, 20);
    static_cast<juce::Component&>(faster).mouseDown(mouse(faster, {5, 5}, {5, 5}));
    CHECK(faster.getToggleState());
    static_cast<juce::Component&>(faster).mouseUp(mouse(faster, {5, 5}, {5, 5}));
    CHECK_FALSE(faster.getToggleState());

    // Drop starts on the next bar and lights up when it has.
    transport.sectionButton(2).triggerClick();
    REQUIRE(show.until([&] { return !show.document().timeline.sections.empty(); }));
    REQUIRE(show.until([&] { return transport.currentSection() == 2; }, 10.0));
    CHECK(show.until([&] { return transport.sectionButton(2).getToggleState(); }));

    // Save, an edit marks the composition unsaved, and opening the file again takes it back as one step.
    CHECK(transport.documentName() == "Untitled");
    const fs::path file = show.media.path / "parity.json";
    show.session.client().saveComposition(file.string());
    REQUIRE(show.until([&] { return fs::exists(file) && !show.session.client().savedPath().empty(); }));
    CHECK(transport.documentName() == "parity");
    CHECK_FALSE(show.session.client().unsaved());
    show.edit(azl::SetLayerOpacity{show.base, 0.3});
    REQUIRE(show.until([&] { return show.session.client().unsaved(); }));
    transport.save();
    REQUIRE(show.until([&] { return !show.session.client().unsaved(); }));
    show.edit(azl::SetLayerOpacity{show.base, 0.9});
    show.session.client().loadComposition(file.string());
    REQUIRE(show.until([&] { return show.document().layers[0].opacity == doctest::Approx(0.3); }));
    REQUIRE(show.until([&] { return !show.session.client().unsaved(); }));

    // Autopilot buttons.
    azl::ui::MidPane& mid = window.mid();
    mid.showTab(1);
    azl::ui::AutopilotView& autopilot = mid.autopilot();
    click(autopilot, autopilot.buttonArea(show.base, 2).getCentre());
    REQUIRE(show.until([&] { return show.document().layers[0].autopilot.mode == azl::AutopilotMode::Random; }));
    click(autopilot, autopilot.buttonArea(show.base, 9).getCentre());
    REQUIRE(show.until([&] { return show.document().layers[0].autopilot.bars == 64; }));
    click(autopilot, autopilot.buttonArea(show.base, 6).getCentre());
    REQUIRE(show.until([&] { return show.document().layers[0].autopilot.bars == 8; }));
    click(autopilot, autopilot.paceArea(3).getCentre());
    REQUIRE(show.until([&] { return show.document().pace == 4.0; }));
    shoot(window, "parity-autopilot.png");

    // A Cue: on Drop, base's Autopilot goes Next every 2 bars; then removed.
    mid.showTab(2);
    azl::ui::CuesView& cues = mid.cues();
    using Part = azl::ui::CuesView::Part;
    cues.addButton().triggerClick();
    REQUIRE(show.until([&] { return cues.draft().has_value(); }));
    run(cues.pickMenu(Part::Do), "Set a layer's Autopilot");
    run(cues.pickMenu(Part::Value), "Next/Every 2 bars");
    cues.doneButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().cues.size() == 1; }));
    const azl::Cue cue = show.document().cues[0];
    CHECK(cue.section == azl::Section::Drop);
    REQUIRE(cue.autopilot.size() == 1);
    CHECK(cue.autopilot[0].layer == show.base);
    CHECK(cue.autopilot[0].mode == azl::AutopilotMode::Next);
    CHECK(cue.autopilot[0].bars == 2);
    REQUIRE(show.until([&] { return show.session.composition().cues.size() == 1; }));
    CHECK(azl::ui::CuesView::describe(show.document(), cue).endsWith(juce::String::fromUTF8("Autopilot Next every 2 bars \xe2\x86\x92 ") + juce::String(show.document().layers[0].name)));
    shoot(window, "parity-cues.png");
    click(cues, cues.removeArea(0).getCentre());
    REQUIRE(show.until([&] { return show.document().cues.empty(); }));

    // History: the last edits by name, and an undo moves one above Now.
    REQUIRE(show.until([&] { return !show.session.pastEdits().empty() && show.session.pastEdits().back() == "Remove cue"; }));
    const size_t past = show.session.pastEdits().size();
    show.session.client().undo();
    REQUIRE(show.until([&] { return show.session.futureEdits().size() == 1; }));
    CHECK(show.session.futureEdits().back() == "Remove cue");
    CHECK(show.session.pastEdits().size() == past - 1);
    mid.showTab(3);
    shoot(window, "parity-history.png");
}

TEST_CASE("a UI joining a restarted engine names the show it resumed, unsaved, and Save writes back to its file")
{
    Show show;
    const fs::path file = show.media.path / "tonight.json";
    show.harness.document().resumeFile({file.string(), true});

    azl::ui::Session joined{"127.0.0.1", show.server.port()};
    azl::ui::TransportBar transport(joined);
    REQUIRE(show.until([&] { return joined.client().synced(); }));
    CHECK(transport.documentName() == "tonight");
    CHECK(joined.client().unsaved());

    show.edit(azl::SetTempo{131.0});
    REQUIRE(show.until([&] { return joined.composition().bpm == 131.0; }));
    transport.save();
    REQUIRE(show.until([&] { return !joined.client().unsaved(); }));
    CHECK(azl::loadCompositionFile(file)->composition.bpm == 131.0);
    CHECK(transport.documentName() == "tonight");
}

TEST_CASE("Open and Save as browse the engine's shows; replacing unsaved changes or a listed show asks first")
{
    Show show;
    azl::ui::TransportBar& transport = show.window->transport();
    const fs::path shows = show.harness.library().root() / "shows";
    const double opacity = show.document().layers[0].opacity;
    auto doubleClick = [&](azl::ui::ShowBrowser& browser, const std::string& name) {
        for (size_t i = 0; i < browser.rows().size(); ++i)
            if (browser.rows()[i].name == name)
                return browser.list().getListBoxModel()->listBoxItemDoubleClicked(
                    int(i), mouse(browser.list(), {5, 5}, {5, 5}));
        FAIL("no show " << name);
    };

    transport.save();
    azl::ui::ShowBrowser* browser = transport.browser();
    REQUIRE(browser);
    CHECK(browser->isVisible());
    CHECK(browser->actionButton().getButtonText() == "Save as");
    REQUIRE(show.until([&] { return browser->folderBox().getNumItems() > 0; }));
    CHECK(fs::equivalent(browser->folderBox().getText().toStdString(), shows));
    browser->pathField().setText("first");
    browser->actionButton().triggerClick();
    REQUIRE(show.until([&] { return fs::exists(shows / "first.json") && !show.session.client().savedPath().empty(); }));
    CHECK_FALSE(browser->isVisible());
    CHECK(transport.documentName() == "first");
    show.session.client().saveComposition((shows / "second.json").string());
    REQUIRE(show.until([&] { return transport.documentName() == "second"; }));

    transport.saveAs();
    browser = transport.browser();
    REQUIRE(show.until([&] { return browser->rows().size() == 2; }));
    doubleClick(*browser, "first");
    CHECK(browser->question().contains("replaces"));
    CHECK(browser->actionButton().getButtonText() == "Replace");
    browser->cancelButton().triggerClick();
    REQUIRE(show.until([&] { return !browser->isVisible(); }));
    CHECK(transport.documentName() == "second");

    transport.open();
    browser = transport.browser();
    CHECK(browser->actionButton().getButtonText() == "Open");
    REQUIRE(show.until([&] { return browser->rows().size() == 2; }));
    browser->pathField().setText((shows / "first.json").string());
    browser->actionButton().triggerClick();
    REQUIRE(show.until([&] { return transport.documentName() == "first"; }));
    CHECK(browser->question().isEmpty());

    show.edit(azl::SetLayerOpacity{show.base, 0.3});
    REQUIRE(show.until([&] { return show.session.client().unsaved(); }));
    transport.open();
    browser = transport.browser();
    REQUIRE(show.until([&] { return browser->rows().size() == 2; }));
    shoot(*show.window, "show-browser-list.png");
    browser->searchField().setText("sec");
    REQUIRE(show.until([&] { return browser->rows().size() == 1; }));
    doubleClick(*browser, "second");
    CHECK(browser->question().contains("backup"));
    CHECK(browser->actionButton().getButtonText() == "Replace");
    pump(show.harness, 200);
    shoot(*show.window, "show-browser.png");
    CHECK(show.document().layers[0].opacity == doctest::Approx(0.3));
    browser->actionButton().triggerClick();
    REQUIRE(show.until([&] { return transport.documentName() == "second"; }));
    REQUIRE(show.until([&] { return show.document().layers[0].opacity == doctest::Approx(opacity); }));
    CHECK_FALSE(browser->isVisible());
}

TEST_CASE("parity: browser search, folders and double-click; number fields scroll, step by ten with Shift and type on a "
          "click; the audio panel jumps to a clip's parameter in another deck")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::BrowserPane& browser = window.browser();

    // Effects filter by name; files group by folder, and a folder header click folds it.
    browser.selectTab(1);
    std::vector<std::string> listed;
    for (const auto& item : browser.effectItems())
        listed.push_back(item.second);
    CHECK(listed == azl::effectTypes());
    CHECK(std::find(listed.begin(), listed.end(), "classicKaleidoscope") != listed.end());
    browser.search().setText("trails");
    REQUIRE(browser.effectItems().size() == 2);
    CHECK(browser.effectItems()[0].second == "trails");
    CHECK(browser.effectItems()[1].second == "classicTrails");
    browser.search().setText("");
    browser.selectTab(0);
    auto files = browser.fileItems();
    REQUIRE(files.size() == 3);
    CHECK(files[0].folder == "loops");
    CHECK(files[0].folderItems.size() == 2);
    click(browser, files[0].bounds.getCentre());
    CHECK(browser.fileItems().size() == 1);
    click(browser, files[0].bounds.getCentre());
    browser.search().setText("240");
    REQUIRE(browser.fileItems().size() == 2);
    CHECK(browser.fileItems()[1].media->name == show.other.name);

    // Double-click puts the clip into the selected layer's first empty cell.
    show.session.selectLayer(show.base);
    const auto thumb = browser.fileItems()[1].bounds.getCentre();
    browser.mouseDoubleClick(mouse(browser, thumb, thumb));
    REQUIRE(show.until([&] { return show.document().layers[0].clips.size() == 2; }));
    CHECK(show.document().layers[0].clips.at(show.columns[1].id).hash == show.other.hash);
    browser.search().setText("");

    // The sensitivity field scrolls (one undo step per run), drags ten steps at a time with Shift, types on a click and
    // steps with the arrow keys while typing.
    azl::ui::NumberField& kick = window.audio().sensitivity("kick");
    size_t before = show.undoSteps();
    for (int i = 0; i < 3; ++i)
        wheel(kick, 0.1f);
    pump(show.harness, 600);
    REQUIRE(show.until([&] { return show.document().sensitivity.count("kick") && show.document().sensitivity.at("kick") == doctest::Approx(1.15); }));
    REQUIRE(show.settled(before + 1));
    drag(show.harness, kick, {10, 5}, {16, 5}, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    REQUIRE(show.until([&] { return show.document().sensitivity.at("kick") == doctest::Approx(2.15); }));
    click(kick, {10, 5});
    REQUIRE(kick.editing());
    juce::TextEditor* editor = openEditor(kick);
    REQUIRE(editor);
    editor->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    REQUIRE(show.until([&] { return show.document().sensitivity.at("kick") == doctest::Approx(2.2); }));
    editor->onEscapeKey();

    // A clip's animated parameter in another deck: the audio panel row shows that deck, selects the clip and
    // highlights the row for a moment.
    const uint64_t deck = show.edit(azl::AddDeck{std::nullopt, azl::Deck{0, "Peak", {{0, "A"}}}, {}})["deck"]["id"];
    REQUIRE(show.until([&] { return show.document().decks.size() == 2; }));
    const uint64_t column = show.document().decks[1].columns[0].id;
    show.edit(azl::SetSlot{show.base, column, show.other});
    const uint64_t fx = show.edit(azl::AddEffect{show.base, std::nullopt, {0, "transform", {}}, column})["effect"]["id"];
    const azl::ParamRef ref{0, fx, "scale"};
    REQUIRE(show.until([&] { return azl::ui::paramInfo(show.session.composition(), ref).has_value(); }));
    show.session.animate(ref, "kick");
    REQUIRE(show.until([&] { return !show.document().modulators.empty(); }));
    REQUIRE(show.until([&] {
        for (const auto& e : window.audio().entries())
            if (e.param == ref)
                return true;
        return false;
    }));
    for (const auto& entry : window.audio().entries())
        if (entry.param == ref)
            click(window.audio(), entry.bounds.getCentre());
    REQUIRE(show.until([&] { return show.session.composition().deck == deck; }));
    CHECK(show.session.selectedClip() == azl::ui::Session::ClipCell{show.base, column});
    CHECK(show.session.focusedParam() == ref);
    CHECK(window.audio().deviceBox().getNumItems() >= 2);
    shoot(window, "parity-jump.png");
    REQUIRE(show.until([&] { return !show.session.focusedParam(); }, 3.0));
}

TEST_CASE("parity: Map lock keeps the mapping from edits; slices and masks rename from the inspector")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.displays().attach({"window-1", "Left projector", 1920, 1080, 60.0});
    Client client(harness.document());
    apply(client, azl::AddScreen{std::nullopt, azl::Screen{.name = "Left", .display = "window-1",
                                                            .slices = {azl::Slice{.name = "Slice 1"}}}});
    const azl::Screen screen = harness.document().document().screens[0];
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::ui::MapView& map = window.map();
    REQUIRE(runUntil(harness, [&] { return session.client().synced() && !session.composition().screens.empty(); }));
    window.setMapMode(true);
    map.select(screen.id, screen.slices[0].id, false);
    REQUIRE(runUntil(harness, [&] { return map.inspector().field("TL") != nullptr; }));

    // Renaming the slice from its Name row.
    juce::Button* name = map.inspector().button("Slice 1");
    REQUIRE(name);
    name->triggerClick();
    juce::TextEditor* editor = nullptr;
    REQUIRE(runUntil(harness, [&] { return (editor = openEditor(map.inspector())) != nullptr; }));
    editor->setText("Left half");
    editor->onReturnKey();
    REQUIRE(runUntil(harness, [&] { return harness.document().document().screens[0].slices[0].name == "Left half"; }));

    // Locked: no nudge, no corner drag, the fields are disabled; Test card still works.
    map.toolbarButton("Lock")->onClick();
    REQUIRE(map.state().locked);
    REQUIRE(runUntil(harness, [&] { return map.inspector().field("TL") && !map.inspector().field("TL")->isEnabled(); }));
    const size_t before = session.client().history().undo;
    CHECK_FALSE(map.nudge(5, 0));
    azl::ui::MapCanvas& canvas = map.canvas();
    const auto corner = canvas.screenPixel(screen.id, {0.0, 0.0});
    drag(harness, canvas, corner, corner + juce::Point<int>(40, 30));
    pump(harness, 300);
    CHECK(session.client().history().undo == before);
    CHECK(harness.document().document().screens[0].slices[0].corners[0] == 0.0);
    REQUIRE(map.inspector().button("Test card"));
    CHECK(map.inspector().button("Test card")->isEnabled());
    // Identify still works: the output shows its card with the red border for a while.
    REQUIRE(map.inspector().button("Identify"));
    CHECK(map.inspector().button("Identify")->isEnabled());
    map.inspector().button("Identify")->triggerClick();
    REQUIRE(runUntil(harness, [&] {
        const azl::Image shown = harness.readScreen(screen.id);
        const uint8_t* p = pixel(shown, 0, 270);
        return p[0] > 250 && p[1] < 5 && p[2] < 5;
    }));
    shoot(window, "parity-map-locked.png");
    map.toolbarButton("Lock")->onClick();
    REQUIRE(runUntil(harness, [&] { return map.inspector().field("TL")->isEnabled(); }));
    CHECK(map.nudge(5, 0));
    REQUIRE(runUntil(harness, [&] { return harness.document().document().screens[0].slices[0].corners[0] > 0.0; }));
}

TEST_CASE("parity: master opacity; the clip card's trigger, speed, in and out; Stop layer; Relink")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::LayerStack& stack = window.grid().stack();
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();

    show.session.selectComposition();
    REQUIRE(show.until([&] { return card.opacityRow() != nullptr; }));
    CHECK(card.opacityRow()->ref() == azl::ParamRef{0, 0, "opacity"});
    click(*card.opacityRow(), card.opacityRow()->barArea().getCentre());
    REQUIRE(show.until([&] { return show.document().opacity < 0.9; }));
    shoot(window, "36-master.png");

    const uint64_t first = show.columns[0].id;
    show.session.selectClip({show.base, first});
    REQUIRE(show.until([&] { return card.choice("Trigger") && card.field("Speed"); }));
    auto clip = [&] { return show.layerOf(show.document(), show.base).clips.at(first); };
    card.choice("Trigger")->setSelectedItemIndex(2, juce::sendNotificationSync);
    REQUIRE(show.until([&] { return clip().quantise == 1; }));
    card.field("Speed")->onChange(2.0);
    REQUIRE(show.until([&] { return clip().transport.speed == 2.0; }));
    card.field("In")->onChange(25.0);
    REQUIRE(show.until([&] { return clip().transport.in == doctest::Approx(0.25); }));
    REQUIRE(show.until([&] { return show.session.layer(show.base)->clips.at(first).transport.in > 0.2; }));
    card.field("Out")->onChange(10.0);
    REQUIRE(show.until([&] { return clip().transport.out == doctest::Approx(0.26); }));
    shoot(window, "36-clip-card.png");

    // Stop layer is live: the layer plays nothing and the history keeps its steps.
    const size_t steps = show.undoSteps();
    run(stack.layerMenu(show.base), "Stop layer");
    REQUIRE(show.until([&] { return !show.layerOf(show.document(), show.base).playing; }));
    pump(show.harness, 200);
    CHECK(show.undoSteps() == steps);

    // A missing clip offers Relink: found again by hash once it is back in a library folder.
    const fs::path gone = copyMedia("count_601_pc.mp4", show.media.path / "loops" / "gone.mp4");
    Client fresh(show.harness.document());
    const std::string hash = addMedia(fresh, gone)["entry"]["hash"];
    fs::remove(gone);
    const uint64_t second = show.columns[1].id;
    show.edit(azl::SetSlot{show.base, second, azl::ClipRef{hash, "gone"}});
    show.session.client().triggerClip(show.base, second);
    REQUIRE(show.until([&] { return show.session.client().failedClips().contains(hash); }));
    CHECK_FALSE(azl::ui::menuEntry(stack.cellMenu(show.base, 0), "Relink (find in the Media Library by hash)"));
    copyMedia("count_601_pc.mp4", show.media.path / "loops" / "found.mp4");
    run(stack.cellMenu(show.base, 1), "Relink (find in the Media Library by hash)");
    REQUIRE(show.until([&] { return !show.session.client().failedClips().contains(hash); }));

    // Relinked to other content, every cell of the old media takes the new one, in one undo step.
    const std::string ghost(64, 'b');
    show.edit(azl::SetSlot{show.base, show.columns[2].id, azl::ClipRef{ghost, "ghost"}});
    show.edit(azl::SetSlot{show.base, show.columns[3].id, azl::ClipRef{ghost, "ghost"}});
    REQUIRE(show.until([&] { return show.session.layer(show.base)->clips.size() == 4; }));
    const size_t before = show.undoSteps();
    show.session.client().relink(ghost, (show.media.path / "loops" / "count_240.mp4").string());
    REQUIRE(show.until([&] {
        const azl::Composition now = show.document();
        const auto& clips = show.layerOf(now, show.base).clips;
        return clips.at(show.columns[2].id).hash == show.other.hash && clips.at(show.columns[3].id).hash == show.other.hash;
    }));
    REQUIRE(show.settled(before + 1));
}

TEST_CASE("parity: column and macro ramp Cues from the Cues tab; History is exact for a UI that joins late")
{
    Show show;
    azl::ui::MidPane& mid = show.window->mid();
    show.edit(azl::AddMacro{std::nullopt, azl::Macro{.name = "Energy"}});
    REQUIRE(show.until([&] { return show.session.composition().macros.size() == 1; }));
    mid.showTab(2);
    azl::ui::CuesView& cues = mid.cues();

    using Part = azl::ui::CuesView::Part;
    cues.addButton().triggerClick();
    REQUIRE(show.until([&] { return cues.draft().has_value(); }));
    run(cues.pickMenu(Part::Do), "Move a macro");
    run(cues.pickMenu(Part::Ramp), "Over 2 bars");
    cues.doneButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().cues.size() == 1; }));
    REQUIRE(show.document().cues[0].macros.size() == 1);
    CHECK(show.document().cues[0].macros[0].bars == 2);

    // A column cue stores the column (and so its deck); "this deck" stores its position instead.
    const juce::String deck(show.document().decks[0].name);
    cues.addButton().triggerClick();
    REQUIRE(show.until([&] { return cues.draft().has_value(); }));
    run(cues.pickMenu(Part::Target), deck + "/" + juce::String(show.columns[1].name));
    cues.doneButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().cues.size() == 2; }));
    CHECK(show.document().cues[1].column == azl::CueColumn{show.columns[1].id, std::nullopt});
    cues.addButton().triggerClick();
    REQUIRE(show.until([&] { return cues.draft().has_value(); }));
    run(cues.pickMenu(Part::Target), juce::String(azl::ui::CuesView::kThisDeck) + "/Column 2");
    cues.doneButton().triggerClick();
    REQUIRE(show.until([&] { return show.document().cues.size() == 3; }));
    CHECK(show.document().cues[2].column == azl::CueColumn{0, 1});
    REQUIRE(show.until([&] { return show.session.composition().cues.size() == 3; }));
    CHECK(azl::ui::CuesView::describe(show.document(), show.document().cues[0]).contains("over 2 bars"));
    CHECK(azl::ui::CuesView::describe(show.document(), show.document().cues[2]).contains("column 2 of this deck"));
    shoot(*show.window, "36-cues.png");

    azl::ui::Session late("127.0.0.1", show.server.port());
    REQUIRE(show.until([&] { return late.client().synced() && late.pastEdits().size() == show.undoSteps(); }));
    CHECK(late.pastEdits().front() == "Set slot");
    CHECK(late.pastEdits().back() == "Add cue");
    CHECK(std::count(late.pastEdits().begin(), late.pastEdits().end(), "Edit") == 0);
}

TEST_CASE("parity: a trigger on an Autopiloted layer shows manual with the bars left; the Layer pane sets the hold")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::LayerStack& stack = window.grid().stack();
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    show.edit(azl::SetSlot{show.base, show.columns[1].id, show.other});
    show.edit(azl::SetLayerAutopilot{show.base, azl::AutopilotMode::Next, 1});

    show.session.selectLayer(show.base);
    REQUIRE(show.until([&] { return card.choice("Hold") != nullptr; }));
    CHECK(card.choice("Hold")->getText() == "16 bars");
    CHECK(card.choice("Every")->getItemText(card.choice("Every")->getNumItems() - 1) == "64 bars");
    card.choice("Hold")->setSelectedItemIndex(1, juce::sendNotificationSync);
    REQUIRE(show.until([&] { return show.layerOf(show.document(), show.base).autopilot.hold == 4; }));
    CHECK(stack.autopilotLabel(show.base) == "auto next 1");

    show.client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", show.base}, {"column", show.columns[1].id}});
    REQUIRE(show.until([&] { return stack.autopilotLabel(show.base).startsWith("manual"); }));
    const int left = stack.autopilotLabel(show.base).fromFirstOccurrenceOf(" ", false, false).getIntValue();
    CHECK(left >= 4);
    CHECK(left <= 5);
    shoot(window, "43-manual.png");
    REQUIRE(show.until([&] { return stack.autopilotLabel(show.base) == "auto next 1"; }));
}

TEST_CASE("parity: the status bar shows the frame guard's level and the Composition card turns the guard off")
{
    Show show;
    azl::ui::TransportBar& transport = show.window->transport();
    azl::ui::OwnerCard& card = show.window->chain().content().ownerCard();
    CHECK(transport.guardText().isEmpty());

    show.harness.engine().setFrameBudget(1e-6);
    REQUIRE(show.until([&] { return transport.guardText() == "guard: 1 layer half-res"; }));

    show.session.selectComposition();
    REQUIRE(show.until([&] { return card.choice("Frame guard") != nullptr; }));
    card.choice("Frame guard")->setSelectedItemIndex(0, juce::sendNotificationSync);
    REQUIRE(show.until([&] { return !show.document().frameGuard; }));
    REQUIRE(show.until([&] { return transport.guardText().isEmpty(); }));
}

TEST_CASE("parity: the status bar flags a screen whose display is missing or mirrored and shows the engine's notice")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.displays().attach({"window-1", "Left projector", 1920, 1080, 60.0});
    harness.displays().attach({"window-2", "Right projector", 1920, 1080, 60.0});
    Client client(harness.document());
    for (const auto& [name, display] : {std::pair{"Left", "window-1"}, std::pair{"Right", "window-2"}})
        apply(client, azl::AddScreen{std::nullopt, azl::Screen{.name = name, .display = display, .slices = {azl::Slice{}}}});
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::ui::TransportBar& transport = window.transport();
    REQUIRE(runUntil(harness, [&] {
        return session.client().synced() && session.composition().screens.size() == 2 &&
               session.client().displays().size() == 2;
    }));
    CHECK(transport.displayText().isEmpty());
    CHECK(session.client().unmirror());

    client.send({{"type", "outputs"}, {"op", "unmirror"}, {"on", false}});
    REQUIRE(runUntil(harness, [&] { return !session.client().unmirror(); }));
    harness.displays().mirror("window-2", "window-1");
    REQUIRE(runUntil(harness, [&] { return transport.displayText() == "Right: display mirrored"; }));
    harness.displays().detach("window-1");
    REQUIRE(runUntil(harness, [&] {
        return transport.displayText() == juce::String::fromUTF8("Left: display missing \xc2\xb7 Right: display mirrored");
    }));

    client.send({{"type", "outputs"}, {"op", "unmirror"}, {"on", true}});
    REQUIRE(runUntil(harness, [&] { return transport.displayText() == "Left: display missing"; }));
    REQUIRE(runUntil(harness, [&] { return session.status().startsWith("Display mirroring turned off"); }));
    CHECK(session.client().unmirror());
}

TEST_CASE("parity: the column Autopilot is set in the Autopilot tab, one undo step per click, pauses the layers' "
          "Autopilots and shows on the deck tab")
{
    Show show;
    azl::ui::MainComponent& window = *show.window;
    azl::ui::LayerStack& stack = window.grid().stack();
    azl::ui::DeckTabs& tabs = window.grid().decks();
    show.edit(azl::SetSlot{show.base, show.columns[1].id, show.other});
    show.edit(azl::SetLayerAutopilot{show.base, azl::AutopilotMode::Next, 1});
    REQUIRE(show.until([&] { return stack.autopilotLabel(show.base) == "auto next 1"; }));
    const uint64_t deck = show.document().deck;
    CHECK(tabs.badge(deck).isEmpty());

    azl::ui::MidPane& mid = window.mid();
    mid.showTab(1);
    azl::ui::AutopilotView& autopilot = mid.autopilot();
    const size_t before = show.undoSteps();
    click(autopilot, autopilot.columnArea(1).getCentre());
    REQUIRE(show.until([&] { return show.document().columnAutopilot.mode == azl::AutopilotMode::Next; }));
    click(autopilot, autopilot.columnArea(6).getCentre());
    REQUIRE(show.until([&] { return show.document().columnAutopilot.bars == 8; }));
    REQUIRE(show.until([&] { return show.undoSteps() == before + 2; }));
    CHECK(show.session.pastEdits().back() == "Set column autopilot");
    REQUIRE(show.until([&] { return stack.autopilotLabel(show.base) == "auto paused"; }));
    CHECK(tabs.badge(deck) == "auto next 8");
    CHECK(autopilot.columnStatus() == "every");
    shoot(window, "67-column-autopilot.png");

    // A trigger holds it, shown with the bars left in its row.
    show.client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", show.base}, {"column", show.columns[1].id}});
    REQUIRE(show.until([&] { return autopilot.columnStatus().startsWith("manual"); }));

    show.session.client().undo();
    REQUIRE(show.until([&] { return show.document().columnAutopilot.bars == 4; }));
    CHECK(show.document().columnAutopilot.mode == azl::AutopilotMode::Next);
    show.session.client().undo();
    REQUIRE(show.until([&] { return show.document().columnAutopilot.mode == azl::AutopilotMode::Off; }));
    REQUIRE(show.until([&] { return stack.autopilotLabel(show.base) == "auto next 1"; }));
    CHECK(tabs.badge(deck).isEmpty());
    CHECK(autopilot.columnStatus() == "every");
}

TEST_CASE("the REC button turns amber while the disk space is low and red once a full disk stopped the recording")
{
    constexpr uint64_t kGB = uint64_t(1) << 30;
    TempDir dir;
    azl::EngineHarness harness({.width = 640, .height = 360});
    std::atomic<uint64_t> free{100 * kGB};
    harness.recorder().setSpaceCheck([&](const fs::path&) { return azl::Recorder::Space{free.load(), 1000 * kGB}; },
                                     std::chrono::milliseconds(1));
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::TransportBar transport(session);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    juce::TextButton& rec = transport.recordButton();

    Client client(harness.document());
    client.send({{"type", "record"}, {"op", "start"}, {"path", (dir.path / "rec.mov").string()},
                 {"reserveMB", 10240}, {"reservePercent", 0}});
    REQUIRE(runUntil(harness, [&] { return rec.getToggleState(); }));
    CHECK_FALSE(bool(rec.getProperties()["accent"]));

    free = 15 * kGB;
    REQUIRE(runUntil(harness, [&] { return bool(rec.getProperties()["accent"]); }));
    CHECK(rec.getToggleState());
    CHECK(rec.getTooltip().contains("Disk space low: 15.0 GB free, the recording stops below 10.0 GB"));

    free = 5 * kGB;
    REQUIRE(runUntil(harness, [&] { return bool(rec.getProperties()["fault"]); }));
    CHECK_FALSE(rec.getToggleState());
    CHECK(rec.getTooltip().startsWith("Recording stopped: the disk is full"));
}

TEST_CASE("the label next to REC shows the audio track's source, audio off, or amber no audio with the reason")
{
    azl::EngineHarness harness({.width = 640, .height = 360});
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    std::atomic<bool> running{false};
    harness.recorder().setAudio(&analysis, [&] {
        return running ? azl::Recorder::AudioSource{.name = "Scarlett Solo USB ch 2"}
                       : azl::Recorder::AudioSource{.reason = "no audio input running"};
    });
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::TransportBar transport(session);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    juce::Label& label = transport.recordAudioLabel();
    const azl::ui::Palette& p = azl::ui::Look::palette();

    REQUIRE(runUntil(harness, [&] { return label.getText() == "no audio: no audio input running"; }));
    CHECK(label.findColour(juce::Label::textColourId) == p.accent);
    CHECK(label.getTooltip().contains("no audio input running"));

    running = true;
    std::vector<float> chunk(800);
    analysis.push(chunk.data(), chunk.size(), 0.0, 48000.0);
    analysis.pump();
    Client client(harness.document());
    client.send({{"type", "record"}, {"op", "set"}, {"audio", true}});
    REQUIRE(runUntil(harness, [&] { return label.getText() == "audio: Scarlett Solo USB ch 2"; }));
    CHECK(label.findColour(juce::Label::textColourId) == p.muted);

    client.send({{"type", "record"}, {"op", "set"}, {"audio", false}});
    REQUIRE(runUntil(harness, [&] { return label.getText() == "audio off"; }));
    CHECK(label.findColour(juce::Label::textColourId) == p.muted);
    harness.recorder().setAudio(nullptr);
}

TEST_CASE("the label next to REC shows when the recording finishes by itself: the gig's end plus grace, or its maximum")
{
    TempDir dir;
    const auto utc = [](const char* date, int hour) { return double(*azl::dateDays(date) * 86400 + hour * 3600); };
    azl::Composition gig;
    gig.gigs = {{.date = "2026-10-16", .start = 20 * 60, .end = 2 * 60}};
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, gig);
    harness.setWallClock(utc("2026-10-17", 1), [](int64_t) { return 0; });
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::TransportBar transport(session);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    juce::Label& label = transport.recordAudioLabel();

    Client client(harness.document());
    client.send({{"type", "record"}, {"op", "start"}, {"path", (dir.path / "gig.mov").string()}, {"audio", false}});
    REQUIRE(runUntil(harness, [&] { return label.getText() == "stops at 02:15"; }));
    CHECK(label.getTooltip().startsWith("It finishes at 02:15, after the gig."));
    CHECK(transport.recordButton().getTooltip().contains("It finishes at 02:15"));
    client.send({{"type", "record"}, {"op", "stop"}});
    REQUIRE(runUntil(harness, [&] { return label.getText() == "audio off"; }));

    harness.setWallClock(utc("2026-10-17", 10), [](int64_t) { return 0; });
    client.send({{"type", "record"}, {"op", "start"}, {"path", (dir.path / "day.mov").string()}});
    REQUIRE(runUntil(harness, [&] { return label.getText() == "stops at 18:00"; }));
    CHECK(label.getTooltip().startsWith("It finishes at 18:00, at its maximum length."));
    client.send({{"type", "record"}, {"op", "stop"}});
    REQUIRE(runUntil(harness, [&] { return !transport.recordButton().getToggleState(); }));
}
