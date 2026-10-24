#include "net/websocket.hpp"
#include "test_client.hpp"
#include "effects/classic_presets.hpp"
#include "document/modulation_edits.hpp"
#include "ui/app/look.hpp"
#include "ui/app/main_component.hpp"
#include "ui/app/preset_tree.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

namespace {

using azl::ParamRef;
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

// A mouse event on `component` at `at`, pressed at `down`.
juce::MouseEvent mouse(juce::Component& component, juce::Point<int> at, juce::Point<int> down,
                       juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    const juce::Time now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, &component, &component, now, down.toFloat(), now, 1, at != down);
}

// Presses at `from`, moves in steps a little apart in time to `to`, and lets go, as a hand does.
void drag(azl::EngineHarness& harness, juce::Component& component, juce::Point<int> from, juce::Point<int> to,
          int steps = 12)
{
    component.mouseDown(mouse(component, from, from));
    for (int i = 1; i <= steps; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        component.mouseDrag(mouse(component, from + (to - from) * i / steps, from));
    }
    component.mouseUp(mouse(component, to, from));
}

void click(juce::Component& component, juce::Point<int> at)
{
    component.mouseDown(mouse(component, at, at));
    component.mouseUp(mouse(component, at, at));
}

// A click on whatever child of `root` is under `at`, as the window routes it.
void clickAt(juce::Component& root, juce::Point<int> at)
{
    juce::Component* target = root.getComponentAt(at);
    REQUIRE(target);
    click(*target, target->getLocalPoint(&root, at));
}

struct Show {
    TempDir media;
    azl::EngineHarness harness{{.width = 960, .height = 540}};
    Client client{harness.document()};
    azl::ClipRef clip;
    uint64_t base = 1;
    uint64_t fx = 0;
    std::vector<azl::Column> columns;

    Show()
    {
        const Message added = addMedia(client, copyMedia("count_100.mp4", media.path / "count_100.mp4"));
        clip = {added["entry"]["hash"], added["entry"]["name"]};
        columns = harness.document().document().decks[0].columns;
        apply(client, azl::SetSlot{base, columns[0].id, clip});
        fx = apply(client, azl::AddEffect{base, std::nullopt, {0, "transform", {}}})["effect"]["id"];
        apply(client, azl::SetLayerOpacity{base, 0.8});
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
        harness.step(4);
    }

    azl::Layer layer() { return document().layers[0]; }
    azl::Composition document() { return harness.document().document(); }
};

size_t undoSteps(azl::ui::Session& session)
{
    return session.client().history().undo;
}

} // namespace

TEST_CASE("panels: effects are added, reordered and removed by drag and drop, and a drag is one undo step")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));

    // From the browser onto the end of the chain, then the new card dragged in front of Transform.
    chain.itemDragMove(DragSource("effect:colorize", &window.browser(), {chain.getWidth() - 20, 60}));
    chain.itemDropped(DragSource("effect:colorize", &window.browser(), {chain.getWidth() - 20, 60}));
    REQUIRE(runUntil(harness, [&] { return show.layer().effects.size() == 2; }));
    CHECK(show.layer().effects[1].type == "colorize");
    const uint64_t colorize = show.layer().effects[1].id;
    REQUIRE(runUntil(harness, [&] { return chain.card(colorize) != nullptr; }));
    const juce::String card = "fxcard:" + juce::String(int64_t(colorize));
    chain.itemDropped(DragSource(card, chain.card(colorize), {chain.card(show.fx)->getX() + 10, 60}));
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[0].id == colorize; }));
    CHECK(chain.gapAt(chain.card(colorize)->getX() + 5) == 0);

    // Onto a clip's cell, the composition header and a layer's control in the grid.
    azl::ui::LayerStack& stack = window.grid().stack();
    // Rows are rebuilt on every change, so the base layer's is looked up each time.
    auto rowTop = [&] {
        for (const auto& r : stack.rows())
            if (r.layer == show.base)
                return r.bounds.getY();
        FAIL("no row for the base layer");
        return 0;
    };
    const juce::Point<int> cell{stack.columnArea(0).getCentreX(), rowTop() + 10};
    stack.itemDragMove(DragSource("effect:edgeDetection", &window.browser(), cell));
    stack.itemDropped(DragSource("effect:edgeDetection", &window.browser(), cell));
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.at(show.columns[0].id).effects.size() == 1; }));
    const juce::Point<int> control{40, rowTop() + 30};
    stack.itemDragMove(DragSource("effect:stripper", &window.browser(), control));
    stack.itemDropped(DragSource("effect:stripper", &window.browser(), control));
    REQUIRE(runUntil(harness, [&] { return show.layer().effects.size() == 3; }));
    window.grid().columns().itemDragMove(DragSource("effect:trails", &window.browser(), {30, 8}));
    window.grid().columns().itemDropped(DragSource("effect:trails", &window.browser(), {30, 8}));
    REQUIRE(runUntil(harness, [&] { return show.document().effects.size() == 1; }));

    // The card's remove button, then a header click collapses Transform to a strip.
    const uint64_t stripper = show.layer().effects[2].id;
    REQUIRE(runUntil(harness, [&] { return chain.card(stripper) != nullptr; }));
    chain.card(stripper)->removeButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.layer().effects.size() == 2; }));
    chain.card(show.fx)->bypassButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[1].bypass; }));
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx)->bypassButton().getToggleState(); }));
    shoot(window, "23-bypass.png");
    chain.card(show.fx)->bypassButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return !show.layer().effects[1].bypass; }));
    click(*chain.card(show.fx), {60, 8});
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx)->getWidth() == azl::ui::EffectCard::kCollapsedWidth; }));
    shoot(window, "23-collapsed.png");
    click(*chain.card(show.fx), {10, 60});
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx)->getWidth() == azl::ui::EffectCard::kWidth; }));

    // Dragging Transform's scale sends many edits but makes one undo step.
    azl::ui::ParamRow* amount = chain.card(show.fx)->row("scale");
    REQUIRE(amount);
    const auto bar = amount->barArea();
    size_t before = undoSteps(session);
    drag(harness, *amount, {bar.getX() + 2, bar.getCentreY()}, {bar.getRight() - bar.getWidth() / 4, bar.getCentreY()});
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[1].params.at("scale") == doctest::Approx(6.0).epsilon(0.05); }));
    CHECK(undoSteps(session) == before + 1);
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[1].params.at("scale") == 1.0; }));

    // The layer's opacity bar in the grid, likewise.
    before = undoSteps(session);
    const int y = rowTop() + 2 * 18 + 8;
    drag(harness, stack, {100, y}, {20, y});
    REQUIRE(runUntil(harness, [&] { return show.layer().opacity < 0.75 && undoSteps(session) == before + 1; }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.layer().opacity == doctest::Approx(0.8); }));

    // Moving a clip to another cell is one undo step too.
    before = undoSteps(session);
    const juce::Point<int> target{stack.columnArea(2).getCentreX(), rowTop() + 10};
    const DragSource moved("cell:" + juce::String(int64_t(show.base)) + ":0", &stack, target);
    stack.itemDragMove(moved);
    stack.itemDropped(moved);
    REQUIRE(runUntil(harness, [&] {
        return show.layer().clips.contains(show.columns[2].id) && !show.layer().clips.contains(show.columns[0].id);
    }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] {
        return show.layer().clips.contains(show.columns[0].id) && !show.layer().clips.contains(show.columns[2].id);
    }));
    CHECK(show.layer().clips.at(show.columns[0].id).effects.size() == 1);
}

TEST_CASE("panels: a drag carries a picture of just its item")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(show.harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(show.harness, [&] { return chain.card(show.fx) != nullptr; }));
    window.browser().selectTab(1);
    azl::ui::LayerStack& stack = window.grid().stack();
    azl::ui::EffectCard& card = *chain.card(show.fx);

    auto check = [](juce::Component& component, juce::Rectangle<int> item) {
        const juce::ScaledImage picture = azl::ui::dragImage(component, item);
        CHECK(picture.getScaledBounds().toNearestInt() == item.withZeroOrigin());
        const juce::Image& image = picture.getImage();
        const int alpha = image.getPixelAt(image.getWidth() / 2, image.getHeight() / 2).getAlpha();
        CHECK((alpha > 100 && alpha < 200));
    };
    const juce::Rectangle<int> row = window.browser().effectItems().front().first;
    check(window.browser(), row);
    CHECK(row.getHeight() < window.browser().getHeight() / 4);
    check(stack, stack.controlArea(show.base));
    CHECK(stack.controlArea(show.base).getWidth() < stack.getWidth() / 4);
    check(card, card.getLocalBounds());
    CHECK(card.getHeight() < window.chain().getHeight());
}

TEST_CASE("panels: a collapsible header toggles on its 22 px chevron or anywhere on its row, not on a control in it, "
          "and still drags")
{
    Show show;
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "colorize", {}}});
    const uint64_t colorize = show.layer().effects.back().id;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
    const int box = azl::ui::Look::metrics().chevronBox;
    CHECK(box >= 22);

    auto card = [&] { return chain.card(show.fx); };
    auto collapsed = [&](bool want) {
        return runUntil(harness, [&] { return session.collapsed(show.fx) == want && card() &&
                                              card()->getWidth() == (want ? azl::ui::EffectCard::kCollapsedWidth
                                                                          : azl::ui::EffectCard::kWidth); });
    };
    const juce::Rectangle<int> header = card()->getLocalBounds().removeFromTop(azl::ui::EffectCard::kHeader);
    CHECK(card()->chevronArea().getWidth() >= box);
    CHECK(card()->chevronArea().getHeight() >= box);
    CHECK(header.contains(card()->chevronArea()));
    shoot(window.chain(), "chevrons-chain.png");

    // The chevron, open and folded.
    clickAt(*card(), card()->chevronArea().getCentre());
    REQUIRE(collapsed(true));
    CHECK(card()->chevronArea().getWidth() >= box);
    clickAt(*card(), card()->chevronArea().getCentre());
    REQUIRE(collapsed(false));

    // An empty part of the header, between the name and the buttons.
    const juce::Point<int> empty{card()->bypassButton().getX() - 70, header.getCentreY()};
    CHECK(card()->getComponentAt(empty) == card());
    clickAt(*card(), empty);
    REQUIRE(collapsed(true));
    clickAt(*card(), {card()->getWidth() / 2, card()->getHeight() / 2});
    REQUIRE(collapsed(false));

    // Bypass, in the header, bypasses and does not fold.
    clickAt(*card(), card()->bypassButton().getBounds().getCentre());
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[0].bypass; }));
    CHECK_FALSE(session.collapsed(show.fx));
    CHECK(card()->getWidth() == azl::ui::EffectCard::kWidth);

    // Folded, the strip keeps its bypass right under the chevron: one undo step, and it stays folded.
    clickAt(*card(), card()->chevronArea().getCentre());
    REQUIRE(collapsed(true));
    const juce::Rectangle<int> folded = card()->bypassButton().getBounds();
    CHECK(card()->bypassButton().isVisible());
    CHECK(card()->bypassButton().getToggleState());
    CHECK(folded.getWidth() >= box);
    CHECK(folded.getHeight() >= box);
    CHECK(folded.getY() >= card()->chevronArea().getBottom());
    CHECK(folded.getBottom() <= 2 * azl::ui::EffectCard::kHeader);
    const size_t before = undoSteps(session);
    clickAt(*card(), folded.getCentre());
    REQUIRE(runUntil(harness, [&] { return !show.layer().effects[0].bypass && undoSteps(session) == before + 1; }));
    CHECK(session.collapsed(show.fx));
    CHECK_FALSE(card()->bypassButton().getToggleState());
    clickAt(*card(), card()->chevronArea().getCentre());
    REQUIRE(collapsed(false));
    CHECK_FALSE(card()->bypassButton().getToggleState());

    // A drag from the header moves the card to the end of the chain and folds nothing.
    REQUIRE(show.layer().effects[0].id == show.fx);
    azl::ui::EffectCard* dragged = card();
    const juce::String description = "fxcard:" + juce::String(int64_t(show.fx));
    const juce::Point<int> from = empty, to = empty + juce::Point<int>(60, 0);
    dragged->mouseDown(mouse(*dragged, from, from));
    dragged->mouseDrag(mouse(*dragged, to, from));
    CHECK(dragged->dragging());
    chain.itemDropped(DragSource(description, dragged, {chain.getWidth() - 20, 60}));
    dragged->mouseUp(mouse(*dragged, to, from));
    REQUIRE(runUntil(harness, [&] {
        const auto effects = show.layer().effects;
        return effects.size() == 2 && effects[0].id == colorize && effects[1].id == show.fx;
    }));
    CHECK_FALSE(session.collapsed(show.fx));

    // A Media Library folder: chevron, then the rest of its row.
    azl::ui::BrowserPane& browser = window.browser();
    browser.selectTab(0);
    auto folder = [&] {
        std::optional<azl::ui::BrowserPane::FileItem> found;
        size_t media = 0;
        for (const auto& item : browser.fileItems()) {
            if (!item.media && !found)
                found = item;
            media += item.media ? 1 : 0;
        }
        return std::pair(found, media);
    };
    REQUIRE(runUntil(harness, [&] { return folder().first && folder().second > 0; }));
    const juce::Rectangle<int> row = folder().first->bounds;
    CHECK(row.getHeight() >= box);
    CHECK(browser.getComponentAt(row.getCentre()) == &browser);
    clickAt(browser, azl::ui::chevronBox(row).getCentre());
    CHECK(folder().second == 0);
    clickAt(browser, {row.getRight() - 40, row.getCentreY()});
    CHECK(folder().second > 0);

    // A section of the Effects tab, likewise.
    browser.selectTab(1);
    azl::ui::PresetTree& tree = browser.effects();
    auto section = [&] {
        for (const auto& r : tree.rows())
            if (r.second.kind == azl::ui::PresetRow::Kind::Section)
                return r;
        FAIL("no section");
        return tree.rows().front();
    };
    REQUIRE(section().second.open);
    CHECK(section().first.getHeight() >= box);
    clickAt(tree, azl::ui::chevronBox(section().first).getCentre());
    CHECK_FALSE(section().second.open);
    clickAt(tree, {section().first.getRight() - 40, section().first.getCentreY()});
    CHECK(section().second.open);

    // A parameter's animation chevron.
    azl::ui::ParamRow* amount = card()->row("scale");
    REQUIRE(amount);
    CHECK(amount->tagArea().getWidth() >= box);
    CHECK(amount->tagArea().getHeight() >= box);
}

TEST_CASE("panels: an effect card moves onto another chain, Option copies it without its links, each one undo step")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    apply(show.client, azl::AddMacro{std::nullopt, azl::Macro{.name = "M", .targets = {{.param = {0, show.fx, "scale"}}}}});
    apply(show.client, azl::SetEffectParam{show.fx, "scale", 2.0});
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
    azl::ui::LayerStack& stack = window.grid().stack();
    const juce::String dragged = "fxcard:" + juce::String(int64_t(show.fx));
    auto rowTop = [&] {
        for (const auto& r : stack.rows())
            if (r.layer == show.base)
                return r.bounds.getY();
        FAIL("no row for the base layer");
        return 0;
    };
    auto onlyOn = [&](const std::vector<azl::Effect>& chain) {
        const azl::Composition c = show.document();
        return chain.size() == 1 && chain[0].id == show.fx && chain[0].params.at("scale") == 2.0 &&
               azl::effectsOn(c, 0).size() + azl::effectsOn(c, show.base).size() == 1;
    };
    auto undo = [&] {
        session.client().undo();
        REQUIRE(runUntil(harness, [&] { return onlyOn(show.layer().effects); }));
    };

    // Onto the Composition header, then the clip's tile: moved with its id and parameters, one undo step each.
    size_t before = undoSteps(session);
    window.grid().columns().itemDragMove(DragSource(dragged, chain.card(show.fx), {30, 8}));
    window.grid().columns().itemDropped(DragSource(dragged, chain.card(show.fx), {30, 8}));
    REQUIRE(runUntil(harness, [&] { return onlyOn(show.document().effects); }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));
    undo();
    const juce::Point<int> cell{stack.columnArea(0).getCentreX(), rowTop() + 10};
    stack.itemDragMove(DragSource(dragged, chain.card(show.fx), cell));
    stack.itemDropped(DragSource(dragged, chain.card(show.fx), cell));
    REQUIRE(runUntil(harness, [&] { return onlyOn(show.layer().clips.at(show.columns[0].id).effects); }));
    undo();

    // Hovering the Composition tab switches to it; the dragged card lives on hidden and drops into that chain.
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
    azl::ui::EffectCard* card = chain.card(show.fx);
    card->mouseDown(mouse(*card, {40, 8}, {40, 8}));
    card->mouseDrag(mouse(*card, {80, 8}, {40, 8}));
    window.chain().itemDragMove(DragSource(dragged, card, {10, 5}));
    CHECK(session.chainTab() == 0);
    CHECK(chain.card(show.fx) == nullptr);
    CHECK(card->dragging());
    chain.itemDropped(DragSource(dragged, card, {chain.getWidth() - 20, 60}));
    card->mouseUp(mouse(*card, {80, 8}, {40, 8}));
    REQUIRE(runUntil(harness, [&] { return onlyOn(show.document().effects); }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return onlyOn(show.layer().effects); }));

    // Option-dropped on the layer's control: a copy with its own id and parameters, which the macro does not drive.
    before = undoSteps(session);
    const juce::Point<int> control{40, rowTop() + 30};
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::altModifier);
    stack.itemDragMove(DragSource(dragged, &stack, control));
    stack.itemDropped(DragSource(dragged, &stack, control));
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys();
    REQUIRE(runUntil(harness, [&] { return show.layer().effects.size() == 2; }));
    const azl::Effect copy = show.layer().effects[1];
    CHECK(copy.id != show.fx);
    CHECK(copy.type == "transform");
    CHECK(copy.params == show.layer().effects[0].params);
    CHECK(show.document().macros[0].targets.size() == 1);
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));
    undo();
}

