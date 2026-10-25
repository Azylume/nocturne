#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/bottom_panes.hpp"
#include "ui/app/main_component.hpp"
#include "ui/app/map_view.hpp"

#include <functional>

namespace {

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

// A mouse wheel notch down, or the smallest trackpad step down.
constexpr float kNotch = -0.1f;
constexpr float kTrackpadPixel = -0.5f / 256.0f;

void wheel(juce::Component& component, juce::Point<int> at, float deltaY, bool precise)
{
    juce::MouseWheelDetails details{};
    details.deltaY = deltaY;
    details.isSmooth = precise;
    const juce::Time now = juce::Time::getCurrentTime();
    component.mouseWheelMove(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), {}, 0.0f,
                                              0.0f, 0.0f, 0.0f, 0.0f, &component, &component, now, at.toFloat(), now, 1,
                                              false),
                             details);
}

// Scrolls a list by a notch, then by a trackpad pixel, checking the first row moves by each; then in trackpad steps
// until it stops, where `lastShown` holds and a notch moves nothing; then back to the top.
void checkScrolls(juce::Component& list, juce::Point<int> at, const std::function<int()>& firstY,
                  const std::function<bool()>& lastShown)
{
    const int top = firstY();
    REQUIRE_FALSE(lastShown());
    wheel(list, at, kNotch, false);
    CHECK(firstY() == top - 24);
    wheel(list, at, kTrackpadPixel, true);
    CHECK(firstY() == top - 25);
    for (int i = 0, before = top + 1; i < 400 && before != firstY(); ++i) {
        before = firstY();
        wheel(list, at, kTrackpadPixel * 8.0f, true);
    }
    CHECK(lastShown());
    const int end = firstY();
    CHECK(end < top - 25);
    wheel(list, at, kNotch, false);
    CHECK(firstY() == end);
    wheel(list, at, 100.0f, false);
    CHECK(firstY() == top);
}

struct Engine {
    TempDir media;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    Client client{harness.document()};
    azl::WebSocketServer server{harness.document(), 0};
};

} // namespace

TEST_CASE("scroll: the Files tab scrolls a long folder under the wheel and the trackpad")
{
    Engine e;
    const char* files[] = {"count_100.mp4", "count_240.mp4", "count_601_pc.mp4", "count_709_tv.mp4", "upright.mp4",
                           "rotated.mp4"};
    for (const char* file : files)
        REQUIRE(addMedia(e.client, copyMedia(file, e.media.path / "Long" / file))["entry"]["status"] == "ok");
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::BrowserPane browser(session);
    browser.setSize(110, 220);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && session.client().library().size() == 6; }));
    browser.selectTab(0);
    REQUIRE(browser.fileItems().size() == 7);
    const auto area = browser.listArea();
    checkScrolls(browser, area.getCentre(), [&] { return browser.fileItems().front().bounds.getY(); },
                 [&] { return area.contains(browser.fileItems().back().bounds); });
    // A row scrolled under the tools does not take clicks.
    wheel(browser, area.getCentre(), kTrackpadPixel * 10.0f, true);
    const auto header = browser.fileItems().front().bounds;
    REQUIRE(header.contains(header.getCentreX(), area.getY() - 1));
    CHECK(browser.tipAt({header.getCentreX(), area.getY() + 1}).isNotEmpty());
    CHECK(browser.tipAt({header.getCentreX(), area.getY() - 1}).isEmpty());
}

TEST_CASE("scroll: the Sources tab scrolls its generators under the wheel and the trackpad")
{
    Engine e;
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::BrowserPane browser(session);
    browser.setSize(260, 110);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced(); }));
    browser.selectTab(2);
    REQUIRE(browser.effectItems().size() == azl::generatorTypes().size() + 1);
    const auto area = browser.listArea();
    checkScrolls(browser, area.getCentre(), [&] { return browser.effectItems().front().first.getY(); },
                 [&] { return area.contains(browser.effectItems().back().first); });
}

TEST_CASE("scroll: the Effects tab follows the smallest trackpad step")
{
    Engine e;
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::BrowserPane browser(session);
    browser.setSize(260, 200);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced(); }));
    browser.selectTab(1);
    azl::ui::PresetTree& tree = browser.effects();
    checkScrolls(tree, tree.getLocalBounds().getCentre(), [&] { return tree.rows().front().first.getY(); },
                 [&] { return tree.rows().back().first.getBottom() <= tree.getHeight(); });
}