TEST_CASE("panels: learn mode assigns parameters to a macro, Animate creates a modulator, the macro editor edits it")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) && chain.ownerCard().opacityRow(); }));
    azl::ui::MacrosView& macros = window.mid().macros();

    // Assign: pick the third macro, click Assign, then click two parameter bars; Esc stops.
    click(macros, macros.knobArea(2).getCentre());
    CHECK(session.selectedMacro() == 2);
    macros.assignButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return session.learning(); }));
    azl::ui::ParamRow* amount = chain.card(show.fx)->row("scale");
    click(*amount, amount->barArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return show.document().macros.size() == 3; }));
    azl::ui::ParamRow* opacity = chain.ownerCard().opacityRow();
    click(*opacity, opacity->barArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return show.document().macros[2].targets.size() == 2; }));
    CHECK(show.layer().effects[0].params.at("scale") == 1.0);
    CHECK(show.document().macros[2].targets[0].param == ParamRef{0, show.fx, "scale"});
    CHECK(show.document().macros[2].targets[1].max == doctest::Approx(-0.8));
    shoot(window, "23-learn.png");
    window.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    CHECK_FALSE(session.learning());
    click(*amount, amount->barArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return show.layer().effects[0].params.at("scale") == doctest::Approx(4.0).epsilon(0.05); }));
    CHECK(show.document().macros[2].targets.size() == 2);

    // The first assignment created three macros and a target in one undo step.
    session.client().undo();
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().macros[2].targets.size() == 1; }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().macros.empty(); }));
    session.client().redo();
    session.client().redo();
    REQUIRE(runUntil(harness, [&] { return show.document().macros.size() == 3 && show.document().macros[2].targets.size() == 2; }));

    // The macro editor: a target's range, and the kick driving the macro.
    REQUIRE(runUntil(harness, [&] { return macros.targetRow(1) != nullptr; }));
    macros.targetRow(1)->toField().onChange(50.0);
    REQUIRE(runUntil(harness, [&] { return show.document().macros[2].targets[1].max == doctest::Approx(-0.3); }));
    // The curve editor drags a control point; each move reports the curve.
    std::vector<azl::Curve> curves;
    azl::ui::CurveEditor editor(session, azl::Curve{}, [&](const azl::Curve& curve) { curves.push_back(curve); });
    drag(harness, editor, {12, editor.getHeight() - 12}, {editor.getWidth() / 2, editor.getHeight() - 12});
    REQUIRE_FALSE(curves.empty());
    CHECK(curves.back().points[0] == doctest::Approx(0.5).epsilon(0.05));
    CHECK(curves.back().points[1] == 0.0);
    CHECK(azl::ui::curveName(curves.back()) == "Custom");

    macros.addModulatorButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1; }));
    CHECK(show.document().modulators[0].target.macro == show.document().macros[2].id);
    CHECK(show.document().modulators[0].feature == "kick");
    CHECK(show.document().modulators[0].envelope == azl::EnvelopePreset::Meter);
    CHECK(show.document().modulators[0].release == 500.0);
    // The envelope shows its preset; the times open behind one click and editing one makes it Custom.
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRow(0) && macros.modulatorRow(0)->envelope().presetBox().isVisible(); }));
    azl::ui::EnvelopeControls& envelope = macros.modulatorRow(0)->envelope();
    CHECK(envelope.presetBox().getText() == "Meter");
    CHECK_FALSE(envelope.releaseField().isVisible());
    envelope.timesToggle().triggerClick();
    REQUIRE(runUntil(harness, [&] { return envelope.releaseField().isVisible(); }));
    CHECK(envelope.holdField().isVisible());
    CHECK_FALSE(envelope.presetBox().isVisible());
    envelope.releaseField().onChange(600.0);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].release == 600.0; }));
    CHECK(show.document().modulators[0].envelope == azl::EnvelopePreset::Custom);
    CHECK(macros.modulatorRow(0)->envelope().releaseField().value() == 600.0);
    envelope.timesToggle().triggerClick();
    REQUIRE(runUntil(harness, [&] { return envelope.presetBox().isVisible(); }));
    CHECK(envelope.presetBox().getText() == "Custom");
    envelope.presetBox().setSelectedId(int(azl::EnvelopePreset::Gate));
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].envelope == azl::EnvelopePreset::Gate; }));
    CHECK(show.document().modulators[0].release == 180.0);

    // Animate from the beat: a modulator on the parameter and its settings under the row.
    const ParamRef position{0, show.fx, "positionX"};
    session.animate(position, "beat");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 2; }));
    const azl::Modulator lfo = show.document().modulators[1];
    CHECK(lfo.source == azl::ModulatorSource::Lfo);
    CHECK(lfo.target.param == position);
    CHECK(lfo.min == doctest::Approx(-1.0));
    CHECK(lfo.max == doctest::Approx(1.0));
    azl::ui::ParamRow* row = chain.card(show.fx)->row("positionX");
    REQUIRE(runUntil(harness, [&] { return row->animateBox() != nullptr; }));
    CHECK(row->preferredHeight() > azl::ui::ParamRow::kHeight);
    row->animateBox()->beatsBox().setSelectedId(8);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].beats == 8.0; }));
    row->animateBox()->sourceBox().setSelectedId(3);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].source == azl::ModulatorSource::Audio; }));
    CHECK(show.document().modulators[1].feature == "snare");
    CHECK(show.document().modulators[1].envelope == azl::EnvelopePreset::Meter);
    REQUIRE(runUntil(harness, [&] { return row->animateBox()->envelope().isVisible(); }));
    CHECK(row->animateBox()->envelope().presetBox().getText() == "Meter");
    CHECK(show.document().modulators.size() == 2);
    REQUIRE(runUntil(harness, [&] { return session.drivenValue(position).has_value(); }));
    session.goToParam({show.base, 0, "opacity"});
    session.selectLayer(show.base);
    REQUIRE(runUntil(harness, [&] { return row->animateBox() && row->animateBox()->sourceBox().getSelectedId() == 3; }));
    shoot(window, "23-chain.png");

    // The "~" tag folds the settings away; the audio panel lists what each source drives.
    click(*row, row->tagArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return row->animateBox() == nullptr; }));
    int listed = 0;
    for (const auto& entry : window.audio().entries())
        listed += (entry.param == position) + (entry.macroSlot == 2);
    CHECK(listed == 2);

    // The audio panel's sensitivity fields set the document's per-source sensitivity; a drag is one undo step.
    window.audio().sensitivity("kick").onChange(150.0);
    REQUIRE(runUntil(harness, [&] { return show.document().sensitivity.count("kick") && show.document().sensitivity.at("kick") == 1.5; }));
    REQUIRE(runUntil(harness, [&] { return window.audio().sensitivity("kick").value() == 150.0; }));
    azl::ui::NumberField& lfoAmount = window.audio().sensitivity("beat");
    size_t sensitivitySteps = undoSteps(session);
    drag(harness, lfoAmount, {10, 5}, {-50, 5});
    REQUIRE(runUntil(harness, [&] { return show.document().sensitivity.count("lfo") && show.document().sensitivity.at("lfo") < 1.0; }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == sensitivitySteps + 1; }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().sensitivity.at("lfo") == 1.0; }));
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().sensitivity.at("kick") == 1.0; }));

    // Undo takes the animation back as one step per edit.
    const size_t steps = undoSteps(session);
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].source == azl::ModulatorSource::Lfo; }));
    CHECK(undoSteps(session) == steps - 1);
}

TEST_CASE("panels: the Macros panel lists every modulator of the macro, each edited and removed on its own row")
{
    Show show;
    azl::Modulator kick;
    kick.source = azl::ModulatorSource::Audio;
    kick.target.macro = 1;
    azl::Modulator lfo;
    lfo.beats = 4.0;
    lfo.target.macro = 1;
    azl::Macro energy{.id = 1, .name = "Energy", .value = 0.2, .targets = {{{0, show.fx, "scale"}, {}, 0.0, 2.0}}};
    apply(show.client, azl::SetMacros{{energy}, {kick, lfo}});

    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    window.mid().showMacros();
    azl::ui::MacrosView& macros = window.mid().macros();
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRows() == 2; }));
    azl::ui::ModulatorRow& kickRow = *macros.modulatorRow(0);
    azl::ui::ModulatorRow& lfoRow = *macros.modulatorRow(1);
    CHECK(kickRow.sourceBox().getText() == "Kick");
    // An old show's raw modulator is flagged; one click puts it on Meter.
    CHECK(kickRow.envelope().rawFlag().isVisible());
    CHECK_FALSE(kickRow.envelope().presetBox().isVisible());
    kickRow.envelope().rawFlag().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].envelope == azl::EnvelopePreset::Meter; }));
    REQUIRE(runUntil(harness, [&] { return kickRow.envelope().presetBox().isVisible(); }));
    CHECK_FALSE(kickRow.envelope().rawFlag().isVisible());
    CHECK(kickRow.envelope().presetBox().getText() == "Meter");
    CHECK_FALSE(kickRow.beatsBox().isVisible());
    CHECK(lfoRow.sourceBox().getText() == "Beat (LFO)");
    CHECK(lfoRow.beatsBox().isVisible());
    CHECK(lfoRow.toField().value() == 100.0);
    shoot(window, "63-modulators.png");

    lfoRow.beatsBox().setSelectedId(8);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].beats == 8.0; }));
    lfoRow.combineBox().setSelectedId(2);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].combine == azl::Combine::Multiply; }));
    lfoRow.fromField().onChange(25.0);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].min == doctest::Approx(0.25); }));
    lfoRow.combineBox().setSelectedId(1);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].combine == azl::Combine::Add; }));
    // An added driver's ends are where it takes the macro from its value: the "to" end at 60% is an offset of 0.4.
    lfoRow.toField().onChange(60.0);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].max == doctest::Approx(0.4); }));
    const size_t steps = undoSteps(session);
    drag(harness, lfoRow, {lfoRow.barArea().getX() + lfoRow.barArea().getWidth() * 6 / 10, lfoRow.barArea().getCentreY()},
         {lfoRow.barArea().getX() + lfoRow.barArea().getWidth() * 8 / 10, lfoRow.barArea().getCentreY()});
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].max == doctest::Approx(0.6).epsilon(0.03); }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    kickRow.sourceBox().setSelectedId(5);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].feature == "bass"; }));

    // Removing one driver leaves the other; Add modulator adds any number more, each with a source not yet driving it.
    kickRow.removeButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1 && macros.modulatorRows() == 1; }));
    CHECK(show.document().modulators[0].source == azl::ModulatorSource::Lfo);
    macros.addModulatorButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRows() == 2; }));
    macros.addModulatorButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRows() == 3; }));
    const azl::Composition document = show.document();
    CHECK(document.modulators[1].feature == "kick");
    CHECK(document.modulators[2].feature == "snare");
    CHECK(document.modulators[2].target.macro == 1);
}

TEST_CASE("panels: Load first shows what the set brings, then applies only the ticked items; macros keep their targets")
{
    Show show;
    azl::Modulator stale;
    stale.name = "Old evolve";
    stale.target.param = {0, show.fx, "gone"};
    azl::Modulator drive;
    drive.source = azl::ModulatorSource::Audio;
    drive.feature = "bass";
    drive.target.param = {show.base, 0, "opacity"};
    drive.min = -0.5;
    drive.max = 0.0;
    azl::Macro energy{.id = 1, .name = "Energy", .targets = {{{0, show.fx, "scale"}, {}, 0.0, 2.0}}};
    azl::Macro strobe{.id = 2, .name = "Strobe", .value = 0.7};
    azl::Modulator kick;
    kick.source = azl::ModulatorSource::Audio;
    kick.target.macro = 1;
    azl::Modulator saw;
    saw.shape = azl::LfoShape::Saw;
    saw.target.macro = 2;
    apply(show.client, azl::SetMacros{{energy, strobe}, {stale, drive, kick, saw}});
    show.client.send({{"type", "modulation"}, {"op", "save"}, {"name", "Techno"}});
    while (show.client.next()["type"] != "modulation") {
    }
    apply(show.client, azl::SetMacros{{{.id = 1, .name = "Other", .targets = {{{show.base, 0, "opacity"}, {}, -1.0, 0.0}}}},
                                      {stale, drive}});
    const azl::Composition before = show.document();

    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced() && !session.client().macroSets().empty(); }));
    window.mid().showMacros();
    azl::ui::MacrosView& macros = window.mid().macros();

    macros.loadButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.setPreview() != nullptr; }));
    azl::ui::SetPreview* preview = macros.setPreview();
    for (const char* key : {"m0", "d0", "m1", "d1"})
        REQUIRE_MESSAGE(preview->item(key), key);
    for (const char* key : {"m0", "d0", "m1", "d1"})
        CHECK(preview->item(key)->getToggleState());
    CHECK(preview->item("m0")->getButtonText() == "Energy");
    CHECK(preview->item("d0")->getButtonText() == "Kick -> Energy");
    CHECK(preview->item("m1")->getButtonText() == "Strobe  (new, no targets)");
    CHECK(preview->item("d1")->getButtonText() == "Beat LFO -> Strobe");
    shoot(window, "63-load-preview.png");
    preview->cancelButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.setPreview() == nullptr; }));
    CHECK(show.document().macros == before.macros);
    CHECK(show.document().modulators == before.modulators);

    macros.loadButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.setPreview() != nullptr; }));
    macros.setPreview()->applyButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.document().macros[0].name == "Energy"; }));
    CHECK(macros.setPreview() == nullptr);
    const azl::Composition loaded = show.document();
    REQUIRE(loaded.macros.size() == 2);
    CHECK(loaded.macros[0].targets == before.macros[0].targets);
    CHECK(loaded.macros[1].name == "Strobe");
    CHECK(loaded.macros[1].value == 0.7);
    CHECK(loaded.macros[1].targets.empty());
    REQUIRE(loaded.modulators.size() == 4);
    CHECK(loaded.modulators[0] == before.modulators[0]);
    CHECK(loaded.modulators[1] == before.modulators[1]);
    CHECK(loaded.modulators[2].target.macro == 1);
    CHECK(loaded.modulators[3].target.macro == loaded.macros[1].id);
    const std::vector<uint64_t> dangling{before.modulators[0].id};
    REQUIRE(runUntil(harness, [&] { return session.client().modulation().dangling == dangling; }));
    const azl::ui::Menu fix = macros.danglingMenu();
    REQUIRE(fix.size() == 1);
    CHECK(fix[0].label.contains(juce::String::fromUTF8("\xe2\x86\x92")));
    REQUIRE_FALSE(fix[0].sub.empty());
    CHECK(fix[0].sub[0].label == "Remove");

    window.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0));
    REQUIRE(runUntil(harness, [&] { return show.document().macros == before.macros; }));
    CHECK(show.document().modulators == before.modulators);
    shoot(window, "65-dangling.png");

    // Unticking the new macro and the kick loads only Energy's name and value.
    macros.loadButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return macros.setPreview() != nullptr; }));
    macros.setPreview()->item("d0")->triggerClick();
    macros.setPreview()->item("m1")->triggerClick();
    REQUIRE(runUntil(harness, [&] { return !macros.setPreview()->item("d1")->isEnabled(); }));
    macros.setPreview()->applyButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return show.document().macros[0].name == "Energy"; }));
    const azl::Composition picked = show.document();
    REQUIRE(picked.macros.size() == 1);
    CHECK(picked.macros[0].targets == before.macros[0].targets);
    CHECK(picked.modulators == before.modulators);
}

TEST_CASE("panels: the audio input list follows the engine's pushes without a reconnect")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(show.harness, [&] { return session.client().synced(); }));
    juce::ComboBox& box = window.audio().deviceBox();
    REQUIRE(runUntil(show.harness, [&] { return box.getNumItems() == 4; }));

    const auto push = [&](Message message) {
        message["type"] = "audio";
        show.harness.document().broadcast(message);
    };
    push({{"op", "devices"},
          {"devices",
           {{{"uid", "builtin"}, {"name", "Built-in Microphone"}},
            {{"uid", "usb"}, {"name", "USB Mic"}},
            {{"uid", "line"}, {"name", "Line In"}}}}});
    REQUIRE(runUntil(show.harness, [&] { return box.getNumItems() == 5; }));
    CHECK(box.getItemText(2) == "USB Mic");
    push({{"op", "input"}, {"name", "USB Mic"}, {"state", "running"}});
    REQUIRE(runUntil(show.harness, [&] { return box.getText() == "USB Mic"; }));

    // The open device is pulled out: the list loses it and the input falls back to the default.
    push({{"op", "devices"}, {"devices", {{{"uid", "builtin"}, {"name", "Built-in Microphone"}}}}});
    push({{"op", "input"}, {"name", "Built-in Microphone"}, {"state", "fallback"}});
    REQUIRE(runUntil(show.harness, [&] { return box.getNumItems() == 3 && box.getText() == "Built-in Microphone"; }));
}

TEST_CASE("panels: the audio input's channels and gain: picker, Auto, gain in dB, the applied gain and a channel hint")
{
    CHECK(azl::ui::channelChoices(1).size() == 1);
    const auto two = azl::ui::channelChoices(2);
    REQUIRE(two.size() == 3);
    CHECK(two[0].label == "1+2");
    CHECK(two[0].channels.empty());
    CHECK(two[2].channels == std::vector<int>{2});
    const auto four = azl::ui::channelChoices(4);
    CHECK(four.size() == 7);
    CHECK(four.back().label == "3+4");
    CHECK(four.back().channels == std::vector<int>{3, 4});
    CHECK(azl::ui::channelHint({{-90.0, 0.0}, {-12.0, 0.05}}) == "Ch 1 silent");
    CHECK(azl::ui::channelHint({{-58.0, 0.97}, {-12.0, 0.05}}) == "Ch 1 noise only");
    CHECK(azl::ui::channelHint({{-30.0, 0.1}, {-12.0, 0.05}}).isEmpty());

    Show show;
    std::mutex mutex;
    std::vector<Message> received;
    show.harness.document().handle("audio", [&](const Message& message, azl::DocumentServer::Sink) {
        std::lock_guard lock(mutex);
        received.push_back(message);
    });
    const auto last = [&](const std::string& op) {
        std::lock_guard lock(mutex);
        for (auto m = received.rbegin(); m != received.rend(); ++m)
            if ((*m)["op"] == op)
                return *m;
        return Message();
    };
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(show.harness, [&] { return session.client().synced(); }));
    azl::ui::AudioPanel& panel = window.audio();
    const auto push = [&](Message message) {
        message["type"] = "audio";
        show.harness.document().broadcast(message);
    };
    push({{"op", "devices"},
          {"devices", {{{"uid", "mic"}, {"name", "MacBook Microphone"}, {"channels", 1}},
                       {{"uid", "solo"}, {"name", "Scarlett Solo"}, {"channels", 2}}}}});
    push({{"op", "input"}, {"uid", "solo"}, {"name", "Scarlett Solo"}, {"channels", {2}}, {"state", "running"}});
    push({{"op", "settings"}, {"autoGain", false}, {"gainDb", 12.0}});
    push({{"op", "state"},
          {"live", true},
          {"gainDb", 12.0},
          {"values", {{"level", 0.6}}},
          {"channels", {{{"levelDb", -58.0}, {"noise", 0.97}}, {{"levelDb", -14.0}, {"noise", 0.04}}}}});
    juce::ComboBox& channels = panel.channelBox();
    REQUIRE(runUntil(show.harness, [&] { return channels.getNumItems() == 3 && channels.getText() == "2"; }));
    CHECK(channels.isEnabled());
    CHECK(!panel.autoGainButton().getToggleState());
    CHECK(panel.gainSlider().getValue() == 12.0);
    CHECK(panel.gainSlider().isEnabled());
    CHECK(session.client().audio().channels.size() == 2);
    shoot(panel, "57-audio-gain.png");

    // The picker and the gain controls send to the engine, which keeps them for this device.
    channels.setSelectedItemIndex(0);
    REQUIRE(runUntil(show.harness, [&] { return last("select").contains("channels"); }));
    CHECK(last("select")["device"] == "solo");
    CHECK(last("select")["channels"].empty());
    panel.gainSlider().setValue(6.0);
    REQUIRE(runUntil(show.harness, [&] { return last("settings").value("gainDb", 0.0) == 6.0; }));
    CHECK(last("settings")["autoGain"] == false);
    panel.autoGainButton().triggerClick();
    REQUIRE(runUntil(show.harness, [&] { return last("settings").value("autoGain", false); }));

    // Auto on, as the engine reports it back: the slider rests, the applied gain still shows.
    push({{"op", "settings"}, {"autoGain", true}, {"gainDb", 6.0}});
    REQUIRE(runUntil(show.harness, [&] { return !panel.gainSlider().isEnabled(); }));
    CHECK(panel.autoGainButton().getToggleState());
    CHECK(panel.tipAt(panel.appliedGainArea().getCentre()).contains("applied"));

    // A one-channel device has nothing to pick.
    push({{"op", "input"}, {"uid", "mic"}, {"name", "MacBook Microphone"}, {"channels", Message::array()},
          {"state", "running"}});
    REQUIRE(runUntil(show.harness, [&] { return channels.getNumItems() == 1 && !channels.isEnabled(); }));
}

TEST_CASE("panels: the Clip tab shows the selected clip, else the selected layer's playing clip, else nothing; the "
          "Layer tab with no layer selected shows nothing")
{
    Show show;
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "edgeDetection", {}}, show.columns[0].id});
    const uint64_t edges = show.layer().clips.at(show.columns[0].id).effects.at(0).id;
    const uint64_t tint = apply(show.client, azl::AddEffect{0, std::nullopt, {0, "colorize", {}}})["effect"]["id"];
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] {
        const azl::Layer* layer = session.layer(show.base);
        return session.client().synced() && layer && layer->playing;
    }));
    azl::ui::ChainContent& chain = window.chain().content();

    session.selectLayer(show.base);
    session.setChainTab(2);
    CHECK(session.chainOwner() == azl::ui::ChainOwner{show.base, show.columns[0].id});
    REQUIRE(runUntil(harness, [&] { return chain.card(edges) && !chain.card(show.fx); }));
    shoot(window, "44-clip-tab-playing.png");

    session.selectComposition();
    session.setChainTab(2);
    CHECK(session.chainOwner() == azl::ui::ChainOwner{0, 0});
    REQUIRE(runUntil(harness, [&] { return !chain.card(edges) && !chain.card(show.fx); }));
    CHECK(chain.ownerCard().opacityRow() == nullptr);
    shoot(window, "44-clip-tab-empty.png");

    session.setChainTab(1);
    CHECK(session.chainOwner() == azl::ui::ChainOwner{azl::ui::kNoLayer});
    REQUIRE(runUntil(harness, [&] { return !chain.card(tint) && !chain.card(show.fx); }));
    CHECK(chain.ownerCard().opacityRow() == nullptr);
    CHECK(chain.ownerCard().choice("Quantise") == nullptr);
    shoot(window, "55-layer-tab-empty.png");
}

TEST_CASE("panels: an animated layer opacity opens its settings box and moves the rows below it down")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.opacityRow() != nullptr; }));

    const ParamRef opacity{show.base, 0, "opacity"};
    session.animate(opacity, "bass");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1; }));
    azl::ui::ParamRow* row = card.opacityRow();
    click(*row, row->tagArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return row->animateBox() == nullptr; }));
    click(*row, row->tagArea().getCentre());
    REQUIRE(runUntil(harness, [&] { return row->animateBox() != nullptr; }));
    CHECK(row->getHeight() == row->preferredHeight());
    CHECK(row->animateBox()->isVisible());
    CHECK(row->animateBox()->getBottom() <= row->getHeight());
    CHECK(card.choice("Blend")->getY() >= row->getBottom());
    shoot(window, "44-opacity-animation.png");
}

TEST_CASE("panels: each driven row of the audio panel shows its own modulator live")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    session.animate({show.base, 0, "opacity"}, "beat");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1; }));
    session.animate({0, show.fx, "positionX"}, "bass");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 2; }));
    REQUIRE(runUntil(harness, [&] { return session.modulation().modulators.size() == 2; }));

    std::vector<uint64_t> shown;
    for (const auto& entry : window.audio().entries())
        if (entry.modulator)
            shown.push_back(entry.modulator);
    CHECK(shown == std::vector<uint64_t>{show.document().modulators[1].id, show.document().modulators[0].id});
    shoot(window, "44-audio-rows.png");
}

TEST_CASE("panels: the Layer tab keeps a layer at output size at a render scale, and sets it back")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.choice("Resolution") != nullptr; }));
    CHECK(card.choice("Resolution")->getText() == "Render scale");
    card.choice("Resolution")->setSelectedItemIndex(1, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return show.layer().fullResolution; }));
    REQUIRE(runUntil(harness, [&] { return card.choice("Resolution")->getText() == "Output size"; }));
    card.choice("Resolution")->setSelectedItemIndex(0, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return !show.layer().fullResolution; }));
}

TEST_CASE("panels: the Layer tab edits the deck in view, marks its own fields and sets them back to shared")
{
    Show show;
    apply(show.client, azl::AddDeck{std::nullopt, azl::Deck{0, "Deck 2", {{0, "E"}}}, {}});
    const uint64_t deck2 = show.document().decks[1].id;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.client().selectDeck(deck2);
    session.selectLayer(show.base);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return show.document().deck == deck2 && card.choice("Chain") != nullptr; }));
    CHECK(card.choice("Chain")->getText() == "Shared");
    juce::ComboBox* blend = card.choice("Blend");
    CHECK(blend->getItemText(blend->getNumItems() - 1) == "Own in Deck 2");
    CHECK_FALSE(card.own("Blend"));

    // Made the deck's own, then edited there; the shared value stays.
    blend->setSelectedItemIndex(blend->getNumItems() - 1, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return show.layer().decks.contains(deck2) && card.own("Blend"); }));
    CHECK(show.layer().decks.at(deck2).blend == azl::BlendMode::Normal);
    card.choice("Blend")->setSelectedItemIndex(int(azl::BlendMode::Add), juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return show.layer().decks.at(deck2).blend == azl::BlendMode::Add; }));
    CHECK(show.layer().blend == azl::BlendMode::Normal);
    CHECK(card.choice("Blend")->getItemText(card.choice("Blend")->getNumItems() - 1) == "Back to shared");

    // The chain made the deck's own: a copy of the shared one with fresh ids, which the cards then show.
    card.choice("Chain")->setSelectedItemIndex(1, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return show.layer().decks.at(deck2).effects.has_value(); }));
    const uint64_t copy = show.layer().decks.at(deck2).effects->at(0).id;
    CHECK(copy != show.fx);
    REQUIRE(runUntil(harness, [&] { return chain.card(copy) != nullptr && chain.card(show.fx) == nullptr; }));
    CHECK(card.own("Chain"));
    shoot(window, "56-layer-tab-deck.png");

    // Back to shared, and deck 1 shows the shared settings.
    card.choice("Blend")->setSelectedItemIndex(card.choice("Blend")->getNumItems() - 1, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return !show.layer().decks.at(deck2).blend && !card.own("Blend"); }));
    session.client().selectDeck(show.document().decks[0].id);
    REQUIRE(runUntil(harness, [&] { return card.choice("Chain") && card.choice("Chain")->getText() == "Shared"; }));
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
}

TEST_CASE("panels: an animated parameter shows its range in its units as two handles on its bar, which pass each other")
{
    Show show;
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "stripper", {{"direction", 30.0}, {"speed", 2.3}}}});
    const uint64_t stripper = show.layer().effects.back().id;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(stripper) != nullptr; }));
    auto modulator = [&](size_t i) { return show.document().modulators.at(i); };
    shoot(*chain.card(stripper), "56-units.png");

    // Direction swings a quarter turn either way of 30 degrees; From and To read in degrees.
    const ParamRef direction{0, stripper, "direction"};
    session.animate(direction, "beat");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1; }));
    CHECK(modulator(0).min == doctest::Approx(-90.0));
    CHECK(modulator(0).max == doctest::Approx(90.0));
    azl::ui::ParamRow* row = chain.card(stripper)->row("direction");
    REQUIRE(runUntil(harness, [&] { return row->animateBox() != nullptr && row->animateBox()->toField().value() == 120.0; }));
    CHECK(row->animateBox()->fromField().value() == -60.0);

    // The To handle dragged down past From to -150 degrees: the animation runs inverted.
    const auto bar = row->barArea();
    const int y = bar.getCentreY();
    const int toX = *row->handleX(1);
    CHECK(toX == bar.getX() + juce::roundToInt(bar.getWidth() * 300.0f / 360.0f));
    const size_t steps = undoSteps(session);
    drag(harness, *row, {toX, y}, {bar.getX() + juce::roundToInt(bar.getWidth() / 12.0f), y});
    REQUIRE(runUntil(harness, [&] { return modulator(0).max == doctest::Approx(-180.0).epsilon(0.03); }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    REQUIRE(runUntil(harness, [&] { return std::abs(row->animateBox()->toField().value() + 150.0) <= 4.0; }));
    CHECK(show.layer().effects[1].params.at("direction") == 30.0);

    // From typed in degrees; the live value moves between the ends.
    row->animateBox()->fromField().onChange(-90.0);
    REQUIRE(runUntil(harness, [&] { return modulator(0).min == doctest::Approx(-120.0); }));
    REQUIRE(runUntil(harness, [&] { return session.drivenValue(direction).has_value(); }));
    shoot(*chain.card(stripper), "56-animate-range.png");

    // An audio source's rest and peak work the same: speed rises from 2.30 to 3.30 strips/s on the kick.
    const ParamRef speed{0, stripper, "speed"};
    session.animate(speed, "kick");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 2; }));
    CHECK(modulator(1).min == 0.0);
    CHECK(modulator(1).max == doctest::Approx(1.0));
    azl::ui::ParamRow* speedRow = chain.card(stripper)->row("speed");
    REQUIRE(runUntil(harness, [&] { return speedRow->animateBox() != nullptr; }));
    CHECK(speedRow->animateBox()->fromField().value() == doctest::Approx(2.3));
    CHECK(speedRow->animateBox()->toField().value() == doctest::Approx(3.3));
    const auto speedBar = speedRow->barArea();
    drag(harness, *speedRow, {*speedRow->handleX(1), speedBar.getCentreY()}, {speedBar.getRight(), speedBar.getCentreY()});
    REQUIRE(runUntil(harness, [&] { return modulator(1).max == doctest::Approx(1.7); }));
    shoot(*chain.card(stripper), "56-audio-range.png");
}

namespace {

// A show with two layers of clips: the base layer's in columns 1 to 3, the top layer's in columns 1 and 2.
struct Grid : Show {
    uint64_t top = 0;

    Grid()
    {
        apply(client, azl::AddLayer{0, std::nullopt, azl::Layer{.name = "Top"}});
        for (const azl::Layer& layer : document().layers)
            if (layer.name == "Top")
                top = layer.id;
        for (size_t i = 1; i < 3; ++i)
            apply(client, azl::SetSlot{base, columns[i].id, clip});
        for (size_t i = 0; i < 2; ++i)
            apply(client, azl::SetSlot{top, columns[i].id, clip});
    }

    azl::ui::Session::ClipCell cell(uint64_t layer, size_t column) const { return {layer, columns[column].id}; }
};

// The middle of a cell's name strip, or of its thumbnail.
juce::Point<int> cellPoint(azl::ui::LayerStack& stack, uint64_t layer, int slot, bool thumb = false)
{
    for (const auto& row : stack.rows())
        if (row.layer == layer)
            return {stack.columnArea(slot).getCentreX(), thumb ? row.bounds.getY() + 10 : row.bounds.getBottom() - 4};
    FAIL("no row for the layer");
    return {};
}

void clickWith(juce::Component& component, juce::Point<int> at, juce::ModifierKeys mods)
{
    component.mouseDown(mouse(component, at, at, mods.withFlags(juce::ModifierKeys::leftButtonModifier)));
    component.mouseUp(mouse(component, at, at, mods));
}

using Cells = std::set<azl::ui::Session::ClipCell>;

struct GridWindow {
    Grid show;
    azl::WebSocketServer server{show.harness.document(), 0};
    azl::ui::Session session{"127.0.0.1", server.port()};
    azl::ui::MainComponent window{session};
    azl::ui::LayerStack& stack = window.grid().stack();

    GridWindow()
    {
        window.setSize(1600, 950);
        REQUIRE(runUntil(show.harness, [&] { return session.client().synced() && session.deckClips().size() == 5; }));
    }

    bool until(const std::function<bool()>& done, double seconds = 20.0) { return runUntil(show.harness, done, seconds); }
    juce::Point<int> at(uint64_t layer, int slot, bool thumb = false) { return cellPoint(stack, layer, slot, thumb); }
};

const auto kCmd = juce::ModifierKeys(juce::ModifierKeys::commandModifier);
const auto kShift = juce::ModifierKeys(juce::ModifierKeys::shiftModifier);

} // namespace

TEST_CASE("panels: Cmd-click toggles clips, Shift-click selects a block, Cmd-A the deck, Esc clears; a plain click "
          "selects one and the thumbnail still triggers")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;

    click(w.stack, w.at(show.base, 0));
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 0)});
    clickWith(w.stack, w.at(show.top, 1), kCmd);
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 0), show.cell(show.top, 1)});
    CHECK(session.selectedClip() == show.cell(show.top, 1));
    clickWith(w.stack, w.at(show.top, 1, true), kCmd);
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 0)});
    CHECK(!session.client().queued(show.top, show.columns[1].id));

    // The block from the base layer's first clip to the top layer's third cell: the empty cell left out.
    clickWith(w.stack, w.at(show.top, 2), kShift);
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 0), show.cell(show.base, 1), show.cell(show.base, 2),
                                           show.cell(show.top, 0), show.cell(show.top, 1)});
    shoot(w.window, "64-block-selected.png");

    // A plain press on a selected clip keeps the block for a drag and selects that clip alone on release.
    w.stack.mouseDown(mouse(w.stack, w.at(show.base, 1), w.at(show.base, 1)));
    CHECK(session.selectedClips().size() == 5);
    w.stack.mouseUp(mouse(w.stack, w.at(show.base, 1), w.at(show.base, 1)));
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 1)});

    CHECK(w.window.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    CHECK(session.selectedClips().size() == 5);
    CHECK(w.window.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    CHECK(session.selectedClips().empty());
    CHECK(!session.selectedClip());

    click(w.stack, w.at(show.top, 0, true));
    CHECK(session.selectedClips().empty());
    REQUIRE(w.until([&] { return session.client().playing(show.top, show.columns[0].id); }));
}

TEST_CASE("panels: an effect dropped on a selected clip lands on every selected clip; Delete, a block drag and an "
          "Option-drag act on the whole selection; each is one undo step")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;
    auto clipsOf = [&](uint64_t layer) {
        std::set<uint64_t> columns;
        for (const auto& [column, clip] : session.layer(layer)->clips)
            columns.insert(column);
        return columns;
    };
    auto withTransform = [&] {
        size_t n = 0;
        for (const auto& cell : session.deckClips())
            n += session.clip(cell)->effects.size() == 1 && session.clip(cell)->effects[0].type == "transform";
        return n;
    };
    auto undo = [&](const std::function<bool()>& back) {
        session.client().undo();
        REQUIRE(w.until(back));
    };
    auto drop = [&](const juce::String& description, juce::Point<int> at) {
        w.stack.itemDragMove(DragSource(description, &w.stack, at));
        w.stack.itemDropped(DragSource(description, &w.stack, at));
    };

    CHECK(w.window.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    size_t before = undoSteps(session);
    drop("effect:transform", w.at(show.top, 1, true));
    REQUIRE(w.until([&] { return withTransform() == 5 && undoSteps(session) == before + 1; }));
    undo([&] { return withTransform() == 0; });

    // A card moves onto the cell it was dropped on, keeping its id; the other clips get copies.
    drop("fxcard:" + juce::String(int64_t(show.fx)), w.at(show.top, 1, true));
    REQUIRE(w.until([&] { return withTransform() == 5 && session.layer(show.base)->effects.empty(); }));
    CHECK(session.clip(show.cell(show.top, 1))->effects[0].id == show.fx);
    CHECK(undoSteps(session) == before + 1);
    undo([&] { return withTransform() == 0 && session.layer(show.base)->effects.size() == 1; });

    click(w.stack, w.at(show.base, 0));
    clickWith(w.stack, w.at(show.top, 1), kCmd);
    CHECK(w.window.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    REQUIRE(w.until([&] { return session.deckClips().size() == 3 && undoSteps(session) == before + 1; }));
    undo([&] { return session.deckClips().size() == 5; });

    // The top layer's two clips move two columns right, then are copied there with Option.
    click(w.stack, w.at(show.top, 0));
    clickWith(w.stack, w.at(show.top, 1), kShift);
    const juce::String top0 = "cell:" + juce::String(int64_t(show.top)) + ":0";
    drop(top0, w.at(show.top, 2, true));
    const std::set<uint64_t> moved{show.columns[2].id, show.columns[3].id};
    REQUIRE(w.until([&] { return clipsOf(show.top) == moved && undoSteps(session) == before + 1; }));
    CHECK(session.selectedClips() == Cells{show.cell(show.top, 2), show.cell(show.top, 3)});
    shoot(w.window, "64-block-moved.png");
    undo([&] { return clipsOf(show.top) == std::set<uint64_t>{show.columns[0].id, show.columns[1].id}; });
    click(w.stack, w.at(show.top, 0));
    clickWith(w.stack, w.at(show.top, 1), kShift);
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::altModifier);
    drop(top0, w.at(show.top, 2, true));
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys();
    REQUIRE(w.until([&] { return clipsOf(show.top).size() == 4 && undoSteps(session) == before + 1; }));

    // A block that would leave the grid does not move.
    drop("cell:" + juce::String(int64_t(show.top)) + ":2", w.at(show.base, 3, true));
    w.until([] { return false; }, 0.3);
    CHECK(clipsOf(show.top).size() == 4);
    CHECK(session.statusIsError());
}

TEST_CASE("panels: the Clip tab edits the fields and an effect parameter all selected clips share, one undo step each, "
          "and shows values that differ as mixed")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;
    const uint64_t first = show.columns[0].id, second = show.columns[1].id;
    apply(show.client, azl::SetClipTransport{.layer = show.base, .playMode = azl::PlayMode::Bounce, .column = first,
                                             .speed = 2.0});
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "transform", {{"scale", 1.0}}}, first});
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "transform", {{"scale", 2.0}}}, second});
    auto clip = [&](uint64_t column) { return *session.clip({show.base, column}); };
    REQUIRE(w.until([&] { return clip(second).effects.size() == 1 && clip(first).transport.speed == 2.0; }));

    click(w.stack, w.at(show.base, 0));
    clickWith(w.stack, w.at(show.base, 1), kCmd);
    azl::ui::OwnerCard& card = w.window.chain().content().ownerCard();
    REQUIRE(w.until([&] { return card.choice("Play") != nullptr; }));
    CHECK(card.choice("Play")->getSelectedItemIndex() == -1);
    CHECK(card.choice("Play")->getTextWhenNothingSelected() == "mixed");
    CHECK(card.field("Speed")->mixed());
    CHECK(card.choice("Sync")->getSelectedItemIndex() == 0);
    const azl::ParamRef amount{0, clip(second).effects[0].id, "scale"};
    CHECK(session.mixedParam(amount));
    shoot(w.window, "64-clip-tab-mixed.png");

    size_t before = undoSteps(session);
    card.choice("Play")->setSelectedItemIndex(5, juce::sendNotificationSync);
    REQUIRE(w.until([&] {
        return clip(first).transport.playMode == azl::PlayMode::RandomJump &&
               clip(second).transport.playMode == azl::PlayMode::RandomJump;
    }));
    CHECK(undoSteps(session) == before + 1);
    card.field("Speed")->onChange(1.5);
    REQUIRE(w.until([&] { return clip(first).transport.speed == 1.5 && clip(second).transport.speed == 1.5; }));
    REQUIRE(w.until([&] { return !card.field("Speed")->mixed() && card.choice("Play")->getSelectedItemIndex() == 5; }));

    // Dragging the parameter to the end of its bar sets it on both clips' Transform in one step.
    azl::ui::ChainContent& chain = w.window.chain().content();
    REQUIRE(w.until([&] { return chain.card(amount.effect) != nullptr; }));
    azl::ui::ParamRow* row = chain.card(amount.effect)->row("scale");
    const auto bar = row->barArea();
    before = undoSteps(session);
    drag(w.show.harness, *row, {bar.getCentreX(), bar.getCentreY()}, {bar.getRight() + 20, bar.getCentreY()});
    REQUIRE(w.until([&] {
        return clip(first).effects[0].params.at("scale") == 8.0 && clip(second).effects[0].params.at("scale") == 8.0;
    }));
    CHECK(!session.mixedParam(amount));
    REQUIRE(w.until([&] { return undoSteps(session) == before + 1; }));
    session.client().undo();
    REQUIRE(w.until([&] { return clip(first).effects[0].params.at("scale") == 1.0; }));
    CHECK(clip(second).effects[0].params.at("scale") == 2.0);
}