TEST_CASE("scroll: the Cues, History and Autopilot tabs scroll their rows")
{
    Engine e;
    for (int bar = 1; bar <= 8; ++bar)
        apply(e.client, azl::AddCue{std::nullopt, azl::Cue{.bar = bar, .autopilot = {{.layer = 1, .mode = azl::AutopilotMode::Next}}}});
    for (int i = 0; i < 3; ++i) {
        azl::Layer layer;
        layer.name = "Layer " + std::to_string(i);
        apply(e.client, azl::AddLayer{0, std::nullopt, layer});
    }
    azl::ui::Session session("127.0.0.1", e.server.port());
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && session.composition().cues.size() == 8; }));

    SUBCASE("Cues")
    {
        azl::ui::CuesView cues(session);
        cues.setSize(320, 130);
        const auto area = cues.listArea();
        checkScrolls(cues, area.getCentre(), [&] { return cues.removeArea(0).getY(); },
                     [&] { return area.contains(cues.removeArea(7)); });
    }
    SUBCASE("History")
    {
        REQUIRE(runUntil(e.harness, [&] { return session.pastEdits().size() == 11; }));
        azl::ui::HistoryView history(session);
        history.setSize(320, 90);
        const size_t last = history.rowCount() - 1;
        checkScrolls(history, {10, 10}, [&] { return history.rowArea(0).getY(); },
                     [&] { return history.getLocalBounds().contains(history.rowArea(last)); });
    }
    SUBCASE("Autopilot")
    {
        azl::ui::AutopilotView autopilot(session);
        autopilot.setSize(420, 90);
        const uint64_t bottom = session.composition().layers.front().id;
        checkScrolls(autopilot, {10, 10}, [&] { return autopilot.paceArea(0).getY(); },
                     [&] { return autopilot.getLocalBounds().contains(autopilot.buttonArea(bottom, 0)); });
    }
}

TEST_CASE("scroll: the Map's tree of screens scrolls")
{
    Engine e;
    for (int i = 0; i < 4; ++i)
        apply(e.client, azl::AddScreen{std::nullopt, azl::Screen{.name = "Screen " + std::to_string(i),
                                                                 .slices = {azl::Slice{.name = "A"}, azl::Slice{.name = "B"}}}});
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MapView map(session);
    map.setSize(900, 160);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && session.composition().screens.size() >= 4; }));
    const auto area = map.treeArea();
    checkScrolls(map, area.getCentre(), [&] { return map.treeRowAreas().front().getY(); },
                 [&] { return area.contains(map.treeRowAreas().back()); });
}

TEST_CASE("scroll: a deck of many columns keeps its cell size and scrolls sideways under pinned layer controls")
{
    Engine e;
    uint64_t deck = e.harness.document().document().decks[0].id;
    for (int i = 0; i < 44; ++i)
        apply(e.client, azl::AddColumn{deck, std::nullopt, {0, "Col " + std::to_string(i + 5)}, {}});
    const std::vector<azl::Column> columns = e.harness.document().document().decks[0].columns;
    REQUIRE(columns.size() == 48);
    azl::ui::Session session("127.0.0.1", e.server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1100, 700);
    REQUIRE(runUntil(e.harness, [&] { return session.client().synced() && window.grid().stack().slots() == 49; }));
    azl::ui::ClipGrid& grid = window.grid();
    azl::ui::LayerStack& stack = grid.stack();
    const int cell = azl::ui::Look::metrics().cellWidth;
    const uint64_t layer = stack.rows().front().layer;

    // The cells are the fixed width at this window and at a narrower one, and the stack is wider than the viewport.
    CHECK(stack.columnArea(0).getWidth() == cell);
    CHECK(stack.getWidth() > grid.viewport().getViewWidth());
    CHECK(stack.columnArea(47).getRight() <= stack.getWidth());
    window.setSize(700, 700);
    CHECK(stack.columnArea(0).getWidth() == cell);
    CHECK(grid.viewport().getHorizontalScrollBar().isVisible());

    // Scrolled sideways, the layer controls and the Composition header stay where they were; the column headers move.
    const auto controlBefore = stack.controlArea(layer);
    grid.viewport().setViewPosition(600, 0);
    CHECK(stack.scrollX() == 600);
    CHECK(stack.controlArea(layer).getY() == controlBefore.getY());
    CHECK(stack.controlArea(layer).getX() - 600 == controlBefore.getX());
    CHECK(stack.controlArea(layer).getRight() - 600 == controlBefore.getRight());

    // A column triggered by the engine, far to the right, is scrolled into view; one in view moves nothing.
    grid.viewport().setViewPosition(0, 0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(2100);
    e.client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[40].id}});
    REQUIRE(runUntil(e.harness, [&] { return stack.activeSlot() == 40 && stack.scrollX() > 0; }));
    const int at = grid.viewport().getViewPositionX();
    CHECK(stack.columnArea(40).getRight() <= at + grid.viewport().getViewWidth());
    CHECK(stack.columnArea(40).getX() >= stack.controlArea(layer).getRight());
    e.client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[39].id}});
    REQUIRE(runUntil(e.harness, [&] { return stack.activeSlot() == 39; }));
    CHECK(grid.viewport().getViewPositionX() == at);

    // Not while the user has just scrolled.
    grid.viewport().setViewPosition(0, 0);
    e.client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[45].id}});
    REQUIRE(runUntil(e.harness, [&] { return stack.activeSlot() == 45; }));
    CHECK(grid.viewport().getViewPositionX() == 0);
}