namespace {

// Every entry of the menu and its submenus has a tooltip.
void checkTips(const azl::ui::Menu& menu)
{
    for (const azl::ui::MenuEntry& entry : menu) {
        if (!entry.run && entry.sub.empty())
            continue;
        CHECK_MESSAGE(entry.tip.isNotEmpty(), entry.label);
        checkTips(entry.sub);
    }
}

const azl::ui::MenuEntry& entryAt(const azl::ui::Menu& menu, const juce::String& path)
{
    const azl::ui::MenuEntry* entry = azl::ui::menuEntry(menu, path);
    REQUIRE_MESSAGE(entry, path);
    return *entry;
}

} // namespace

TEST_CASE("panels: context menus on clips act on the whole selection, show shortcuts, tooltips and mixed states; "
          "copy, paste, duplicate and effects through menus and keys, each one undo step")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;
    apply(show.client, azl::SetClipTransport{.layer = show.base, .playMode = azl::PlayMode::Bounce, .column = show.columns[0].id});
    auto clip = [&](uint64_t layer, size_t column) { return session.clip(show.cell(layer, column)); };
    REQUIRE(w.until([&] { return clip(show.base, 0)->transport.playMode == azl::PlayMode::Bounce; }));
    auto steps = [&] { return undoSteps(session); };

    click(w.stack, w.at(show.base, 0));
    clickWith(w.stack, w.at(show.base, 1), kCmd);
    azl::ui::Menu menu = w.stack.cellMenu(show.base, 0);
    checkTips(menu);
    checkTips(w.stack.layerMenu(show.base));
    checkTips(w.window.grid().decks().deckMenu(session.composition().deck));
    checkTips(w.window.grid().columns().columnMenu(0));
    CHECK(entryAt(menu, "Copy").shortcut.isNotEmpty());
    CHECK(entryAt(menu, "Clear").shortcut.isNotEmpty());
    CHECK(entryAt(menu, "Transport/Bounce").mixed);
    CHECK(entryAt(menu, "Transport/Loop").mixed);
    CHECK(entryAt(menu, "Transport/BPM sync").ticked);
    CHECK(entryAt(menu, "Step/Layer's").ticked);
    CHECK(!entryAt(menu, "Paste").enabled);
    azl::ui::showMenu(w.stack, menu);
    w.until([] { return false; }, 0.5);
    shoot(w.window, "64-clip-menu.png");
    juce::PopupMenu::dismissAllActiveMenus();

    size_t before = steps();
    entryAt(menu, "Transport/Random Jump").run();
    REQUIRE(w.until([&] {
        return clip(show.base, 0)->transport.playMode == azl::PlayMode::RandomJump &&
               clip(show.base, 1)->transport.playMode == azl::PlayMode::RandomJump && steps() == before + 1;
    }));
    CHECK(entryAt(w.stack.cellMenu(show.base, 1), "Transport/Random Jump").ticked);
    entryAt(menu, "Step/4 bars").run();
    entryAt(menu, "Add effect/transform").run();
    REQUIRE(w.until([&] {
        return clip(show.base, 1)->step == 4 && clip(show.base, 0)->effects.size() == 1 &&
               clip(show.base, 1)->effects.size() == 1 && steps() == before + 3;
    }));

    // Copied with the menu, pasted with Cmd-V at the top layer's second clip: fresh effect ids.
    entryAt(menu, "Copy").run();
    click(w.stack, w.at(show.top, 1));
    CHECK(w.window.keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE(w.until([&] { return clip(show.top, 2) && clip(show.top, 2)->effects.size() == 1 && steps() == before + 4; }));
    CHECK(clip(show.top, 1)->effects[0].id != clip(show.base, 0)->effects[0].id);
    CHECK(session.selectedClips() == Cells{show.cell(show.top, 1), show.cell(show.top, 2)});
    shoot(w.window, "64-pasted.png");

    // Cmd-D copies the base layer's last clip into the cell right of it.
    click(w.stack, w.at(show.base, 2));
    CHECK(w.window.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE(w.until([&] { return clip(show.base, 3) != nullptr && steps() == before + 5; }));

    // The clip's effects copied by Cmd-Shift-C land on the layer from its menu; the card's menu duplicates one.
    click(w.stack, w.at(show.base, 0));
    CHECK(w.window.keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)));
    const size_t layerEffects = session.layer(show.base)->effects.size();
    entryAt(w.stack.layerMenu(show.base), "Paste effects").run();
    REQUIRE(w.until([&] { return session.layer(show.base)->effects.size() == layerEffects + 1; }));
    azl::ui::ChainContent& chain = w.window.chain().content();
    const uint64_t effect = clip(show.base, 0)->effects[0].id;
    REQUIRE(w.until([&] { return chain.card(effect) != nullptr; }));
    checkTips(chain.card(effect)->cardMenu());
    entryAt(chain.card(effect)->cardMenu(), "Duplicate").run();
    REQUIRE(w.until([&] { return clip(show.base, 0)->effects.size() == 2; }));

    entryAt(w.window.grid().columns().columnMenu(1), "Select all clips in column").run();
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 1), show.cell(show.top, 1)});
}

TEST_CASE("panels: Shift-click on a layer selects its clips; an effect type comes off the selected clips; Move clip "
          "effects to layer hoists a chain they carry alike, per deck where it differs; each one undo step")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;
    auto clip = [&](size_t column) { return session.clip(show.cell(show.base, column)); };
    auto steps = [&] { return undoSteps(session); };
    for (const auto& row : w.stack.rows())
        if (row.layer == show.base)
            clickWith(w.stack, {40, row.bounds.getY() + 8}, kShift);
    CHECK(session.selectedClips() == Cells{show.cell(show.base, 0), show.cell(show.base, 1), show.cell(show.base, 2)});

    entryAt(w.stack.cellMenu(show.base, 0), "Add effect/transform").run();
    REQUIRE(w.until([&] { return clip(2)->effects.size() == 1; }));
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "colorize", {}}, show.columns[0].id});
    REQUIRE(w.until([&] { return clip(0)->effects.size() == 2; }));
    const azl::ui::Menu menu = w.stack.cellMenu(show.base, 1);
    CHECK(entryAt(menu, "Remove effect/colorize").mixed);
    CHECK(!entryAt(menu, "Remove effect/transform").mixed);
    size_t before = steps();
    entryAt(menu, "Remove effect/colorize").run();
    REQUIRE(w.until([&] { return clip(0)->effects.size() == 1 && steps() == before + 1; }));

    // Alike on every selected clip: the first clip's Transform (same id) leads the layer's chain.
    const uint64_t transform = clip(0)->effects[0].id;
    before = steps();
    entryAt(w.stack.layerMenu(show.base), "Move clip effects to layer").run();
    REQUIRE(w.until([&] {
        const auto& effects = session.layer(show.base)->effects;
        return effects.size() == 2 && effects[0].id == transform && clip(1)->effects.empty() && steps() == before + 1;
    }));
    session.client().undo();
    REQUIRE(w.until([&] { return clip(1)->effects.size() == 1 && session.layer(show.base)->effects.size() == 1; }));

    // A second deck whose clip carries Colorize: with no clip selected, each deck gets its own chain.
    const uint64_t second = 900;
    apply(show.client, azl::AddDeck{std::nullopt, azl::Deck{0, "B", {azl::Column{second, "B1"}}},
                                    {azl::Cell{show.base, second, azl::ClipRef{show.clip.hash, "b", {}, 0, std::nullopt,
                                                                               {{0, "colorize", {}}}}}}});
    REQUIRE(w.until([&] { return session.composition().decks.size() == 2; }));
    CHECK(w.window.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    entryAt(w.stack.layerMenu(show.base), "Move clip effects to layer").run();
    REQUIRE(w.until([&] { return session.layer(show.base)->decks.size() == 2; }));
    const azl::Layer layer = *session.layer(show.base);
    const auto chainIn = [&](uint64_t deck) { return *layer.decks.at(deck).effects; };
    const uint64_t first = session.composition().decks[0].id, other = session.composition().decks[1].id;
    REQUIRE(chainIn(first).size() == 2);
    CHECK(chainIn(first)[0].type == "transform");
    CHECK(chainIn(first)[1].type == "transform");
    CHECK(chainIn(first)[1].id != show.fx);
    REQUIRE(chainIn(other).size() == 2);
    CHECK(chainIn(other)[0].type == "colorize");
    CHECK(clip(0)->effects.empty());
    CHECK(session.layer(show.base)->clips.at(second).effects.empty());
}

TEST_CASE("panels: Repeat last edit applies the last clip edit to every selected clip by effect type and parameter "
          "name, skips and counts the clips without it, one undo step")
{
    GridWindow w;
    Grid& show = w.show;
    azl::ui::Session& session = w.session;
    for (const auto& cell : {show.cell(show.base, 0), show.cell(show.base, 1), show.cell(show.top, 0)})
        apply(show.client, azl::AddEffect{cell.layer, std::nullopt, {0, "transform", {{"scale", 1.0}}}, cell.column});
    auto clip = [&](uint64_t layer, size_t column) { return session.clip(show.cell(layer, column)); };
    auto amount = [&](uint64_t layer, size_t column) { return clip(layer, column)->effects.at(0).params.at("scale"); };
    REQUIRE(w.until([&] { return clip(show.top, 0)->effects.size() == 1; }));
    auto steps = [&] { return undoSteps(session); };
    const auto cmdR = juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0);

    // The base layer's first clip alone: its Transform scale dragged to the end of the bar.
    click(w.stack, w.at(show.base, 0));
    azl::ui::ChainContent& chain = w.window.chain().content();
    const uint64_t transform = clip(show.base, 0)->effects[0].id;
    REQUIRE(w.until([&] { return chain.card(transform) != nullptr; }));
    azl::ui::ParamRow* row = chain.card(transform)->row("scale");
    const auto bar = row->barArea();
    drag(show.harness, *row, {bar.getCentreX(), bar.getCentreY()}, {bar.getRight() + 20, bar.getCentreY()});
    REQUIRE(w.until([&] { return amount(show.base, 0) == 8.0; }));

    CHECK(w.window.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    size_t before = steps();
    CHECK(w.window.keyPressed(cmdR));
    REQUIRE(w.until([&] { return amount(show.base, 1) == 8.0 && amount(show.top, 0) == 8.0 && steps() == before + 1; }));
    CHECK(session.status().contains("on 3 clips"));
    CHECK(session.status().contains("skipped 2"));
    shoot(w.window, "64-repeated.png");

    // A transport field set on one clip, repeated from the menu on all of them.
    click(w.stack, w.at(show.base, 2));
    azl::ui::OwnerCard& card = chain.ownerCard();
    REQUIRE(w.until([&] { return card.choice("Play") != nullptr && session.selectedClips().size() == 1; }));
    card.choice("Play")->setSelectedItemIndex(1, juce::sendNotificationSync);
    REQUIRE(w.until([&] { return clip(show.base, 2)->transport.playMode == azl::PlayMode::Bounce; }));
    CHECK(w.window.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    entryAt(w.stack.cellMenu(show.base, 0), "Repeat last edit").run();
    REQUIRE(w.until([&] {
        for (const auto& cell : session.deckClips())
            if (session.clip(cell)->transport.playMode != azl::PlayMode::Bounce)
                return false;
        return true;
    }));

    // Removing an effect with its card's button, repeated: every Transform goes.
    click(w.stack, w.at(show.base, 0));
    REQUIRE(w.until([&] { return chain.card(transform) != nullptr; }));
    chain.card(transform)->removeButton().triggerClick();
    REQUIRE(w.until([&] { return clip(show.base, 0)->effects.empty(); }));
    CHECK(w.window.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    before = steps();
    CHECK(w.window.keyPressed(cmdR));
    REQUIRE(w.until([&] {
        return clip(show.base, 1)->effects.empty() && clip(show.top, 0)->effects.empty() && steps() == before + 1;
    }));
    session.client().undo();
    REQUIRE(w.until([&] { return clip(show.base, 1)->effects.size() == 1 && clip(show.top, 0)->effects.size() == 1; }));
}

TEST_CASE("panels: a full window paint of a filled grid stays within a frame")
{
    Show show;
    for (int i = 0; i < 4; ++i)
        apply(show.client, azl::AddLayer{0, std::nullopt, {.name = "layer " + std::to_string(i)}});
    const uint64_t deck = show.document().decks[0].id;
    while (show.document().decks[0].columns.size() < 40)
        apply(show.client, azl::AddColumn{deck, std::nullopt, {}, {}});
    for (const azl::Layer& layer : show.document().layers)
        for (const azl::Column& column : show.document().decks[0].columns)
            apply(show.client, azl::SetSlot{layer.id, column.id, show.clip});
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(show.harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(show.harness, [&] { return window.grid().stack().rows().size() == 5; }));

    window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f);
    const auto start = std::chrono::steady_clock::now();
    constexpr int kPaints = 20;
    for (int i = 0; i < kPaints; ++i)
        window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kPaints;
    MESSAGE("full window paint: " << ms << " ms");
    CHECK(ms < 16.0);
}

TEST_CASE("panels: a Sources tab generator drops into a cell like a file and shows its settings in the Clip tab")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));

    window.browser().selectTab(2);
    std::vector<std::string> listed;
    for (const auto& [bounds, type] : window.browser().effectItems())
        listed.push_back(type);
    std::vector<std::string> sources = azl::generatorTypes();
    sources.push_back(azl::ui::kTextSource);
    CHECK(listed == sources);

    azl::ui::LayerStack& stack = window.grid().stack();
    int top = 0;
    for (const auto& r : stack.rows())
        if (r.layer == show.base)
            top = r.bounds.getY();
    const uint64_t column = show.columns[1].id;
    const juce::Point<int> cell{stack.columnArea(1).getCentreX(), top + 10};
    const size_t before = undoSteps(session);
    stack.itemDragMove(DragSource("source:lines", &window.browser(), cell));
    stack.itemDropped(DragSource("source:lines", &window.browser(), cell));
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.contains(column); }));
    const azl::ClipRef clip = show.layer().clips.at(column);
    CHECK(azl::isGeneratorClip(clip));
    CHECK(clip.name == "Lines");
    CHECK(clip.effects.size() == 1);
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));

    // Its parameters show under Source in the clip card, not as a card in the chain; the media-only fields are gone.
    session.selectClip({show.base, column});
    session.setChainTab(2);
    azl::ui::ChainContent& chain = window.chain().content();
    azl::ui::OwnerCard& card = chain.ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.sourceRow("count") != nullptr; }));
    CHECK(card.hasSource());
    for (const char* param : {"width", "angle", "speed", "softness"})
        CHECK(card.sourceRow(param));
    REQUIRE(card.swatchRow("red") != nullptr);
    CHECK(card.swatchRow("red")->getBottom() <= card.sourceRow("red")->getY());
    CHECK(chain.card(clip.effects[0].id) == nullptr);
    for (const char* label : {"Play", "Sync", "Length"})
        CHECK(card.choice(label) == nullptr);
    for (const char* label : {"Speed", "In", "Out"})
        CHECK(card.field(label) == nullptr);
    for (const char* label : {"Trigger", "Step", "Transition"})
        CHECK(card.choice(label) != nullptr);
    shoot(window, "sources.png");

    // A parameter edit is one undo step; an effect dropped first in the chain lands after the generator, and its
    // header names it without a stage kind.
    const ParamRef count{0, clip.effects[0].id, "count"};
    size_t steps = undoSteps(session);
    session.setParam(count, 4.0);
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.at(column).effects[0].params["count"] == 4.0; }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    CHECK(chain.gapAt(0) == 1);
    chain.drop("effect:transform", chain.gapAt(0));
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.at(column).effects.size() == 2; }));
    const azl::ClipRef withTransform = show.layer().clips.at(column);
    CHECK(withTransform.effects[0].type == "lines");
    CHECK(withTransform.effects[1].type == "transform");
    REQUIRE(runUntil(harness, [&] { return chain.card(withTransform.effects[1].id) != nullptr; }));
    CHECK(chain.card(withTransform.effects[1].id)->headerText() == "transform");
    CHECK(chain.card(withTransform.effects[0].id) == nullptr);
    CHECK(chain.gapAt(chain.card(withTransform.effects[1].id)->getRight() + 1) == 2);
}

TEST_CASE("panels: Text in the Sources tab drops or double-clicks into a cell, and the clip card edits it")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    azl::ui::BrowserPane& browser = window.browser();
    browser.selectTab(2);
    REQUIRE_FALSE(browser.effectItems().empty());
    CHECK(browser.effectItems().back().second == azl::ui::kTextSource);
    const juce::Point<int> textRow = browser.effectItems().back().first.getCentre();
    const azl::TextClip defaults{.text = "TEXT"};
    auto settled = [&](size_t before) {
        REQUIRE(runUntil(harness, [&] { return undoSteps(session) >= before + 1; }));
        runUntil(harness, [] { return false; }, 0.3);
        CHECK(undoSteps(session) == before + 1);
    };

    // Dropped onto an empty cell.
    azl::ui::LayerStack& stack = window.grid().stack();
    int top = 0;
    for (const auto& r : stack.rows())
        if (r.layer == show.base)
            top = r.bounds.getY();
    const uint64_t column = show.columns[1].id;
    const juce::Point<int> cell{stack.columnArea(1).getCentreX(), top + 10};
    size_t before = undoSteps(session);
    stack.itemDragMove(DragSource("source:text", &browser, cell));
    stack.itemDropped(DragSource("source:text", &browser, cell));
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.contains(column); }));
    const azl::ClipRef dropped = show.layer().clips.at(column);
    CHECK(dropped.name == "Text");
    CHECK(dropped.hash.empty());
    REQUIRE(dropped.text);
    CHECK(*dropped.text == defaults);
    settled(before);

    // Double-clicked with the first clip selected: a text clip takes that cell.
    const uint64_t first = show.columns[0].id;
    session.selectClip({show.base, first});
    before = undoSteps(session);
    browser.mouseDoubleClick(mouse(browser, textRow, textRow));
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.at(first).text.has_value(); }));
    CHECK(*show.layer().clips.at(first).text == defaults);
    CHECK(show.layer().clips.size() == 2);
    settled(before);

    // The clip card's Source section: text, size and colour shown, the rest folded.
    session.selectClip({show.base, column});
    session.setChainTab(2);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.textField() != nullptr; }));
    CHECK(card.hasSource());
    CHECK(card.textField()->getText() == "TEXT");
    for (const char* label : {"Play", "Sync", "Length"})
        CHECK(card.choice(label) == nullptr);
    for (const char* label : {"Speed", "In", "Out"})
        CHECK(card.field(label) == nullptr);
    CHECK(card.choice("Trigger") != nullptr);
    CHECK(card.field("Size")->isVisible());
    CHECK(card.swatchRow(azl::kTextColour)->isVisible());
    CHECK_FALSE(card.swatchRow(azl::kTextOutline)->isVisible());
    CHECK_FALSE(card.choice("Font")->isVisible());
    card.moreToggle()->triggerClick();
    REQUIRE(runUntil(harness, [&] { return card.choice("Font")->isVisible() && card.field("Outline")->isVisible(); }));
    CHECK(card.swatchRow(azl::kTextOutline)->isVisible());
    shoot(window, "98-text-clip-card.png");

    auto text = [&] { return *show.layer().clips.at(column).text; };
    auto step = [&](const std::function<void()>& act, const std::function<bool(const azl::TextClip&)>& done) {
        const size_t steps = undoSteps(session);
        act();
        REQUIRE(runUntil(harness, [&] { return done(text()); }));
        settled(steps);
    };
    step([&] {
        card.textField()->setText("Hello\\nWorld");
        card.textField()->onReturnKey();
    }, [](const azl::TextClip& t) { return t.text == "Hello\nWorld"; });
    step([&] { card.field("Size")->onChange(20.0); }, [](const azl::TextClip& t) { return t.size == 0.2; });
    auto pick = [&](const char* group, const char* hex) {
        azl::ui::Swatch& swatch = card.swatchRow(group)->swatch();
        swatch.openPicker();
        REQUIRE(swatch.picker() != nullptr);
        swatch.picker()->hexField().setText(hex);
        swatch.picker()->hexField().onReturnKey();
    };
    step([&] { pick(azl::kTextColour, "#00ffff"); },
         [](const azl::TextClip& t) { return t.color == std::array<double, 4>{0.0, 1.0, 1.0, 1.0}; });
    card.swatchRow(azl::kTextColour)->swatch().closePicker();
    juce::ComboBox* font = card.choice("Font");
    int other = 0;
    while (other < font->getNumItems() && font->getItemText(other) == "Helvetica")
        ++other;
    REQUIRE(other < font->getNumItems());
    const std::string otherFont = font->getItemText(other).toStdString();
    step([&] { font->setSelectedItemIndex(other, juce::sendNotificationSync); },
         [&](const azl::TextClip& t) { return t.font == otherFont; });
    step([&] { card.choice("Align")->setSelectedItemIndex(0, juce::sendNotificationSync); },
         [](const azl::TextClip& t) { return t.align == azl::TextAlign::Left; });
    step([&] { card.field("X")->onChange(10.0); }, [](const azl::TextClip& t) { return t.x == 0.1; });
    step([&] { card.field("Y")->onChange(80.0); }, [](const azl::TextClip& t) { return t.y == 0.8; });
    step([&] { card.field("Outline")->onChange(10.0); }, [](const azl::TextClip& t) { return t.outline == 0.1; });
    step([&] { pick(azl::kTextOutline, "#00ff00"); },
         [](const azl::TextClip& t) { return t.outlineColor == std::array<double, 4>{0.0, 1.0, 0.0, 1.0}; });
    card.swatchRow(azl::kTextOutline)->swatch().closePicker();
    REQUIRE(runUntil(harness, [&] { return card.textField()->getText() == "Hello\\nWorld"; }));
    CHECK(card.field("Size")->value() == 20.0);
}

namespace {

const char* kStripperPresetFile = R"(<?xml version="1.0" encoding="UTF-8"?>
<Preset name="P" className="Stripper">
  <RenderPass name="Stripper" type="DryWetEffect" baseType="DryWetEffect">
    <Params name="Params"><ParamRange name="Opacity" default="1" value="0.75"/></Params>
    <RenderPass name="Stripper" type="FFGLEffect" baseType="Effect">
      <Params name="Params"><ParamRange name="Number" default="0.1" value="0.3"/></Params>
    </RenderPass>
    <ChoosableMixer name="Mixer"><RenderPass name="Add" type="Add" baseType="Mixer"/></ChoosableMixer>
  </RenderPass>
</Preset>
)";

using Row = azl::ui::PresetRow;

std::vector<std::string> labels(const azl::ui::PresetTree& tree, Row::Kind kind, const std::string& type = {})
{
    std::vector<std::string> out;
    for (const auto& [bounds, row] : tree.rows())
        if (row.kind == kind && (type.empty() || row.effectType == type))
            out.push_back(kind == Row::Kind::Effect ? row.effectType : row.label.toStdString());
    return out;
}

// The presets the tree lists under one effect's group.
std::vector<std::string> presetsIn(const azl::ui::PresetTree& tree, const std::string& type, const std::string& group)
{
    std::vector<std::string> out;
    for (const auto& [bounds, row] : tree.rows())
        if (row.kind == Row::Kind::Preset && row.effectType == type && row.group == group)
            out.push_back(row.label.toStdString());
    return out;
}

std::optional<std::pair<juce::Rectangle<int>, Row>> rowOf(const azl::ui::PresetTree& tree, Row::Kind kind,
                                                          const std::string& label, const std::string& type = {})
{
    for (const auto& row : tree.rows())
        if (row.second.kind == kind &&
            (kind == Row::Kind::Effect ? row.second.effectType == label : row.second.label == juce::String(label)) &&
            (type.empty() || row.second.effectType == type))
            return row;
    return std::nullopt;
}

void clickHeader(azl::ui::PresetTree& tree, Row::Kind kind, const std::string& label, const std::string& type = {})
{
    const auto row = rowOf(tree, kind, label, type);
    REQUIRE(row);
    click(tree, {row->first.getX() + 6, row->first.getCentreY()});
}

std::vector<std::string> names(const std::vector<azl::ui::PresetEntry>& entries, const std::string& type,
                               const std::string& group)
{
    std::vector<std::string> out;
    for (const auto& entry : entries)
        if (entry.effectType == type && entry.group == group)
            out.push_back(entry.name);
    return out;
}

struct PresetShow {
    Show show;
    TempDir presets, settingsDir;
    azl::WebSocketServer server{show.harness.document(), 0};
    uint64_t stripper = 0;

    PresetShow()
    {
        fs::create_directories(presets.path / "Video Effects/Stripper");
        std::ofstream(presets.path / "Video Effects/Stripper/Wide.xml") << kStripperPresetFile;
        Client client(show.harness.document());
        client.send({{"type", "preset"}, {"op", "import"}, {"roots", {presets.path.string()}}});
        client.next();
        stripper = apply(client, azl::AddEffect{show.base, std::nullopt, {0, "classicStripper", {{"number", 0.6}}}})
                       ["effect"]["id"];
        client.send({{"type", "preset"}, {"op", "save"}, {"effect", stripper}, {"name", "My Bars"}});
        client.next();
    }

    std::unique_ptr<juce::PropertiesFile> settings() const
    {
        return std::make_unique<juce::PropertiesFile>(juce::File(juce::String((settingsDir.path / "UI.settings").string())),
                                                      juce::PropertiesFile::Options());
    }
};

} // namespace

TEST_CASE("presets: the Effects tab groups presets by effect, then Built-in Classic, Imported and Mine, with no empty "
          "headers, open state kept in the settings across a restart, and a search that opens what it finds")
{
    PresetShow p;
    azl::EngineHarness& harness = p.show.harness;
    const std::vector<std::string> looks = [] {
        std::vector<std::string> out;
        for (const azl::ClassicPreset& look : azl::classicBuiltinPresets())
            if (look.effectType == "classicStripper")
                out.push_back(look.name);
        std::sort(out.begin(), out.end());
        return out;
    }();
    {
        azl::ui::Session session("127.0.0.1", p.server.port());
        azl::ui::MainComponent window(session, p.settings());
        window.setSize(1600, 950);
        REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
        REQUIRE(runUntil(harness, [&] { return !names(session.client().presetEntries(), "classicStripper", "mine").empty(); }));
        CHECK(names(session.client().presetEntries(), "classicStripper", "imported") == std::vector<std::string>{"Wide"});

        window.browser().selectTab(1);
        azl::ui::PresetTree& tree = window.browser().effects();
        CHECK(tree.isVisible());
        CHECK(labels(tree, Row::Kind::Section) == std::vector<std::string>{"Ours", "Classic", "ISF"});
        CHECK(labels(tree, Row::Kind::Effect) == azl::effectTypes());
        CHECK(rowOf(tree, Row::Kind::Effect, "classicShiftRgb")->second.label == "Classic Shift RGB");
        CHECK(labels(tree, Row::Kind::Group).empty());
        // Our effects sit in Ours, the pack in Classic, imported shaders in ISF.
        std::string section;
        for (const auto& [bounds, row] : tree.rows()) {
            if (row.kind == Row::Kind::Section)
                section = row.label.toStdString();
            else if (row.kind == Row::Kind::Effect)
                CHECK(section == (azl::findEffect(row.effectType)->category.empty() ? "Ours"
                                                                                    : azl::findEffect(row.effectType)->category));
        }

        // An effect opens from its triangle into one header per group that holds presets.
        clickHeader(tree, Row::Kind::Effect, "classicStripper");
        clickHeader(tree, Row::Kind::Effect, "classicColorize");
        clickHeader(tree, Row::Kind::Effect, "transform");
        CHECK(labels(tree, Row::Kind::Group, "classicStripper") ==
              std::vector<std::string>{"Built-in Classic", "Imported", "Mine"});
        CHECK(labels(tree, Row::Kind::Group, "classicColorize") == std::vector<std::string>{"Built-in Classic"});
        CHECK(labels(tree, Row::Kind::Group, "transform").empty());
        CHECK_FALSE(session.groupOpen("effect/transform", false));
        std::vector<std::string> builtin = presetsIn(tree, "classicStripper", "builtin");
        std::sort(builtin.begin(), builtin.end());
        CHECK(builtin == looks);
        CHECK(presetsIn(tree, "classicStripper", "imported") == std::vector<std::string>{"Wide"});
        CHECK(presetsIn(tree, "classicStripper", "mine") == std::vector<std::string>{"My Bars"});
        // Every preset row follows its own effect and group header.
        std::string effect, group;
        for (const auto& [bounds, row] : tree.rows()) {
            if (row.kind == Row::Kind::Effect)
                effect = row.effectType;
            if (row.kind == Row::Kind::Group) {
                CHECK(row.effectType == effect);
                CHECK(row.count > 0);
                group = row.group;
            }
            if (row.kind == Row::Kind::Preset) {
                CHECK(row.preset.effectType == effect);
                CHECK(row.preset.group == group);
            }
        }
        // A section folds its effects, a group header its presets; the state goes to the settings file.
        clickHeader(tree, Row::Kind::Section, "Ours");
        CHECK(labels(tree, Row::Kind::Effect, "transform").empty());
        shoot(window, "presets_browser.png");
        clickHeader(tree, Row::Kind::Group, "Built-in Classic", "classicStripper");
        CHECK(presetsIn(tree, "classicStripper", "builtin").empty());
        CHECK(presetsIn(tree, "classicStripper", "mine") == std::vector<std::string>{"My Bars"});

        // A search keeps what matches by preset or effect name and opens it, folded or not.
        window.browser().search().setText("bars");
        CHECK(labels(tree, Row::Kind::Effect) == std::vector<std::string>{"classicStripper"});
        CHECK_FALSE(presetsIn(tree, "classicStripper", "builtin").empty());
        for (const std::string& name : presetsIn(tree, "classicStripper", "builtin"))
            CHECK(juce::String(name).containsIgnoreCase("bars"));
        CHECK(labels(tree, Row::Kind::Group, "classicStripper") == std::vector<std::string>{"Built-in Classic", "Mine"});
        shoot(window, "presets_search.png");
        window.browser().search().setText("Kaleido");
        const std::vector<std::string> kaleido = labels(tree, Row::Kind::Effect);
        CHECK(std::find(kaleido.begin(), kaleido.end(), "classicKaleidoscope") != kaleido.end());
        for (const std::string& type : kaleido)
            CHECK(juce::String(type).containsIgnoreCase("kaleido"));
        CHECK(presetsIn(tree, "classicKaleidoscope", "builtin").size() ==
              names(session.client().presetEntries(), "classicKaleidoscope", "builtin").size());
        window.browser().search().setText("trails");
        CHECK(labels(tree, Row::Kind::Effect) == std::vector<std::string>{"trails", "classicTrails"});
        CHECK(labels(tree, Row::Kind::Group).empty());
        window.browser().search().setText("");
        CHECK(presetsIn(tree, "classicStripper", "builtin").empty());
    }

    // A restart reads the same state back.
    azl::ui::Session session("127.0.0.1", p.server.port());
    azl::ui::MainComponent window(session, p.settings());
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(harness, [&] { return !session.client().presetEntries().empty(); }));
    window.browser().selectTab(1);
    azl::ui::PresetTree& tree = window.browser().effects();
    CHECK(session.groupOpen("effect/classicStripper", false));
    CHECK_FALSE(session.groupOpen("group/classicStripper/builtin", true));
    CHECK_FALSE(session.groupOpen("section/Ours", true));
    CHECK(labels(tree, Row::Kind::Effect, "transform").empty());
    CHECK(presetsIn(tree, "classicStripper", "builtin").empty());
    CHECK(presetsIn(tree, "classicStripper", "imported") == std::vector<std::string>{"Wide"});
    CHECK(labels(tree, Row::Kind::Group, "classicColorize") == std::vector<std::string>{"Built-in Classic"});

    // The add-effect menu groups them the same way.
    const azl::ui::Menu menu = azl::ui::addEffectMenu(session, [](const azl::Effect&) {});
    CHECK(azl::ui::menuEntry(menu, "classicStripper/Imported/Wide"));
    CHECK(azl::ui::menuEntry(menu, "classicStripper/Mine/My Bars"));
    CHECK_FALSE(azl::ui::menuEntry(menu, "classicColorize/Mine"));
    CHECK(azl::ui::menuEntry(menu, "transform")->sub.empty());
}

TEST_CASE("presets: a preset drags onto a clip, a layer and a selection with its settings, double-click adds it, and "
          "each is one undo step")
{
    PresetShow p;
    Show& show = p.show;
    azl::EngineHarness& harness = show.harness;
    Client client(harness.document());
    apply(client, azl::SetSlot{show.base, show.columns[1].id, show.clip});
    azl::ui::Session session("127.0.0.1", p.server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(harness, [&] { return !names(session.client().presetEntries(), "classicStripper", "imported").empty(); }));
    const auto entries = session.client().presetEntries();
    const auto wide = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.name == "Wide"; });
    REQUIRE(wide != entries.end());
    const juce::String payload = azl::ui::presetDrag(*wide);
    REQUIRE(azl::ui::draggedEffect(payload));
    CHECK(azl::ui::draggedEffect(payload)->params == wide->params);

    azl::ui::LayerStack& stack = window.grid().stack();
    int top = 0;
    for (const auto& r : stack.rows())
        if (r.layer == show.base)
            top = r.bounds.getY();
    auto dropOn = [&](juce::Component& target, juce::Point<int> at) {
        auto* dropTarget = dynamic_cast<juce::DragAndDropTarget*>(&target);
        REQUIRE(dropTarget->isInterestedInDragSource(DragSource(payload, &window.browser(), at)));
        dropTarget->itemDragMove(DragSource(payload, &window.browser(), at));
        dropTarget->itemDropped(DragSource(payload, &window.browser(), at));
    };
    auto stripperOn = [&](const std::vector<azl::Effect>& chain) {
        return std::count_if(chain.begin(), chain.end(), [&](const azl::Effect& e) {
            return e.type == "classicStripper" && e.params.at("number") == wide->params.at("number");
        });
    };

    // A clip.
    size_t before = undoSteps(session);
    const uint64_t first = show.columns[0].id, second = show.columns[1].id;
    dropOn(stack, {stack.columnArea(0).getCentreX(), top + 10});
    REQUIRE(runUntil(harness, [&] { return stripperOn(show.layer().clips.at(first).effects) == 1; }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));

    // A layer.
    before = undoSteps(session);
    dropOn(stack, stack.controlArea(show.base).getCentre());
    REQUIRE(runUntil(harness, [&] { return stripperOn(show.layer().effects) == 1; }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));

    // A selection of two clips: both, one step.
    session.selectClip({show.base, first});
    session.toggleClip({show.base, second});
    before = undoSteps(session);
    dropOn(stack, {stack.columnArea(1).getCentreX(), top + 10});
    REQUIRE(runUntil(harness, [&] {
        return stripperOn(show.layer().clips.at(first).effects) == 2 && stripperOn(show.layer().clips.at(second).effects) == 1;
    }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));

    // The chain and the Composition header take it too.
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
    dropOn(chain, {chain.getWidth() - 20, 60});
    REQUIRE(runUntil(harness, [&] { return stripperOn(show.layer().effects) == 2; }));
    dropOn(window.grid().columns(), {30, 8});
    REQUIRE(runUntil(harness, [&] { return stripperOn(show.document().effects) == 1; }));

    // Double-click on a preset row adds it to the chain shown.
    window.browser().selectTab(1);
    azl::ui::PresetTree& tree = window.browser().effects();
    session.setGroupOpen("effect/classicStripper", true);
    const auto row = rowOf(tree, Row::Kind::Preset, "Wide");
    REQUIRE(row);
    tree.mouseDoubleClick(mouse(tree, row->first.getCentre(), row->first.getCentre()));
    REQUIRE(runUntil(harness, [&] { return stripperOn(show.layer().effects) == 3; }));
    // And the effect row adds it plain.
    const auto colorize = rowOf(tree, Row::Kind::Effect, "classicColorize");
    REQUIRE(colorize);
    tree.mouseDoubleClick(mouse(tree, colorize->first.getCentre(), colorize->first.getCentre()));
    REQUIRE(runUntil(harness, [&] {
        const auto effects = show.layer().effects;
        return std::any_of(effects.begin(), effects.end(), [](const azl::Effect& e) { return e.type == "classicColorize"; });
    }));
}

TEST_CASE("presets: an effect card's Presets box lists its groups, applies a preset on a click, saves to Mine, and "
          "renames and deletes only in Mine")
{
    PresetShow p;
    Show& show = p.show;
    azl::EngineHarness& harness = show.harness;
    azl::ui::Session session("127.0.0.1", p.server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(harness, [&] { return !names(session.client().presetEntries(), "classicStripper", "mine").empty(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(p.stripper) != nullptr; }));
    azl::ui::EffectCard& card = *chain.card(p.stripper);

    card.showPresets();
    REQUIRE(card.presetPanel() != nullptr);
    azl::ui::PresetTree& tree = card.presetPanel()->tree();
    CHECK(labels(tree, Row::Kind::Section).empty());
    CHECK(labels(tree, Row::Kind::Effect).empty());
    CHECK(labels(tree, Row::Kind::Group) == std::vector<std::string>{"Built-in Classic", "Imported", "Mine"});
    CHECK(card.presetPanel()->saveAs().isVisible());
    runUntil(harness, [] { return false; }, 0.3);
    shoot(window, "presets_card.png");

    // The groups' state is the browser's.
    clickHeader(tree, Row::Kind::Group, "Imported");
    CHECK_FALSE(session.groupOpen("group/classicStripper/imported", true));
    CHECK(presetsIn(tree, "classicStripper", "imported").empty());
    card.presetPanel()->search().setText("wide");
    CHECK(presetsIn(tree, "classicStripper", "imported") == std::vector<std::string>{"Wide"});
    CHECK(presetsIn(tree, "classicStripper", "builtin") == std::vector<std::string>{"Wide Bars"});
    CHECK(labels(tree, Row::Kind::Group) == std::vector<std::string>{"Built-in Classic", "Imported"});
    card.presetPanel()->search().setText("");

    // Built-in and imported presets cannot be renamed or deleted; Mine can.
    const auto look = rowOf(tree, Row::Kind::Preset, "Two-Way Bars");
    REQUIRE(look);
    const azl::ui::Menu lookMenu = tree.presetMenu(look->second);
    CHECK_FALSE(azl::ui::menuEntry(lookMenu, "Rename...")->enabled);
    CHECK_FALSE(azl::ui::menuEntry(lookMenu, "Delete")->enabled);
    const auto mine = rowOf(tree, Row::Kind::Preset, "My Bars");
    REQUIRE(mine);
    const azl::ui::Menu mineMenu = tree.presetMenu(mine->second);
    CHECK(azl::ui::menuEntry(mineMenu, "Rename...")->enabled);
    CHECK(azl::ui::menuEntry(mineMenu, "Delete")->enabled);

    // A click applies a preset to the effect and closes the box.
    click(tree, look->first.getCentre());
    REQUIRE(runUntil(harness, [&] {
        const azl::Composition document = show.document();
        const azl::Effect* effect = azl::effectById(document, p.stripper);
        return effect && effect->params.at("number") == look->second.preset.params.at("number");
    }));
    REQUIRE(runUntil(harness, [&] { return card.presetPanel() == nullptr; }));

    // Save as goes to Mine; rename and delete act there, and an emptied Mine shows no header.
    session.client().savePreset(p.stripper, "Second");
    REQUIRE(runUntil(harness, [&] { return names(session.client().presetEntries(), "classicStripper", "mine").size() == 2; }));
    session.client().renamePreset("classicStripper", "Second", "Third");
    REQUIRE(runUntil(harness, [&] {
        return names(session.client().presetEntries(), "classicStripper", "mine") == std::vector<std::string>{"My Bars", "Third"};
    }));
    card.showPresets();
    REQUIRE(card.presetPanel() != nullptr);
    azl::ui::PresetTree& again = card.presetPanel()->tree();
    CHECK(presetsIn(again, "classicStripper", "mine") == std::vector<std::string>{"My Bars", "Third"});
    azl::ui::menuEntry(again.presetMenu(rowOf(again, Row::Kind::Preset, "My Bars")->second), "Delete")->run();
    session.client().deletePreset("classicStripper", "Third");
    REQUIRE(runUntil(harness, [&] { return names(session.client().presetEntries(), "classicStripper", "mine").empty(); }));
    CHECK(labels(again, Row::Kind::Group) == std::vector<std::string>{"Built-in Classic", "Imported"});
    card.closePresets();
    REQUIRE(runUntil(harness, [&] { return card.presetPanel() == nullptr; }));
}

namespace {

struct ColourWindow {
    Show show;
    uint64_t threshold = [this] {
        apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "classicThreshold", {}}});
        return show.layer().effects.back().id;
    }();
    azl::WebSocketServer server{show.harness.document(), 0};
    azl::ui::Session session{"127.0.0.1", server.port()};
    azl::ui::MainComponent window{session};

    ColourWindow()
    {
        window.setSize(1600, 950);
        REQUIRE(until([&] { return session.client().synced() && azl::effectById(session.composition(), threshold); }));
        session.selectLayer(show.base);
        REQUIRE(until([&] { return chain().card(threshold) != nullptr; }));
    }

    bool until(const std::function<bool()>& done, double seconds = 20.0) { return runUntil(show.harness, done, seconds); }
    azl::ui::ChainContent& chain() { return window.chain().content(); }
    double param(uint64_t effect, const std::string& name)
    {
        const azl::Composition document = show.document();
        const azl::Effect* e = azl::effectById(document, effect);
        return e ? e->params.at(name) : -1.0;
    }
};

bool near(double a, double b, double within = 1.0 / 255.0)
{
    return std::abs(a - b) <= within;
}

} // namespace

TEST_CASE("colours: every colour group has a swatch; a drag in its picker moves the sliders in one undo step, a slider "
          "moves the swatch, hex and recent colours apply")
{
    ColourWindow w;
    azl::ui::Session& session = w.session;
    azl::ui::EffectCard& card = *w.chain().card(w.threshold);
    REQUIRE(card.swatchRow("color1Red") != nullptr);
    REQUIRE(card.swatchRow("color2Red") != nullptr);
    CHECK(card.swatchRow("color1Red")->getBottom() <= card.row("color1Red")->getY());
    azl::ui::Swatch& swatch = card.swatchRow("color2Red")->swatch();
    REQUIRE(swatch.shown());
    CHECK(*swatch.shown() == azl::Rgba{0.0, 0.0, 0.0, 1.0});

    swatch.openPicker();
    REQUIRE(swatch.picker() != nullptr);
    azl::ui::ColourPicker& picker = *swatch.picker();
    CHECK(!picker.alphaArea().isEmpty());

    // A drag across the square to full saturation and value at hue 0: one undo step, the sliders follow.
    const size_t before = undoSteps(session);
    const auto square = picker.squareArea();
    drag(w.show.harness, picker, square.getCentre(), {square.getRight() + 10, square.getY() - 10});
    REQUIRE(w.until([&] {
        return near(w.param(w.threshold, "color2Red"), 1.0) && near(w.param(w.threshold, "color2Green"), 0.0);
    }));
    CHECK(near(w.param(w.threshold, "color2Blue"), 0.0));
    REQUIRE(w.until([&] { return undoSteps(session) == before + 1; }));
    REQUIRE(w.until([&] { return swatch.shown() && near(swatch.shown()->r, 1.0); }));
    REQUIRE(!session.recentColours().empty());
    CHECK(azl::toHex(session.recentColours().front(), false) == "#ff0000");
    session.client().undo();
    REQUIRE(w.until([&] { return w.param(w.threshold, "color2Red") < 0.6; }));
    session.client().redo();
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Red"), 1.0); }));

    // A slider moves the swatch and the open picker.
    azl::ui::ParamRow& green = *card.row("color2Green");
    drag(w.show.harness, green, {green.barArea().getX() + 2, green.barArea().getCentreY()},
         {green.barArea().getRight() + 10, green.barArea().getCentreY()});
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Green"), 1.0); }));
    REQUIRE(w.until([&] { return swatch.shown() && near(swatch.shown()->g, 1.0); }));
    CHECK(near(azl::toRgba(picker.colour()).g, 1.0));
    CHECK(picker.hexField().getText() == "#ffff00ff");

    // Hex: six digits keep the alpha, eight set it, anything else is put back.
    picker.hexField().setText("#336699");
    picker.hexField().onReturnKey();
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Blue"), 0x99 / 255.0); }));
    CHECK(near(w.param(w.threshold, "color2Red"), 0x33 / 255.0));
    CHECK(near(w.param(w.threshold, "color2Alpha"), 1.0));
    picker.hexField().setText("33669980");
    picker.hexField().onReturnKey();
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Alpha"), 0x80 / 255.0); }));
    picker.hexField().setText("nonsense");
    picker.hexField().onReturnKey();
    CHECK(picker.hexField().getText() == "#33669980");

    // A recent colour applies with one click.
    REQUIRE(session.recentColours().size() >= 3);
    CHECK(azl::toHex(session.recentColours()[2], false) == "#ff0000");
    click(picker, picker.recentArea(2).getCentre());
    REQUIRE(w.until([&] {
        return near(w.param(w.threshold, "color2Red"), 1.0) && near(w.param(w.threshold, "color2Blue"), 0.0);
    }));
    CHECK(azl::toHex(session.recentColours().front(), false) == "#ff0000");

    // The slider view shows the colour's own parameter rows.
    picker.showSliders(true);
    REQUIRE(picker.rows().size() == 4);
    CHECK(picker.rows()[3]->ref().param == "color2Alpha");
    CHECK(picker.squareArea().isEmpty());
    picker.showSliders(false);
    runUntil(w.show.harness, [] { return false; }, 0.3);
    shoot(w.window, "75-colour-picker.png");
    swatch.closePicker();
    REQUIRE(w.until([&] { return swatch.picker() == nullptr; }));
}

TEST_CASE("colours: a modulated colour shows its driven value on the swatch; a text clip's colours have swatches")
{
    ColourWindow w;
    azl::ui::Session& session = w.session;
    azl::ui::Swatch& swatch = w.chain().card(w.threshold)->swatchRow("color1Red")->swatch();
    const azl::ParamRef red{0, w.threshold, "color1Red"};
    session.animate(red, "beat");
    REQUIRE(w.until([&] {
        const auto driven = session.drivenValue(red);
        return driven && *driven < 0.95 && swatch.shown() && near(swatch.shown()->r, *driven, 0.02);
    }));

    const uint64_t column = w.show.columns[1].id;
    session.client().edit(
        azl::SetSlot{w.show.base, column, azl::ClipRef{.name = "Text", .text = azl::TextClip{.text = "hi"}}});
    REQUIRE(w.until([&] { return session.clip({w.show.base, column}) != nullptr; }));
    session.selectClip({w.show.base, column});
    azl::ui::OwnerCard& owner = w.chain().ownerCard();
    REQUIRE(w.until([&] { return owner.swatchRow(azl::kTextOutline) != nullptr; }));
    azl::ui::Swatch& text = owner.swatchRow(azl::kTextColour)->swatch();
    CHECK(*text.shown() == azl::Rgba{1.0, 1.0, 1.0, 1.0});
    text.openPicker();
    REQUIRE(text.picker() != nullptr);
    text.picker()->hexField().setText("#ff8000");
    text.picker()->hexField().onReturnKey();
    auto textColour = [&] { return session.clip({w.show.base, column})->text->color; };
    REQUIRE(w.until([&] { return near(textColour()[1], 0x80 / 255.0); }));
    CHECK(textColour()[0] == 1.0);
    CHECK(textColour()[3] == 1.0);
    text.picker()->showSliders(true);
    REQUIRE(text.picker()->fields().size() == 4);
    text.picker()->fields()[3]->onChange(50.0);
    REQUIRE(w.until([&] { return near(textColour()[3], 0.5); }));
    text.closePicker();
}

TEST_CASE("colours: the palette under every picker: the colour dragged onto an empty swatch adds it, a click picks it, "
          "right-click updates, removes and clears; the Palette tab renames and recolours; each is one undo step")
{
    ColourWindow w;
    azl::ui::Session& session = w.session;
    azl::ui::Swatch& swatch = w.chain().card(w.threshold)->swatchRow("color2Red")->swatch();
    swatch.openPicker();
    REQUIRE(swatch.picker() != nullptr);
    azl::ui::ColourPicker& picker = *swatch.picker();
    CHECK(picker.paletteCells() == 12);
    auto palette = [&] {
        const azl::Composition document = w.show.document();
        return document.palettes.empty() ? azl::ColourPalette{} : document.palettes[0];
    };
    auto setHex = [&](const juce::String& hex) {
        picker.hexField().setText(hex);
        picker.hexField().onReturnKey();
    };

    setHex("#336699");
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Blue"), 0x99 / 255.0); }));
    size_t before = undoSteps(session);
    drag(w.show.harness, picker, picker.previewArea().getCentre(), picker.paletteArea(0).getCentre());
    REQUIRE(w.until([&] { return palette().colours.size() == 1; }));
    CHECK(palette().name == "Palette 1");
    CHECK(palette().colours[0].name == "Colour 1");
    CHECK(azl::toHex(azl::rgbaOf(palette().colours[0].colour), false) == "#336699");
    REQUIRE(w.until([&] { return undoSteps(session) == before + 1; }));

    // Onto any empty swatch it is added at the end; onto a filled one it adds nothing.
    setHex("#ff0000");
    drag(w.show.harness, picker, picker.previewArea().getCentre(), picker.paletteArea(7).getCentre());
    REQUIRE(w.until([&] { return palette().colours.size() == 2; }));
    CHECK(azl::toHex(azl::rgbaOf(palette().colours[1].colour), false) == "#ff0000");
    drag(w.show.harness, picker, picker.previewArea().getCentre(), picker.paletteArea(0).getCentre());
    runUntil(w.show.harness, [] { return false; }, 0.3);
    CHECK(palette().colours.size() == 2);

    // One click picks a palette colour.
    before = undoSteps(session);
    click(picker, picker.paletteArea(0).getCentre());
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color2Red"), 0x33 / 255.0); }));
    REQUIRE(w.until([&] { return undoSteps(session) == before + 1; }));

    // Right-click: update with the current colour, remove, clear all; each undoes.
    azl::ui::menuEntry(picker.paletteMenu(1), "Update with current colour")->run();
    REQUIRE(w.until([&] { return azl::toHex(azl::rgbaOf(palette().colours[1].colour), false) == "#336699"; }));
    CHECK_FALSE(azl::ui::menuEntry(picker.paletteMenu(5), "Remove")->enabled);
    azl::ui::menuEntry(picker.paletteMenu(0), "Remove")->run();
    REQUIRE(w.until([&] { return palette().colours.size() == 1; }));
    session.client().undo();
    REQUIRE(w.until([&] { return palette().colours.size() == 2; }));
    azl::ui::menuEntry(picker.paletteMenu(0), "Clear all")->run();
    REQUIRE(w.until([&] { return palette().colours.empty(); }));
    session.client().undo();
    REQUIRE(w.until([&] { return palette().colours.size() == 2; }));
    runUntil(w.show.harness, [] { return false; }, 0.3);
    shoot(w.window, "75-palette-picker.png");
    swatch.closePicker();

    // The Palette tab: names, a new colour from the recent ones, removal, and a swatch whose picker edits the colour.
    w.window.mid().showTab(4);
    azl::ui::PaletteView& view = w.window.mid().palette();
    REQUIRE(w.until([&] { return view.swatch(1) != nullptr; }));
    view.rename(0, "flyer green");
    REQUIRE(w.until([&] { return session.composition().palettes[0].colours[0].name == "flyer green"; }));
    view.renamePalette("Flyer");
    REQUIRE(w.until([&] { return palette().name == "Flyer" && palette().colours[0].name == "flyer green"; }));
    view.addButton().onClick();
    REQUIRE(w.until([&] { return palette().colours.size() == 3; }));
    CHECK(palette().colours[2].colour == azl::arrayOf(session.recentColours().front()));
    click(view, view.removeArea(2).getCentre());
    REQUIRE(w.until([&] { return palette().colours.size() == 2; }));
    REQUIRE(w.until([&] { return view.swatch(2) == nullptr; }));
    view.swatch(1)->openPicker();
    REQUIRE(view.swatch(1)->picker() != nullptr);
    view.swatch(1)->picker()->hexField().setText("#00ff00");
    view.swatch(1)->picker()->hexField().onReturnKey();
    REQUIRE(w.until([&] { return azl::toHex(azl::rgbaOf(palette().colours[1].colour), false) == "#00ff00"; }));
    REQUIRE(w.until([&] { return view.swatch(1)->shown() && near(view.swatch(1)->shown()->g, 1.0); }));
    view.swatch(1)->closePicker();
    runUntil(w.show.harness, [] { return false; }, 0.3);
    shoot(w.window, "75-palette-tab.png");

    // A show loaded with a palette shows it; one without shows none.
    azl::Composition loaded = w.show.document();
    loaded.palettes = {{7, "Loaded", {{"bone", {0.9, 0.88, 0.8, 1.0}}}}};
    session.client().edit(azl::LoadComposition{loaded});
    REQUIRE(w.until([&] { return view.swatch(0) && !view.swatch(1) && palette().name == "Loaded"; }));
    loaded.palettes.clear();
    session.client().edit(azl::LoadComposition{loaded});
    REQUIRE(w.until([&] { return view.swatch(0) == nullptr; }));
}

TEST_CASE("presets: the effect header's preset menu saves, saves as, renames, deletes, goes back to default and sets "
          "the default new effects start from; the header names the preset and marks it once edited")
{
    PresetShow p;
    Show& show = p.show;
    azl::EngineHarness& harness = show.harness;
    azl::ui::Session session("127.0.0.1", p.server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(harness, [&] { return !names(session.client().presetEntries(), "classicStripper", "mine").empty(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(p.stripper) != nullptr; }));
    azl::ui::EffectCard& card = *chain.card(p.stripper);
    auto effect = [&] {
        const azl::Composition document = show.document();
        const azl::Effect* e = azl::effectById(document, p.stripper);
        return e ? *e : azl::Effect{};
    };
    auto mine = [&] { return names(session.client().presetEntries(), "classicStripper", "mine"); };
    auto contains = [](const std::vector<std::string>& list, const std::string& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    CHECK(card.presetLabel().isEmpty());
    CHECK_FALSE(azl::ui::menuEntry(card.presetMenu(), "Save")->enabled);

    // Save as names the preset; an edit marks it; Save clears the mark.
    card.saveAs("Bars A");
    REQUIRE(runUntil(harness, [&] { return contains(mine(), "Bars A") && effect().preset == "Bars A"; }));
    REQUIRE(runUntil(harness, [&] { return card.presetLabel() == "Bars A"; }));
    session.setParam({0, p.stripper, "number"}, 0.9);
    REQUIRE(runUntil(harness, [&] { return card.presetLabel() == "Bars A*"; }));
    REQUIRE(azl::ui::menuEntry(card.presetMenu(), "Save")->enabled);
    azl::ui::menuEntry(card.presetMenu(), "Save")->run();
    REQUIRE(runUntil(harness, [&] { return card.presetLabel() == "Bars A"; }));
    runUntil(harness, [] { return false; }, 0.2);
    shoot(window, "75-preset-header.png");

    // Rename follows on the effect.
    card.renamePreset("Bars B");
    REQUIRE(runUntil(harness, [&] { return contains(mine(), "Bars B") && !contains(mine(), "Bars A"); }));
    REQUIRE(runUntil(harness, [&] { return effect().preset == "Bars B" && card.presetLabel() == "Bars B"; }));

    // Set as default: a new effect of the type starts from it, named after it.
    azl::ui::menuEntry(card.presetMenu(), "Set as default")->run();
    REQUIRE(runUntil(harness, [&] { return session.client().presetDefaults().count("classicStripper") == 1; }));
    CHECK(session.client().presetDefaults().at("classicStripper") == "Bars B");
    CHECK(azl::ui::menuEntry(card.presetMenu(), "Clear default")->ticked);
    chain.drop("effect:classicStripper", std::nullopt);
    REQUIRE(runUntil(harness, [&] { return show.layer().effects.size() == 3; }));
    const azl::Effect added = show.layer().effects.back();
    CHECK(added.preset == "Bars B");
    CHECK(added.params.at("number") == doctest::Approx(0.9));

    // Back to default returns to what a new effect starts from.
    session.setParam({0, p.stripper, "number"}, 0.2);
    REQUIRE(runUntil(harness, [&] { return card.presetLabel() == "Bars B*"; }));
    azl::ui::menuEntry(card.presetMenu(), "Back to default")->run();
    REQUIRE(runUntil(harness, [&] { return effect().params.at("number") == doctest::Approx(0.9); }));
    REQUIRE(runUntil(harness, [&] { return card.presetLabel() == "Bars B"; }));
    azl::ui::menuEntry(card.presetMenu(), "Clear default")->run();
    REQUIRE(runUntil(harness, [&] { return session.client().presetDefaults().empty(); }));

    // A built-in preset is read-only.
    session.client().applyPreset(p.stripper, "Two-Way Bars", "builtin");
    REQUIRE(runUntil(harness, [&] { return effect().preset == "Two-Way Bars"; }));
    const azl::ui::Menu builtin = card.presetMenu();
    CHECK_FALSE(azl::ui::menuEntry(builtin, "Save")->enabled);
    CHECK_FALSE(azl::ui::menuEntry(builtin, "Rename...")->enabled);
    CHECK_FALSE(azl::ui::menuEntry(builtin, "Delete")->enabled);
    CHECK(azl::ui::menuEntry(builtin, "Set as default")->enabled);

    // Delete removes your own preset and the effect keeps its settings.
    card.saveAs("Temp");
    REQUIRE(runUntil(harness, [&] { return effect().preset == "Temp" && contains(mine(), "Temp"); }));
    azl::ui::menuEntry(card.presetMenu(), "Delete")->run();
    REQUIRE(runUntil(harness, [&] { return !contains(mine(), "Temp") && effect().preset.empty(); }));
}

TEST_CASE("presets: a chain preset saved from the owner card lists in the browser and drops onto the composition and a "
          "clip; the presets export to a file and import back")
{
    PresetShow p;
    Show& show = p.show;
    azl::EngineHarness& harness = show.harness;
    azl::ui::Session session("127.0.0.1", p.server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(p.stripper) != nullptr; }));
    CHECK(azl::ui::menuEntry(chain.ownerCard().chainMenu(), "Save chain as preset...")->enabled);
    CHECK_FALSE(azl::ui::menuEntry(chain.ownerCard().chainMenu(), "Add chain preset")->enabled);
    chain.ownerCard().saveChain("Look");
    REQUIRE(runUntil(harness, [&] { return session.client().chainPresets().contains("Look"); }));
    CHECK(azl::ui::chainEffects(session.client().chainPresets(), "Look").size() == 2);

    window.browser().selectTab(1);
    azl::ui::PresetTree& tree = window.browser().effects();
    const auto row = rowOf(tree, Row::Kind::Chain, "Look");
    REQUIRE(row);
    CHECK(row->second.count == 2);
    CHECK(azl::ui::menuEntry(tree.chainMenu(row->second), "Export presets...") != nullptr);
    const juce::String payload =
        azl::ui::chainDrag("Look", session.client().chainPresets().value("Look", azl::Message::array()));
    CHECK(azl::ui::isEffectDrag(payload));

    // Onto the Composition header: both effects in one undo step.
    size_t before = undoSteps(session);
    auto* header = dynamic_cast<juce::DragAndDropTarget*>(&window.grid().columns());
    REQUIRE(header->isInterestedInDragSource(DragSource(payload, &window.browser(), {30, 8})));
    header->itemDragMove(DragSource(payload, &window.browser(), {30, 8}));
    header->itemDropped(DragSource(payload, &window.browser(), {30, 8}));
    REQUIRE(runUntil(harness, [&] { return show.document().effects.size() == 2; }));
    CHECK(show.document().effects[0].type == "transform");
    CHECK(show.document().effects[1].type == "classicStripper");
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));

    // Onto a clip's chain.
    session.selectClip({show.base, show.columns[0].id});
    REQUIRE(runUntil(harness, [&] { return session.chainOwner().column.has_value(); }));
    chain.drop(payload, std::nullopt);
    REQUIRE(runUntil(harness, [&] { return show.layer().clips.at(show.columns[0].id).effects.size() == 2; }));

    // Export, delete, import back.
    TempDir dir;
    const juce::File file(juce::String((dir.path / "presets.json").string()));
    REQUIRE(azl::ui::exportPresets(session, file));
    const azl::Message exported = azl::Message::parse(file.loadFileAsString().toStdString());
    CHECK(exported["chains"].contains("Look"));
    CHECK(exported["presets"]["classicStripper"].contains("My Bars"));
    CHECK_FALSE(exported["presets"].contains("transform"));
    session.client().deleteChainPreset("Look");
    session.client().deletePreset("classicStripper", "My Bars");
    REQUIRE(runUntil(harness, [&] {
        return !session.client().chainPresets().contains("Look") &&
               names(session.client().presetEntries(), "classicStripper", "mine").empty();
    }));
    REQUIRE(azl::ui::importPresets(session, file));
    REQUIRE(runUntil(harness, [&] {
        return session.client().chainPresets().contains("Look") &&
               names(session.client().presetEntries(), "classicStripper", "mine") == std::vector<std::string>{"My Bars"};
    }));
    const juce::File junk(juce::String((dir.path / "junk.json").string()));
    junk.replaceWithText("[1, 2]");
    CHECK_FALSE(azl::ui::importPresets(session, junk));
}

TEST_CASE("colours: colours linked to a palette colour follow it in engine and UI; a linked colour's picker edits the "
          "palette colour in one undo step; a click relinks; unlink keeps the colour")
{
    ColourWindow w;
    azl::ui::Session& session = w.session;
    session.client().edit(azl::SetPalette{{0, "Flyer", {{"green", {0.2, 0.9, 0.3, 1.0}}, {"purple", {0.5, 0.1, 0.7, 1.0}}}}});
    REQUIRE(w.until([&] { return session.composition().palettes.size() == 1; }));
    const uint64_t green = session.composition().palettes[0].colours[0].id;
    const uint64_t purple = session.composition().palettes[0].colours[1].id;
    azl::ui::EffectCard& card = *w.chain().card(w.threshold);
    azl::ui::Swatch& one = card.swatchRow("color1Red")->swatch();
    azl::ui::Swatch& two = card.swatchRow("color2Red")->swatch();
    const azl::ColourRef first{w.threshold, "color1Red"}, second{w.threshold, "color2Red"};
    auto linkIs = [&](const azl::ColourRef& ref, std::optional<uint64_t> id) {
        return azl::linkOf(w.show.document(), ref) == id && azl::linkOf(session.composition(), ref) == id;
    };

    // Linking from the picker's palette menu gives the colour the palette colour.
    one.openPicker();
    REQUIRE(one.picker() != nullptr);
    CHECK_FALSE(azl::ui::menuEntry(one.picker()->paletteMenu(0), "Unlink")->enabled);
    azl::ui::menuEntry(one.picker()->paletteMenu(0), "Link to this colour")->run();
    REQUIRE(w.until([&] { return linkIs(first, green) && near(w.param(w.threshold, "color1Green"), 0.9); }));
    one.closePicker();
    two.openPicker();
    REQUIRE(two.picker() != nullptr);
    azl::ui::ColourPicker& picker = *two.picker();
    azl::ui::menuEntry(picker.paletteMenu(0), "Link to this colour")->run();
    REQUIRE(w.until([&] { return linkIs(second, green) && picker.target().group == azl::kPaletteSlot; }));
    REQUIRE(w.until([&] { return picker.unlinkButton().isVisible(); }));

    // The linked colour's picker edits the palette colour: the engine recolours both, one undo step.
    const size_t before = undoSteps(session);
    picker.hexField().setText("#ff0000");
    picker.hexField().onReturnKey();
    REQUIRE(w.until([&] {
        return near(w.param(w.threshold, "color1Red"), 1.0) && near(w.param(w.threshold, "color2Red"), 1.0) &&
               near(w.param(w.threshold, "color1Green"), 0.0);
    }));
    CHECK(azl::toHex(azl::rgbaOf(w.show.document().palettes[0].colours[0].colour), false) == "#ff0000");
    REQUIRE(w.until([&] { return undoSteps(session) == before + 1; }));
    REQUIRE(w.until([&] { return one.shown() && near(one.shown()->r, 1.0) && near(one.shown()->g, 0.0); }));
    session.client().undo();
    REQUIRE(w.until([&] {
        return near(w.param(w.threshold, "color1Green"), 0.9) && near(w.param(w.threshold, "color2Green"), 0.9);
    }));
    REQUIRE(w.until([&] { return one.shown() && near(one.shown()->g, 0.9); }));

    // The Palette tab's swatch recolours every linked colour too.
    w.window.mid().showTab(4);
    azl::ui::PaletteView& view = w.window.mid().palette();
    REQUIRE(w.until([&] { return view.swatch(0) != nullptr; }));
    view.swatch(0)->openPicker();
    REQUIRE(view.swatch(0)->picker() != nullptr);
    view.swatch(0)->picker()->hexField().setText("#0000ff");
    view.swatch(0)->picker()->hexField().onReturnKey();
    REQUIRE(w.until([&] {
        return near(w.param(w.threshold, "color1Blue"), 1.0) && near(w.param(w.threshold, "color2Blue"), 1.0);
    }));
    view.swatch(0)->closePicker();

    // A click on another palette colour relinks a linked colour.
    click(picker, picker.paletteArea(1).getCentre());
    REQUIRE(w.until([&] { return linkIs(second, purple) && near(w.param(w.threshold, "color2Red"), 0.5); }));
    CHECK(near(w.param(w.threshold, "color1Blue"), 1.0));
    runUntil(w.show.harness, [] { return false; }, 0.3);
    shoot(w.window, "75-palette-link.png");

    // Unlinked, the colour stays and no longer follows.
    picker.unlinkButton().onClick();
    REQUIRE(w.until([&] { return linkIs(second, std::nullopt); }));
    CHECK(near(w.param(w.threshold, "color2Red"), 0.5));
    azl::ColourPalette white = session.composition().palettes[0];
    white.colours[1].colour = {1.0, 1.0, 1.0, 1.0};
    session.client().edit(azl::SetPalette{white});
    REQUIRE(w.until([&] { return w.show.document().palettes[0].colours[1].colour[1] == 1.0; }));
    CHECK(near(w.param(w.threshold, "color2Red"), 0.5));
    two.closePicker();
}

TEST_CASE("panels: every option parameter is a dropdown of its options in order (a checkbox for Off and On), a choice "
          "stores the option's value in one undo step, a macro steps it, and a shortened label names the parameter in "
          "full in its tooltip")
{
    Show show;
    for (const std::string& type : azl::effectTypes()) {
        const azl::EffectDef* def = azl::findEffect(type);
        if (std::any_of(def->params.begin(), def->params.end(), [](const azl::ParamDef& p) { return !p.options.empty(); }))
            apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, type, {}, true}});
    }
    std::vector<uint64_t> effects;
    for (const azl::Effect& e : show.layer().effects)
        if (e.id != show.fx)
            effects.push_back(e.id);
    REQUIRE(effects.size() >= 18);
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(effects.back()) != nullptr; }));
    auto effectOf = [&](uint64_t id) {
        for (const azl::Effect& e : show.layer().effects)
            if (e.id == id)
                return e;
        FAIL("no effect " << id);
        return azl::Effect{};
    };

    // Each row of each card: a checkbox when the parameter's options are Off and On, else a dropdown with the option
    // names in order exactly when it has options, set to the option its value picks; a label that is shortened or does
    // not fit names the parameter in full in its tip.
    size_t dropdowns = 0;
    size_t switches = 0;
    std::vector<std::string> cut;
    for (const uint64_t id : effects) {
        const azl::Effect effect = effectOf(id);
        const azl::EffectDef* def = azl::findEffect(effect.type);
        for (const azl::ParamDef& p : def->params) {
            CAPTURE(effect.type);
            CAPTURE(p.name);
            azl::ui::ParamRow* row = chain.card(id)->row(p.name);
            REQUIRE(row);
            const bool offOn = p.options == std::vector<std::string>{"Off", "On"};
            juce::ComboBox* box = row->optionBox();
            CHECK((box != nullptr) == (!p.options.empty() && !offOn));
            CHECK((row->switchBox() != nullptr) == offOn);
            if (offOn) {
                ++switches;
                CHECK(row->switchBox()->getToggleState() == (azl::optionIndex(p, p.defaultValue) == 1));
                CHECK(row->switchBox()->getTooltip().isNotEmpty());
            }
            if (box) {
                ++dropdowns;
                REQUIRE(box->getNumItems() == int(p.options.size()));
                for (size_t i = 0; i < p.options.size(); ++i)
                    CHECK(box->getItemText(int(i)).toStdString() == p.options[i]);
                CHECK(box->getSelectedItemIndex() == azl::optionIndex(p, p.defaultValue));
                CHECK(box->getTooltip().isNotEmpty());
            }
            const auto info = azl::ui::paramInfo(session.composition(), row->ref());
            REQUIRE(info);
            const bool fits = juce::GlyphArrangement::getStringWidthInt(azl::ui::Look::font(), info->label) <=
                              row->labelArea().getWidth();
            if (!fits)
                cut.push_back(effect.type + "/" + info->label);
            if (!fits || !p.fullLabel.empty()) {
                const std::string full = p.fullLabel.empty() ? info->label : p.fullLabel;
                CHECK(row->tipAt(row->labelArea().getCentre()).startsWith(juce::String(full) + ":"));
            }
        }
    }
    CHECK(dropdowns + switches >= 40);
    CHECK(switches > 0);
    MESSAGE("labels cut off (named in full in their tooltip): " << cut.size());
    for (const std::string& label : cut)
        MESSAGE("  " << label);

    // Classic Stripper: MovementType reads as Movement; picking an option stores what a preset saved with it holds.
    uint64_t stripper = 0;
    for (const uint64_t id : effects)
        if (effectOf(id).type == "classicStripper")
            stripper = id;
    REQUIRE(stripper);
    apply(show.client, azl::SetEffectBypass{stripper, false});
    azl::ui::ParamRow* movement = chain.card(stripper)->row("movementType");
    CHECK(azl::ui::paramInfo(session.composition(), movement->ref())->label == "Movement");
    CHECK(movement->tipAt(movement->labelArea().getCentre()).startsWith("MovementType:"));
    REQUIRE(runUntil(harness, [&] { return !chain.card(stripper)->bypassButton().getToggleState(); }));
    const size_t steps = undoSteps(session);
    movement->optionBox()->setSelectedItemIndex(1, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return effectOf(stripper).params.count("movementType") == 1; }));
    CHECK(effectOf(stripper).params.at("movementType") == 0.5);
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    azl::ui::ParamRow* mode = chain.card(stripper)->row("mode");
    mode->optionBox()->setSelectedItemIndex(5, juce::sendNotificationSync);
    REQUIRE(runUntil(harness, [&] { return effectOf(stripper).params.count("mode") == 1; }));
    CHECK(effectOf(stripper).params.at("mode") == doctest::Approx(5.0 / 6.0).epsilon(1e-12));
    CHECK(mode->optionBox()->getText() == "Negative");

    // A whole-number choice stores its index.
    for (const uint64_t id : effects)
        if (effectOf(id).type == "classicShiftRgb") {
            chain.card(id)->row("mode")->optionBox()->setSelectedItemIndex(4, juce::sendNotificationSync);
            REQUIRE(runUntil(harness, [&] { return effectOf(id).params.count("mode") == 1; }));
            CHECK(effectOf(id).params.at("mode") == 4.0);
        }

    // A macro on Mode drives it only to option values, and the dropdown shows the driven option.
    const ParamRef modeRef{0, stripper, "mode"};
    session.assignToMacro(0, modeRef);
    REQUIRE(runUntil(harness, [&] { return show.document().macros.size() == 1; }));
    const uint64_t macro = show.document().macros[0].id;
    const azl::ParamDef& modeDef = *azl::paramDef(show.document(), modeRef);
    std::set<int> seen;
    for (const double v : {0.0, 0.13, 0.31, 0.5, 0.77, 1.0}) {
        session.client().edit(azl::SetMacroValue{macro, v});
        CAPTURE(v);
        REQUIRE(runUntil(harness, [&] {
            const auto driven = session.drivenValue(modeRef);
            return driven && show.document().macros[0].value == v;
        }));
        runUntil(harness, [] { return false; }, 0.2);
        const double driven = *session.drivenValue(modeRef);
        const int option = azl::optionIndex(modeDef, driven);
        CHECK(driven == doctest::Approx(azl::optionValue(modeDef, option)).epsilon(1e-6));
        CHECK(mode->optionBox()->getSelectedItemIndex() == option);
        seen.insert(option);
    }
    CHECK(seen.size() >= 3);
    CHECK(effectOf(stripper).params.at("mode") == doctest::Approx(5.0 / 6.0).epsilon(1e-12));
    shoot(*chain.card(stripper), "101-classic-stripper.png");
}

TEST_CASE("steps: an effect card's Autopilot badge cycles and sets its timing; the step strip starts closed, adds, "
          "reorders and removes steps and moves the effect to one")
{
    Show show;
    apply(show.client, azl::AddEffect{show.base, std::nullopt, {0, "classicStripper", {{"globalWidth", 0.5}}}});
    const uint64_t stripper = show.layer().effects.back().id;
    azl::WebSocketServer server{show.harness.document(), 0};
    azl::ui::Session session{"127.0.0.1", server.port()};
    azl::ui::MainComponent window{session};
    window.setSize(1600, 950);
    auto until = [&](const std::function<bool()>& done) { return runUntil(show.harness, done); };
    auto effect = [&] { return *azl::effectById(show.document(), stripper); };
    REQUIRE(until([&] { return session.client().synced() && azl::effectById(session.composition(), stripper); }));
    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(until([&] { return chain.card(stripper) != nullptr; }));
    azl::ui::EffectCard& card = *chain.card(stripper);
    azl::ui::StepStrip& strip = card.stepStrip();

    // Closed at first: one thin row and no badge while there are no steps.
    CHECK(strip.getHeight() == azl::ui::StepStrip::kRow);
    CHECK_FALSE(card.pilotBadge().isVisible());
    click(strip, {30, azl::ui::StepStrip::kRow / 2});
    REQUIRE(until([&] { return session.stepsOpen(stripper) && strip.getHeight() > azl::ui::StepStrip::kRow; }));
    CHECK(strip.chipAt(strip.getLocalBounds().getTopLeft() + juce::Point<int>(4, azl::ui::StepStrip::kRow + 4)) ==
          azl::ui::StepStrip::kAdd);

    // Add from current values, twice with an edit between; each is one undo step.
    const size_t before = undoSteps(session);
    strip.addMenu()[0].run();
    REQUIRE(until([&] { return effect().steps.size() == 1; }));
    apply(show.client, azl::SetEffectParam{stripper, "globalWidth", 0.2});
    REQUIRE(until([&] { return azl::effectById(session.composition(), stripper)->params.at("globalWidth") == 0.2; }));
    strip.addMenu()[0].run();
    REQUIRE(until([&] { return effect().steps.size() == 2; }));
    CHECK(effect().steps[0].params.at("globalWidth") == 0.5);
    CHECK(effect().steps[1].params.at("globalWidth") == 0.2);
    CHECK(undoSteps(session) == before + 3);
    REQUIRE(until([&] { return card.pilotBadge().isVisible(); }));

    // Reorder and remove from a chip's menu.
    strip.chipMenu(0)[3].run();
    REQUIRE(until([&] { return effect().steps[0].params.at("globalWidth") == 0.2; }));
    strip.chipMenu(0)[3].run();
    REQUIRE(until([&] { return effect().steps[0].params.at("globalWidth") == 0.5; }));

    // The badge: a click cycles Off, Next, Random; its menu sets Every, Hold and the transition.
    CHECK(card.pilotBadge().text() == "AP");
    click(card.pilotBadge(), {4, 4});
    REQUIRE(until([&] { return effect().autopilot.mode == azl::AutopilotMode::Next; }));
    azl::ui::Menu menu = card.pilotBadge().menu();
    menu[0].sub[0].run();
    REQUIRE(until([&] { return effect().autopilot.bars == 1; }));
    menu[2].sub[5].run();
    REQUIRE(until([&] { return effect().transition == 4.0; }));
    REQUIRE(until([&] { return card.pilotBadge().text() == "N1"; }));

    // The Autopilot moves through the steps on the bar; the clock shows where the effect is.
    REQUIRE(until([&] {
        const auto clock = session.client().clock();
        auto it = clock.effects.find(stripper);
        return it != clock.effects.end() && it->second.step;
    }));
    click(card.pilotBadge(), {4, 4});
    REQUIRE(until([&] { return card.pilotBadge().text() == "R1"; }));
    click(card.pilotBadge(), {4, 4});
    REQUIRE(until([&] { return effect().autopilot.mode == azl::AutopilotMode::Off; }));

    // A click on a chip moves the effect there; the slider shows the value it glides to.
    const auto chip = strip.getLocalBounds().getTopLeft() + juce::Point<int>(4 + 22, azl::ui::StepStrip::kRow + 4);
    REQUIRE(strip.chipAt(chip) == size_t(1));
    click(strip, chip);
    REQUIRE(until([&] { return effect().playing && effect().playing->step == 1; }));
    REQUIRE(until([&] {
        const auto driven = session.drivenValue({0, stripper, "globalWidth"});
        return driven && std::abs(*driven - 0.2) < 1e-4;
    }));
    REQUIRE(until([&] { return session.client().clock().effects[stripper].step == size_t(1); }));
    runUntil(show.harness, [] { return false; }, 0.3);
    shoot(card, "76-effect-steps.png");

    strip.chipMenu(1)[5].run();
    REQUIRE(until([&] { return effect().steps.size() == 1; }));
}

TEST_CASE("colours: the Palette tab selects a palette live and adds one as a copy; its badge runs the palette "
          "Autopilot; a palette becomes a deck's, a macro takes the palette position and a colour's hue; the show's "
          "live colours ride along")
{
    ColourWindow w;
    azl::ui::Session& session = w.session;
    apply(w.show.client, azl::SetPalette{{0, "Day", {{"sun", {1.0, 0.8, 0.2, 1.0}}, {"sky", {0.3, 0.6, 1.0, 1.0}}}}});
    w.window.mid().showTab(4);
    azl::ui::PaletteView& view = w.window.mid().palette();
    REQUIRE(w.until([&] { return view.swatch(1) != nullptr && view.pilotBadge().isVisible(); }));
    auto palettes = [&] { return w.show.document().palettes; };

    view.addPalette();
    REQUIRE(w.until([&] { return palettes().size() == 2; }));
    azl::ColourPalette night = palettes()[1];
    CHECK(night.name == "Palette 2");
    CHECK(night.colours.size() == 2);
    CHECK(night.colours[0].id != palettes()[0].colours[0].id);
    night.name = "Night";
    night.colours[0].colour = {0.1, 0.1, 0.5, 1.0};
    session.client().edit(azl::SetPalette{night});
    session.client().edit(azl::SetPaletteLink{{w.threshold, "color1Red"}, palettes()[0].colours[0].id});
    REQUIRE(w.until([&] { return near(w.param(w.threshold, "color1Green"), 0.8) && palettes()[1].name == "Night"; }));

    // A click on a chip selects the palette, live: the document recolours, and the clock carries the live colours.
    click(view, view.chipArea(1).getCentre());
    REQUIRE(w.until([&] { return w.show.document().palette == night.id && near(w.param(w.threshold, "color1Blue"), 0.5); }));
    REQUIRE(w.until([&] {
        const azl::ui::ClockState clock = session.client().clock();
        return clock.palette && clock.palette->step == 1 && clock.paletteColours.size() == 2 &&
               near(clock.paletteColours[0][2], 0.5, 0.01);
    }));
    CHECK(view.titleArea() == view.chipArea(1));

    // The badge cycles the palette Autopilot; its menu sets how often and the crossfade.
    click(view.pilotBadge(), view.pilotBadge().getLocalBounds().getCentre());
    REQUIRE(w.until([&] { return w.show.document().palettePilot.mode == azl::AutopilotMode::Next; }));
    CHECK(view.pilotBadge().text() == "N4");
    const azl::ui::Menu pilot = view.pilotBadge().menu();
    azl::ui::menuEntry(pilot, "Every/2 bars")->run();
    REQUIRE(w.until([&] { return view.pilotBadge().text() == "N2"; }));
    const azl::ui::Menu transition = view.pilotBadge().menu();
    azl::ui::menuEntry(transition, "Transition/4 beats")->run();
    REQUIRE(w.until([&] { return w.show.document().paletteTransition == 4.0; }));

    // A deck's palette, and a macro on the palette position and on a colour's hue.
    const azl::ui::Menu chip = view.chipMenu(0);
    azl::ui::menuEntry(chip, "Deck palette/" + azl::ui::utf8(w.show.document().decks[0].name))->run();
    REQUIRE(w.until([&] { return w.show.document().decks[0].palette == palettes()[0].id; }));
    apply(w.show.client, azl::AddMacro{std::nullopt, {.name = "Mood"}});
    REQUIRE(w.until([&] { return session.composition().macros.size() == 1; }));
    const azl::ui::Menu position = view.chipMenu(0);
    azl::ui::menuEntry(position, "Position to macro/Mood")->run();
    REQUIRE(w.until([&] { return w.show.document().macros[0].targets.size() == 1; }));
    const azl::ui::Menu hue = view.colourMenu(1);
    azl::ui::menuEntry(hue, "Hue to macro/Mood")->run();
    REQUIRE(w.until([&] { return w.show.document().macros[0].targets.size() == 2; }));
    CHECK(w.show.document().macros[0].targets[0].param == ParamRef{0, 0, "palettePosition"});
    CHECK(w.show.document().macros[0].targets[1].param == ParamRef{0, 0, "paletteHue2"});
    CHECK(azl::ui::menuEntry(chip, "Remove palette")->enabled);

    session.client().edit(azl::SetPaletteAutopilot{azl::AutopilotMode::Off});
    REQUIRE(w.until([&] { return view.pilotBadge().text() == "AP"; }));
    runUntil(w.show.harness, [] { return false; }, 0.5);
    shoot(view, "75-palette-steps.png");
    shoot(w.window, "75-palette-steps-window.png");
}

TEST_CASE("panels: the layer card lists user ISF transitions and folds the transition's settings")
{
    Show show;
    std::ofstream(show.harness.userIsf().folder() / "Test Split.fs") << R"(/*{
    "INPUTS": [
        {"NAME": "startImage", "TYPE": "image"},
        {"NAME": "endImage", "TYPE": "image"},
        {"NAME": "progress", "TYPE": "float"},
        {"NAME": "scale", "TYPE": "float", "MIN": 0.0, "MAX": 2.0, "DEFAULT": 1.0}
    ]
}*/
vec4 getFromColor(vec2 uv) { return IMG_NORM_PIXEL(startImage, uv); }
vec4 getToColor(vec2 uv) { return IMG_NORM_PIXEL(endImage, uv); }
vec4 transition(vec2 uv) { return uv.x < progress * scale ? getToColor(uv) : getFromColor(uv); }
void main() { gl_FragColor = transition(isf_FragNormCoord.xy); }
)";
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.choice("Transition") != nullptr; }));
    CHECK(card.field("scale") == nullptr);

    session.client().rescanIsf();
    auto listed = [&] {
        juce::ComboBox* box = card.choice("Transition");
        return box && box->getItemText(box->getNumItems() - 1) == "Test Split (user)";
    };
    REQUIRE(runUntil(harness, listed));
    juce::ComboBox* box = card.choice("Transition");
    box->setSelectedItemIndex(box->getNumItems() - 1, juce::sendNotificationSync);
    const azl::TransitionMode split = azl::userTransitionMode("userTestSplit");
    REQUIRE(runUntil(harness, [&] { return show.layer().transition.mode == split && card.field("scale") != nullptr; }));
    CHECK_FALSE(card.hasSource());
    CHECK_FALSE(card.field("scale")->isVisible());
    card.moreToggle()->onClick();
    REQUIRE(runUntil(harness, [&] { return card.field("scale")->isVisible(); }));
    card.field("scale")->onChange(1.5);
    REQUIRE(runUntil(harness, [&] {
        const auto params = show.layer().transitionParams;
        return params.contains("userTestSplit") && params.at("userTestSplit").at("scale") == 1.5;
    }));
}

TEST_CASE("panels: the LFO editor offers every shape and edits a custom curve, one undo step per edit")
{
    Show show;
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.selectLayer(show.base);
    azl::ui::OwnerCard& card = window.chain().content().ownerCard();
    REQUIRE(runUntil(harness, [&] { return card.opacityRow() != nullptr; }));
    const ParamRef opacity{show.base, 0, "opacity"};
    session.animate(opacity, "beat");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 1; }));
    azl::ui::ParamRow* row = card.opacityRow();
    REQUIRE(runUntil(harness, [&] { return row->animateBox() != nullptr; }));
    azl::ui::AnimateBox& box = *row->animateBox();
    azl::ui::LfoEditor& editor = box.lfoEditor();
    REQUIRE(runUntil(harness, [&] { return row->getHeight() == row->preferredHeight() && editor.isVisible(); }));
    CHECK(editor.getHeight() == azl::ui::LfoEditor::kHeight);
    CHECK(editor.getBottom() <= box.getHeight());
    CHECK(card.choice("Blend")->getY() >= row->getBottom());

    juce::StringArray shapes;
    for (int i = 0; i < box.shapeBox().getNumItems(); ++i)
        shapes.add(box.shapeBox().getItemText(i));
    CHECK(shapes.joinIntoString(",") == "Sine,Saw,Square,Ramp,Random,Custom");

    // The plot is the engine's curve through the modulator's range, at 64 phases.
    auto plotsAsEngine = [&] {
        const azl::Composition document = session.composition();
        const azl::Modulator& m = document.modulators[0];
        for (int i = 0; i < 64; ++i) {
            const double x = i / 64.0;
            const double source = std::clamp(azl::lfoAt(m.shape, x, m.points), 0.0, 1.0);
            if (editor.plotAt(x) != doctest::Approx(azl::ui::modulatedAt(document, m, source)))
                return false;
        }
        return true;
    };
    CHECK(plotsAsEngine());
    box.shapeBox().setSelectedId(4);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].shape == azl::LfoShape::Ramp; }));
    // Custom starts from the shape it had.
    box.shapeBox().setSelectedId(6);
    REQUIRE(runUntil(harness, [&] { return session.composition().modulators[0].shape == azl::LfoShape::Custom; }));
    auto points = [&] { return show.document().modulators[0].points; };
    REQUIRE(points().size() == 2);
    CHECK(points()[0].value == 0.0);
    CHECK(points()[1].value == 1.0);
    CHECK(plotsAsEngine());

    // A drag moves the point between its neighbours: many edits, one undo step.
    size_t steps = undoSteps(session);
    const auto plot = editor.plot();
    const auto from = editor.pointPosition(0).toInt();
    drag(harness, editor, from, {juce::roundToInt(plot.getX() + plot.getWidth() * 0.5f), juce::roundToInt(plot.getY())});
    REQUIRE(runUntil(harness, [&] { return points()[0].phase == doctest::Approx(0.5).epsilon(0.02); }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    CHECK(points()[0].value > 0.5);
    REQUIRE(runUntil(harness, [&] { return session.composition().modulators[0].points == points(); }));
    CHECK(plotsAsEngine());
    shoot(window, "70-lfo-editor.png");

    // Double-click adds a point in phase order, or makes one smooth; right-click removes one.
    steps = undoSteps(session);
    const juce::Point<int> quarter{juce::roundToInt(plot.getX() + plot.getWidth() * 0.25f),
                                   juce::roundToInt(plot.getCentreY())};
    editor.mouseDoubleClick(mouse(editor, quarter, quarter));
    REQUIRE(runUntil(harness, [&] { return points().size() == 3 && session.composition().modulators[0].points.size() == 3; }));
    CHECK(points()[0].phase == doctest::Approx(0.25).epsilon(0.02));
    CHECK(undoSteps(session) == steps + 1);
    const auto first = editor.pointPosition(0).toInt();
    editor.mouseDoubleClick(mouse(editor, first, first));
    REQUIRE(runUntil(harness, [&] { return points()[0].smooth && session.composition().modulators[0].points[0].smooth; }));
    CHECK(undoSteps(session) == steps + 2);
    const auto last = editor.pointPosition(2).toInt();
    editor.mouseDown(mouse(editor, last, last, juce::ModifierKeys::rightButtonModifier));
    REQUIRE(runUntil(harness, [&] { return points().size() == 2 && session.composition().modulators[0].points.size() == 2; }));
    CHECK(undoSteps(session) == steps + 3);
    shoot(window, "70-lfo-editor-edited.png");
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return points().size() == 3; }));

    // An audio source has no LFO editor and the row shrinks back.
    box.sourceBox().setSelectedId(5);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].source == azl::ModulatorSource::Audio; }));
    REQUIRE(runUntil(harness, [&] {
        azl::ui::AnimateBox* now = row->animateBox();
        return now && !now->lfoEditor().isVisible() && row->getHeight() == row->preferredHeight() &&
               now->preferredHeight() == azl::ui::AnimateBox::kHeight;
    }));
}

TEST_CASE("panels: a macro's LFO modulator row shows its curve editor, an audio one does not")
{
    Show show;
    azl::Modulator kick;
    kick.source = azl::ModulatorSource::Audio;
    kick.target.macro = 1;
    azl::Modulator lfo;
    lfo.beats = 2.0;
    lfo.shape = azl::LfoShape::Custom;
    lfo.points = {{0.0, 0.2, true}, {0.4, 1.0}, {0.6, 0.6, true}};
    lfo.target.macro = 1;
    azl::Macro energy{.id = 1, .name = "Energy", .value = 0.2, .targets = {{{0, show.fx, "scale"}, {}, 0.0, 2.0}}};
    apply(show.client, azl::SetMacros{{energy}, {kick, lfo}});

    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    window.mid().showMacros();
    azl::ui::MacrosView& macros = window.mid().macros();
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRows() == 2; }));
    azl::ui::ModulatorRow& kickRow = *macros.modulatorRow(0);
    azl::ui::ModulatorRow& lfoRow = *macros.modulatorRow(1);
    REQUIRE(runUntil(harness, [&] { return lfoRow.getHeight() == lfoRow.preferredHeight(); }));
    CHECK_FALSE(kickRow.lfoEditor().isVisible());
    CHECK(kickRow.getHeight() == azl::ui::ModulatorRow::kHeight);
    CHECK(lfoRow.lfoEditor().isVisible());
    CHECK(lfoRow.shapeBox().getText() == "Custom");
    CHECK(lfoRow.getY() >= kickRow.getBottom());

    const size_t steps = undoSteps(session);
    azl::ui::LfoEditor& editor = lfoRow.lfoEditor();
    const auto plot = editor.plot();
    const auto from = editor.pointPosition(1).toInt();
    drag(harness, editor, from, {juce::roundToInt(plot.getX() + plot.getWidth() * 0.3f), from.y});
    REQUIRE(runUntil(harness, [&] {
        return show.document().modulators[1].points[1].phase == doctest::Approx(0.3).epsilon(0.02);
    }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == steps + 1; }));
    CHECK(show.document().modulators[1].points[1].value == doctest::Approx(1.0).epsilon(0.02));
    shoot(window, "70-lfo-macro-row.png");

    lfoRow.sourceBox().setSelectedId(3);
    REQUIRE(runUntil(harness, [&] {
        return !lfoRow.lfoEditor().isVisible() && lfoRow.getHeight() == azl::ui::ModulatorRow::kHeight;
    }));
}

TEST_CASE("panels: Low, Mid and High drive parameters and macros; the spectrum's edge handles move the band edges, "
          "one undo step per drag")
{
    Show show;
    azl::Modulator kick;
    kick.source = azl::ModulatorSource::Audio;
    kick.target.macro = 1;
    azl::Macro energy{.id = 1, .name = "Energy", .value = 0.2, .targets = {{{0, show.fx, "scale"}, {}, 0.0, 2.0}}};
    apply(show.client, azl::SetMacros{{energy}, {kick}});
    azl::WebSocketServer server(show.harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::EngineHarness& harness = show.harness;
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));

    // A macro's driver and a parameter's animation pick a band from their source menus.
    window.mid().showMacros();
    azl::ui::MacrosView& macros = window.mid().macros();
    REQUIRE(runUntil(harness, [&] { return macros.modulatorRows() == 1; }));
    azl::ui::ModulatorRow& row = *macros.modulatorRow(0);
    CHECK(row.sourceBox().getNumItems() == 8);
    CHECK(row.sourceBox().getItemText(6) == "Mid");
    row.sourceBox().setSelectedId(8);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[0].feature == "highBand"; }));
    REQUIRE(runUntil(harness, [&] { return row.sourceBox().getText() == "High"; }));

    session.selectLayer(show.base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(show.fx) != nullptr; }));
    const ParamRef position{0, show.fx, "positionX"};
    session.animate(position, "beat");
    REQUIRE(runUntil(harness, [&] { return show.document().modulators.size() == 2; }));
    azl::ui::ParamRow* param = chain.card(show.fx)->row("positionX");
    REQUIRE(runUntil(harness, [&] { return param->animateBox() != nullptr; }));
    param->animateBox()->sourceBox().setSelectedId(6);
    REQUIRE(runUntil(harness, [&] { return show.document().modulators[1].feature == "lowBand"; }));
    CHECK(show.document().modulators[1].source == azl::ModulatorSource::Audio);
    CHECK(show.document().modulators[1].envelope == azl::EnvelopePreset::Meter);

    // The audio panel lists each band with what it drives, and its sensitivity.
    azl::ui::AudioPanel& panel = window.audio();
    int listed = 0;
    for (const auto& entry : panel.entries())
        listed += (entry.param == position) + (entry.macroSlot == 0);
    CHECK(listed == 2);
    panel.sensitivity("midBand").onChange(200.0);
    REQUIRE(runUntil(harness, [&] {
        return show.document().sensitivity.count("midBand") && show.document().sensitivity.at("midBand") == 2.0;
    }));

    // Dragging the Mid/High handle to the right raises that edge, as one undo step; a drag never crosses the other edge.
    show.harness.document().broadcast({{"type", "audio"},
                                       {"op", "state"},
                                       {"live", true},
                                       {"values", {{"lowBand", 0.3}, {"midBand", 0.7}, {"highBand", 0.5}}},
                                       {"spectrum", Message(std::vector<double>(24, 0.4))}});
    REQUIRE(runUntil(harness, [&] { return session.client().audio().values.count("midBand"); }));
    CHECK(panel.tipAt(panel.edgeHandle(1)).contains("edge"));
    const size_t before = undoSteps(session);
    const juce::Point<int> high = panel.edgeHandle(1);
    drag(harness, panel, high, high + juce::Point<int>(40, 0));
    REQUIRE(runUntil(harness, [&] { return show.document().bandEdges[1] > 2500.0; }));
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 1; }));
    CHECK(show.document().bandEdges[0] == azl::kDefaultBandEdges[0]);
    REQUIRE(runUntil(harness, [&] { return std::abs(panel.edgeHandle(1).x - (high.x + 40)) <= 1; }));
    shoot(panel, "69-audio-bands.png");

    const juce::Point<int> low = panel.edgeHandle(0);
    drag(harness, panel, low, {panel.spectrumArea().getRight() - 2, low.y});
    REQUIRE(runUntil(harness, [&] { return undoSteps(session) == before + 2; }));
    const auto edges = show.document().bandEdges;
    CHECK(edges[0] > azl::kDefaultBandEdges[0]);
    CHECK(edges[1] >= edges[0] * azl::kMinMidBandRatio);
    session.client().undo();
    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return show.document().bandEdges == azl::kDefaultBandEdges; }));
}
