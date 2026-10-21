#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>

namespace {

using MapPoint = azl::Point;
using azl::ui::MapView;

constexpr double kPi = 3.14159265358979323846;

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
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
}

juce::MouseEvent mouse(juce::Component& component, juce::Point<int> at, juce::Point<int> down,
                       juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    const juce::Time now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, &component, &component, now, down.toFloat(), now, 1, at != down);
}

// Presses at `from`, moves in steps to `to` and lets go; `during` runs before the release.
void drag(azl::EngineHarness& harness, juce::Component& component, juce::Point<int> from, juce::Point<int> to,
          juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier, const std::function<void()>& during = {})
{
    component.mouseDown(mouse(component, from, from, mods));
    for (int i = 1; i <= 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        component.mouseDrag(mouse(component, from + (to - from) * i / 10, from, mods));
    }
    if (during)
        during();
    component.mouseUp(mouse(component, to, from, mods));
}

void wheel(juce::Component& component, juce::Point<int> at, float deltaY)
{
    juce::MouseWheelDetails details{};
    details.deltaY = deltaY;
    component.mouseWheelMove(mouse(component, at, at, {}), details);
}

juce::String u8(const char* text)
{
    return juce::String::fromUTF8(text);
}

const azl::Screen& screenOf(const azl::Composition& c, uint64_t id)
{
    for (const azl::Screen& s : c.screens)
        if (s.id == id)
            return s;
    FAIL("no screen " << id);
    return c.screens.front();
}

} // namespace

TEST_CASE("map: frames, handles, zoom and pan, open large, soft edge, masks, test card, guides, corner target and the "
          "Input view, each drag one undo step")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.displays().attach({"window-1", "Left projector", 1920, 1080, 60.0});
    harness.displays().attach({"window-2", "Right projector", 1920, 1080, 60.0});
    harness.displays().attach({"window-3", "Stage projector", 1280, 720, 60.0});
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_100.mp4", media.path / "count_100.mp4"));
    const auto columns = harness.document().document().decks[0].columns;
    apply(client, azl::SetSlot{1, columns[0].id, azl::ClipRef{added["entry"]["hash"], added["entry"]["name"]}});
    auto screen = [](std::string name, std::string display, std::array<double, 4> rect) {
        return azl::Screen{.name = std::move(name), .display = std::move(display),
                           .slices = {azl::Slice{.name = "Slice", .inputRect = rect}}};
    };
    apply(client, azl::AddScreen{std::nullopt, screen("Left", "window-1", {0.0, 0.0, 0.55, 1.0})});
    apply(client, azl::AddScreen{std::nullopt, screen("Right", "window-2", {0.45, 0.0, 0.55, 1.0})});
    apply(client, azl::AddScreen{std::nullopt, screen("Top", "", {0.0, 0.0, 1.0, 1.0})});
    azl::Layer groupLayer;
    groupLayer.name = "Stage";
    groupLayer.group = true;
    const uint64_t group = apply(client, azl::AddLayer{0, std::nullopt, groupLayer})["layer"]["id"];
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
    harness.step(4);
    const auto screens = harness.document().document().screens;
    const uint64_t left = screens[0].id, right = screens[1].id;
    const uint64_t leftSlice = screens[0].slices[0].id, rightSlice = screens[1].slices[0].id;

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    MapView& map = window.map();
    azl::ui::MapCanvas& canvas = map.canvas();
    REQUIRE(runUntil(harness, [&] { return session.client().synced() && session.client().displays().size() == 3; }));
    window.setMapMode(true);
    REQUIRE(runUntil(harness, [&] {
        return session.preview("screen:" + std::to_string(left)).isValid() &&
               session.preview("screen:" + std::to_string(right)).isValid();
    }));
    auto doc = [&] { return harness.document().document(); };
    auto undo = [&] { return session.client().history().undo; };
    auto settle = [&](size_t steps) { return runUntil(harness, [&] { return undo() == steps; }); };

    // While Map shows, the outputs draw the guides; the first screen's first slice is selected.
    CHECK(runUntil(harness, [&] { return harness.outputs().guides(); }));
    CHECK(map.state().screen == left);
    CHECK(map.state().item == leftSlice);
    shoot(window, "24-smoke-all.png");

    SUBCASE("a corner drag is one undo step and the corner target follows it on that output")
    {
        const size_t before = undo();
        std::optional<MapPoint> during;
        drag(harness, canvas, canvas.screenPixel(left, {0, 0}), canvas.screenPixel(left, {0, 0}) + juce::Point<int>(40, 20),
             juce::ModifierKeys::leftButtonModifier, [&] {
                 runUntil(harness, [&] { return harness.outputs().target("window-1").has_value(); });
                 during = harness.outputs().target("window-1");
             });
        REQUIRE(settle(before + 1));
        const auto corners = screenOf(doc(), left).slices[0].corners;
        CHECK(corners[0] > 0.1);
        CHECK(corners[1] > 0.08);
        CHECK(corners[2] == doctest::Approx(1.0));
        REQUIRE(during);
        CHECK((*during)[0] > 0.05);
        // The corner stays selected, so the target stays on its output until the selection moves on.
        CHECK(runUntil(harness, [&] {
            const auto t = harness.outputs().target("window-1");
            return t && std::abs((*t)[0] - corners[0]) < 1e-6;
        }));
        map.select(right, rightSlice, false);
        CHECK(runUntil(harness, [&] { return !harness.outputs().target("window-1"); }));

        // Undo takes the whole drag back.
        session.client().undo();
        CHECK(runUntil(harness, [&] { return screenOf(doc(), left).slices[0].corners[0] == 0.0; }));
    }

    SUBCASE("an edge handle moves both of its points")
    {
        const size_t before = undo();
        const auto from = canvas.screenPixel(left, {1.0, 0.5});
        drag(harness, canvas, from, from - juce::Point<int>(30, 0));
        REQUIRE(settle(before + 1));
        const auto c = screenOf(doc(), left).slices[0].corners;
        CHECK(c[2] < 0.95);
        CHECK(c[4] == doctest::Approx(c[2]));
        CHECK(c[3] == doctest::Approx(0.0));
        CHECK(c[0] == doctest::Approx(0.0));
        CHECK(c[6] == doctest::Approx(0.0));
    }

    SUBCASE("the rotate handle with Shift snaps to 15 degrees")
    {
        const size_t before = undo();
        const auto centre = canvas.screenPixel(left, {0.5, 0.5}).toFloat();
        const auto from = canvas.rotatePixel().toFloat();
        const float angle = float(22.0 * kPi / 180.0);
        const auto v = from - centre;
        const juce::Point<float> to{centre.x + v.x * std::cos(angle) - v.y * std::sin(angle),
                                    centre.y + v.x * std::sin(angle) + v.y * std::cos(angle)};
        drag(harness, canvas, from.roundToInt(), to.roundToInt(),
             juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        REQUIRE(settle(before + 1));
        const auto expected = azl::ui::transformed({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 0, 0, 1, 15.0 * kPi / 180.0,
                                                   azl::ui::kFrameW / azl::ui::kFrameH);
        const auto c = screenOf(doc(), left).slices[0].corners;
        for (size_t k = 0; k < 4; ++k) {
            CHECK(c[k * 2] == doctest::Approx(std::clamp(expected[k][0], -1.0, 2.0)).epsilon(1e-6));
            CHECK(c[k * 2 + 1] == doctest::Approx(std::clamp(expected[k][1], -1.0, 2.0)).epsilon(1e-6));
        }
    }

    SUBCASE("the wheel zooms around the cursor and a drag on empty space pans, neither edits; Fit resets")
    {
        const size_t before = undo();
        const auto corners = screenOf(doc(), left).slices[0].corners;
        const auto at = canvas.screenPixel(left, {0.25, 0.25});
        const MapPoint board = canvas.viewport().toBoard({double(at.x), double(at.y)});
        for (int i = 0; i < 5; ++i)
            wheel(canvas, at, 0.2f);
        CHECK(canvas.zoomPercent() > 200);
        const MapPoint after = canvas.viewport().toBoard({double(at.x), double(at.y)});
        CHECK(after[0] == doctest::Approx(board[0]));
        CHECK(after[1] == doctest::Approx(board[1]));
        const auto offset = canvas.viewport().offset;
        const juce::Point<int> empty{canvas.getWidth() - 10, canvas.getHeight() - 10};
        drag(harness, canvas, empty, empty - juce::Point<int>(100, 50));
        CHECK(canvas.viewport().offset[0] == doctest::Approx(offset[0] - 100));
        CHECK(canvas.viewport().offset[1] == doctest::Approx(offset[1] - 50));
        map.toolbarButton("Fit")->onClick();
        CHECK(canvas.zoomPercent() == 100);
        pump(harness, 200);
        CHECK(undo() == before);
        CHECK(screenOf(doc(), left).slices[0].corners == corners);
    }

    SUBCASE("double-clicking a screen's name opens it large; Esc goes back")
    {
        const auto title = canvas.titlePixel(right);
        canvas.mouseDoubleClick(mouse(canvas, title, title));
        CHECK(map.state().focus == right);
        CHECK(map.state().screen == right);
        CHECK(map.toolbarButton(u8("\xe2\x86\x90 All screens"))->isVisible());
        CHECK(canvas.screenPixel(right, {1, 1}).x - canvas.screenPixel(right, {0, 0}).x > 900);
        pump(harness, 300);
        shoot(window, "24-smoke-large.png");
        CHECK(window.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        CHECK(map.state().focus == 0);
    }

    SUBCASE("Auto from overlap fades both slices across their overlap in one step")
    {
        const size_t before = undo();
        REQUIRE(runUntil(harness, [&] { return map.inspector().button("Auto from overlap") != nullptr; }));
        map.inspector().button("Auto from overlap")->onClick();
        REQUIRE(settle(before + 1));
        const auto d = doc();
        CHECK(screenOf(d, left).slices[0].softEdge.right > 0.1);
        CHECK(screenOf(d, right).slices[0].softEdge.left > 0.1);
        CHECK(runUntil(harness, [&] {
            auto* f = map.inspector().field(u8("L \xc2\xb7 R %"), 1);
            return f && f->value() > 10.0;
        }));
    }

    SUBCASE("a mask is added, selected and inverted")
    {
        map.toolbarButton("+ Mask")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).masks.size() == 1 && map.state().mask; }));
        CHECK(map.state().maskSlice == 0);
        CHECK(map.state().item == screenOf(doc(), left).masks[0].id);
        REQUIRE(runUntil(harness, [&] { return map.inspector().button("Inverted: shows only inside") != nullptr; }));
        map.inspector().button("Inverted: shows only inside")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).masks[0].invert; }));
        // A mask moves by dragging inside it.
        const size_t before = undo();
        const auto inside = canvas.screenPixel(left, {0.5, 0.5});
        drag(harness, canvas, inside, inside + juce::Point<int>(20, 0));
        REQUIRE(settle(before + 1));
        CHECK(screenOf(doc(), left).masks[0].points[0][0] > 0.36);
        CHECK(screenOf(doc(), left).slices[0].corners[0] == 0.0);
    }

    SUBCASE("test card per screen and on all screens")
    {
        REQUIRE(runUntil(harness, [&] { return map.inspector().button("Test card") != nullptr; }));
        map.inspector().button("Test card")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).testCard; }));
        CHECK(!screenOf(doc(), right).testCard);
        map.toolbarButton("Test card: all screens")->onClick();
        REQUIRE(runUntil(harness, [&] { return doc().testCard; }));
        CHECK(runUntil(harness, [&] { return map.toolbarButton("Test card: all screens")->getToggleState(); }));
        pump(harness, 300);
        shoot(window, "24-smoke-testcard.png");
        map.toolbarButton("Test card: all screens")->onClick();
        REQUIRE(runUntil(harness, [&] { return !doc().testCard; }));
    }

    SUBCASE("guides are document edits, shown on the outputs only while Map is open")
    {
        map.toolbarButton("+ Guide |")->onClick();
        REQUIRE(runUntil(harness, [&] { return doc().guides.size() == 1 && doc().guides[0].vertical; }));
        REQUIRE(runUntil(harness, [&] { return map.inspector().field("Vertical") != nullptr; }));
        const size_t before = undo();
        azl::ui::NumberField& field = *map.inspector().field("Vertical");
        for (int i = 0; i < 3; ++i)
            wheel(field, {10, 5}, 0.1f);
        pump(harness, 600);
        REQUIRE(settle(before + 1));
        CHECK(doc().guides[0].position == doctest::Approx(0.503));
        window.setMapMode(false);
        CHECK(runUntil(harness, [&] { return !harness.outputs().guides(); }));
        window.setMapMode(true);
        CHECK(runUntil(harness, [&] { return harness.outputs().guides(); }));
    }

    SUBCASE("inspector corner fields drag as one step; relative Move goes back to 0")
    {
        REQUIRE(runUntil(harness, [&] { return map.inspector().field("TR") != nullptr; }));
        const size_t before = undo();
        azl::ui::NumberField& tr = *map.inspector().field("TR");
        drag(harness, tr, {40, 5}, {10, 5});
        REQUIRE(settle(before + 1));
        CHECK(screenOf(doc(), left).slices[0].corners[2] == doctest::Approx(1.0 - 10.0 / 1920.0));
        azl::ui::NumberField& move = *map.inspector().field(u8("Move X \xc2\xb7 Y"));
        drag(harness, move, {10, 5}, {40, 5});
        REQUIRE(settle(before + 2));
        CHECK(screenOf(doc(), left).slices[0].corners[0] == doctest::Approx(10.0 / 1920.0));
        CHECK(runUntil(harness, [&] { return move.value() == 0.0; }));
    }

    SUBCASE("Bezier warp 3x3: grid points are handles on the surface the engine renders; arrows nudge them")
    {
        map.state().targets = false;
        map.changed();
        // The whole composition, so the slice's centre shows the test card's centre cross.
        apply(client, azl::SetSliceInput{leftSlice, std::nullopt, std::array<double, 4>{0.0, 0.0, 1.0, 1.0}});
        REQUIRE(runUntil(harness, [&] { return map.slice() && map.slice()->inputRect[2] == 1.0; }));
        REQUIRE(runUntil(harness, [&] { return map.inspector().choiceFor("Warp") != nullptr; }));
        size_t before = undo();
        map.inspector().choiceFor("Warp")->setSelectedItemIndex(2, juce::sendNotificationSync);
        REQUIRE(settle(before + 1));
        azl::Slice slice = screenOf(doc(), left).slices[0];
        REQUIRE(azl::ui::warpAnchors(slice) == std::array<int, 2>{3, 3});
        CHECK(slice.corners == std::array<double, 8>{0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0});
        REQUIRE(runUntil(harness, [&] { return map.shape().size() == 9; }));

        // The centre point dragged: one undo step, and it stays exactly on the engine's surface.
        before = undo();
        const auto from = canvas.screenPixel(left, {0.5, 0.5});
        drag(harness, canvas, from, from + juce::Point<int>(36, 24));
        REQUIRE(settle(before + 1));
        slice = screenOf(doc(), left).slices[0];
        const MapPoint centre = azl::ui::anchorsOf(slice)[4];
        const MapPoint onSurface = azl::ui::warpSurfaceAt(slice.warp, {0.5, 0.5});
        CHECK(onSurface[0] == doctest::Approx(centre[0]).epsilon(1e-12));
        CHECK(onSurface[1] == doctest::Approx(centre[1]).epsilon(1e-12));
        const double perPixel = 1.0 / (canvas.screenPixel(left, {1, 0}).x - canvas.screenPixel(left, {0, 0}).x);
        CHECK(centre[0] == doctest::Approx(0.5 + 36 * perPixel).epsilon(0.02));
        CHECK(centre[0] > 0.55);
        CHECK(map.state().point == 4);

        // The rendered output: the test card's centre cross lies under the dragged point.
        map.inspector().button("Test card")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).testCard; }));
        harness.step(2);
        const azl::Image picture = harness.readScreen(left);
        auto whiteAround = [&](MapPoint p) {
            const int cx = int(p[0] * picture.width), cy = int(p[1] * picture.height);
            int white = 0;
            for (int y = cy - 1; y <= cy + 1; ++y)
                for (int x = cx - 1; x <= cx + 1; ++x) {
                    const uint8_t* px = picture.rgba.data() + (size_t(y) * picture.width + size_t(x)) * 4;
                    white += px[0] > 220 && px[1] > 220 && px[2] > 220;
                }
            return white;
        };
        CHECK(whiteAround(centre) >= 5);
        CHECK(whiteAround({0.5, 0.5}) < 5);

        // Arrows nudge the selected point by one output pixel, Shift by ten.
        before = undo();
        CHECK(window.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        REQUIRE(settle(before + 1));
        CHECK(azl::ui::anchorsOf(screenOf(doc(), left).slices[0])[4][0] == doctest::Approx(centre[0] + 1.0 / 1920));
        CHECK(window.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)));
        REQUIRE(settle(before + 2));
        CHECK(azl::ui::anchorsOf(screenOf(doc(), left).slices[0])[4][1] == doctest::Approx(centre[1] + 10.0 / 1080));
        pump(harness, 300);
        shoot(window, "24-smoke-warp.png");

        // Flatten keeps the outer corners; Off goes back to a corner pin through them.
        const azl::ui::Points outer = azl::ui::outerCorners(azl::ui::anchorsOf(screenOf(doc(), left).slices[0]), 3, 3);
        map.inspector().button("Flatten to corners")->onClick();
        REQUIRE(runUntil(harness, [&] {
            return std::abs(azl::ui::anchorsOf(screenOf(doc(), left).slices[0])[4][0] - 0.5) < 1e-9;
        }));
        REQUIRE(runUntil(harness, [&] { return map.inspector().choiceFor("Warp") != nullptr; }));
        map.inspector().choiceFor("Warp")->setSelectedItemIndex(0, juce::sendNotificationSync);
        REQUIRE(runUntil(harness, [&] { return azl::ui::warpAnchors(screenOf(doc(), left).slices[0])[0] == 0; }));
        CHECK(azl::ui::cornersOf(screenOf(doc(), left).slices[0]) == outer);
    }

    SUBCASE("Bezier corners: corner and handle drags, cubic edges as the engine renders them, Coons interior")
    {
        map.state().targets = false;
        map.changed();
        apply(client, azl::SetSliceInput{leftSlice, std::nullopt, std::array<double, 4>{0.0, 0.0, 1.0, 1.0}});
        REQUIRE(runUntil(harness, [&] { return map.slice() && map.slice()->inputRect[2] == 1.0; }));
        REQUIRE(runUntil(harness, [&] { return map.inspector().choiceFor("Warp") != nullptr; }));
        CHECK(map.inspector().choiceFor("Warp")->getItemText(1) == juce::String::fromUTF8("Bezier \xc2\xb7 corners"));
        CHECK(map.inspector().choiceFor("Warp")->getItemText(2) == juce::String::fromUTF8("Grid 3 \xc3\x97 3"));
        size_t before = undo();
        map.inspector().choiceFor("Warp")->setSelectedItemIndex(1, juce::sendNotificationSync);
        REQUIRE(settle(before + 1));
        REQUIRE(azl::ui::isCornerWarp(screenOf(doc(), left).slices[0].warp));
        REQUIRE(runUntil(harness, [&] { return map.shape().size() == 12; }));
        const azl::ui::Points start = map.shape();

        // A corner takes its two handles along.
        before = undo();
        const auto tl = canvas.screenPixel(left, start[0]);
        drag(harness, canvas, tl, tl + juce::Point<int>(20, 14));
        REQUIRE(settle(before + 1));
        azl::ui::Points now = azl::ui::cornerPointsOf(screenOf(doc(), left).slices[0]);
        const double dx = now[0][0] - start[0][0], dy = now[0][1] - start[0][1];
        CHECK(dx > 0.03);
        for (int h : {4, 11}) {
            CHECK(now[size_t(h)][0] == doctest::Approx(start[size_t(h)][0] + dx));
            CHECK(now[size_t(h)][1] == doctest::Approx(start[size_t(h)][1] + dy));
        }
        CHECK(now[1] == start[1]);

        // A round handle bends only its own edge: the top's handle near TR.
        before = undo();
        const auto handle = canvas.screenPixel(left, now[5]);
        drag(harness, canvas, handle, handle + juce::Point<int>(0, 40));
        REQUIRE(settle(before + 1));
        const azl::Slice bent = screenOf(doc(), left).slices[0];
        const azl::ui::Points after = azl::ui::cornerPointsOf(bent);
        CHECK(after[5][1] > now[5][1] + 0.1);
        for (int k : {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11})
            CHECK(after[size_t(k)] == now[size_t(k)]);
        CHECK(map.state().point == 5);

        // The rendered top edge follows the cubic curve: the test card's red border lies just inside it, black outside.
        map.inspector().button("Test card")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).testCard; }));
        harness.step(2);
        const azl::Image picture = harness.readScreen(left);
        auto pixel = [&](double x, double y) {
            const size_t at = (size_t(y * picture.height) * picture.width + size_t(x * picture.width)) * 4;
            return std::array<int, 3>{picture.rgba[at], picture.rgba[at + 1], picture.rgba[at + 2]};
        };
        for (double s : {0.25, 0.5, 0.75}) {
            const double u = 1 - s;
            MapPoint p;
            for (int k = 0; k < 2; ++k)
                p[size_t(k)] = u * u * u * after[0][size_t(k)] + 3 * u * u * s * after[4][size_t(k)] +
                               3 * u * s * s * after[5][size_t(k)] + s * s * s * after[1][size_t(k)];
            CAPTURE(s);
            const auto in = pixel(p[0], p[1] + 1.0 / picture.height);
            CHECK(in[0] > 200);
            CHECK(in[1] < 60);
            const auto out = pixel(p[0], p[1] - 4.0 / picture.height);
            CHECK(out[0] < 40);
        }
        // Inside, the surface is the Coons patch of the four cubic edges.
        auto cubic = [](MapPoint a, MapPoint b, MapPoint c, MapPoint d, double s) {
            const double u = 1 - s;
            return MapPoint{u * u * u * a[0] + 3 * u * u * s * b[0] + 3 * u * s * s * c[0] + s * s * s * d[0],
                            u * u * u * a[1] + 3 * u * u * s * b[1] + 3 * u * s * s * c[1] + s * s * s * d[1]};
        };
        const auto& q = after;
        for (MapPoint uv : {MapPoint{0.3, 0.6}, MapPoint{0.5, 0.5}, MapPoint{0.8, 0.2}}) {
            const double u = uv[0], v = uv[1];
            const MapPoint top = cubic(q[0], q[4], q[5], q[1], u), bottom = cubic(q[3], q[9], q[8], q[2], u);
            const MapPoint leftEdge = cubic(q[0], q[11], q[10], q[3], v), rightEdge = cubic(q[1], q[6], q[7], q[2], v);
            MapPoint coons;
            for (size_t k = 0; k < 2; ++k)
                coons[k] = (1 - v) * top[k] + v * bottom[k] + (1 - u) * leftEdge[k] + u * rightEdge[k] -
                           ((1 - u) * (1 - v) * q[0][k] + u * (1 - v) * q[1][k] + u * v * q[2][k] + (1 - u) * v * q[3][k]);
            const MapPoint surface = azl::ui::warpSurfaceAt(bent.warp, uv);
            CHECK(surface[0] == doctest::Approx(coons[0]).epsilon(1e-9));
            CHECK(surface[1] == doctest::Approx(coons[1]).epsilon(1e-9));
        }
        pump(harness, 300);
        shoot(window, "24-smoke-corners.png");

        // Off keeps the four corners as the corner pin.
        map.inspector().choiceFor("Warp")->setSelectedItemIndex(0, juce::sendNotificationSync);
        REQUIRE(runUntil(harness, [&] { return !azl::ui::isCornerWarp(screenOf(doc(), left).slices[0].warp); }));
        const auto pin = azl::ui::cornersOf(screenOf(doc(), left).slices[0]);
        for (size_t k = 0; k < 4; ++k)
            CHECK(pin[k] == after[k]);
    }

    SUBCASE("a slice mask is added, follows its slice, and drags on the canvas in one step")
    {
        REQUIRE(runUntil(harness, [&] { return map.inspector().button("+ Mask on slice") != nullptr; }));
        map.inspector().button("+ Mask on slice")->onClick();
        REQUIRE(runUntil(harness, [&] {
            return screenOf(doc(), left).slices[0].masks.size() == 1 && map.state().mask && map.state().maskSlice == leftSlice;
        }));
        const azl::Mask added = screenOf(doc(), left).slices[0].masks[0];
        CHECK(map.state().item == added.id);
        CHECK(added.points[0][0] == doctest::Approx(0.35 * 0.55));
        const size_t before = undo();
        const auto inside = canvas.screenPixel(left, azl::ui::centroid(map.shape()));
        drag(harness, canvas, inside, inside + juce::Point<int>(30, 10));
        REQUIRE(settle(before + 1));
        const azl::Mask moved = screenOf(doc(), left).slices[0].masks[0];
        CHECK(moved.points[0][0] > added.points[0][0] + 0.01);
        CHECK(moved.points[0][1] > added.points[0][1]);
        CHECK(screenOf(doc(), left).masks.empty());
        CHECK(screenOf(doc(), left).slices[0].corners[0] == 0.0);
        map.inspector().button("Inverted: shows only inside")->onClick();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).slices[0].masks[0].invert; }));
        pump(harness, 300);
        shoot(window, "24-smoke-slice-mask.png");
    }

    SUBCASE("in the tree a double-click on a screen's name opens it large; F2 renames it, as one undo step")
    {
        const juce::Point<int> rightName{20, 22 + 23 + 20 + 8};
        map.mouseDoubleClick(mouse(map, rightName, rightName));
        CHECK(map.state().focus == right);
        CHECK(map.escape());
        CHECK(map.state().focus == 0);

        const juce::Point<int> leftName{20, 22 + 8};
        map.mouseDown(mouse(map, leftName, leftName));
        map.mouseUp(mouse(map, leftName, leftName));
        REQUIRE(map.state().screen == left);
        const size_t before = undo();
        CHECK(window.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
        juce::TextEditor* editor = nullptr;
        for (juce::Component* child : map.getChildren())
            if (auto* e = dynamic_cast<juce::TextEditor*>(child); e && e->isVisible())
                editor = e;
        REQUIRE(editor);
        CHECK(editor->getText() == "Left");
        editor->setText("Left wall");
        editor->onReturnKey();
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), left).name == "Left wall"; }));
        CHECK(settle(before + 1));
    }

    SUBCASE("assigning a group to a projector from the group's card creates its Screen in one step")
    {
        window.setMapMode(false);
        session.selectLayer(group);
        azl::ui::OwnerCard& card = window.chain().content().ownerCard();
        REQUIRE(runUntil(harness, [&] { return card.choice("Projector") != nullptr; }));
        const size_t before = undo();
        card.choice("Projector")->setSelectedItemIndex(1, juce::sendNotificationSync);
        const bool made = runUntil(harness, [&] { return doc().screens.size() == 4; });
        INFO(session.status().toStdString());
        REQUIRE(made);
        const azl::Screen created = doc().screens.back();
        CHECK(created.display == "window-3");
        CHECK(created.name == "Stage");
        REQUIRE(created.slices.size() == 1);
        CHECK(created.slices[0].input == group);
        CHECK(settle(before + 1));
    }

    SUBCASE("Input view: edges snap to other slices (not with Alt), a drag moves, Shift+drag draws a new slice")
    {
        map.toolbarButton("Input")->onClick();
        CHECK(map.state().input);
        pump(harness, 200);
        // The selected slice's overlap with the right one is hatched; its right edge snaps to the other's left edge.
        size_t before = undo();
        drag(harness, canvas, canvas.inputPixel({0.55, 0.5}), canvas.inputPixel({0.453, 0.5}));
        REQUIRE(settle(before + 1));
        CHECK(screenOf(doc(), left).slices[0].inputRect[2] == doctest::Approx(0.45));
        CHECK(runUntil(harness, [&] {
            auto* f = map.inspector().field(u8("Input W \xc2\xb7 H"));
            return f && std::abs(f->value() - 45.0) < 0.05;
        }));
        before = undo();
        drag(harness, canvas, canvas.inputPixel({0.45, 0.5}), canvas.inputPixel({0.5, 0.5}),
             juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);
        REQUIRE(settle(before + 1));
        CHECK(screenOf(doc(), left).slices[0].inputRect[2] == doctest::Approx(0.5).epsilon(0.01));
        pump(harness, 300);
        shoot(window, "24-smoke-input.png");

        before = undo();
        drag(harness, canvas, canvas.inputPixel({0.1, 0.5}), canvas.inputPixel({0.2, 0.6}));
        REQUIRE(settle(before + 1));
        CHECK(screenOf(doc(), left).slices[0].inputRect[0] == doctest::Approx(0.1).epsilon(0.02));

        // Clicking the right slice's rectangle selects it, in the Output view and the inspector too.
        const auto onRight = canvas.inputPixel({0.95, 0.2});
        canvas.mouseDown(mouse(canvas, onRight, onRight));
        canvas.mouseUp(mouse(canvas, onRight, onRight));
        CHECK(map.state().screen == right);
        CHECK(map.state().item == rightSlice);

        const size_t slices = screenOf(doc(), right).slices.size();
        drag(harness, canvas, canvas.inputPixel({0.6, 0.05}), canvas.inputPixel({0.8, 0.3}),
             juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        REQUIRE(runUntil(harness, [&] { return screenOf(doc(), right).slices.size() == slices + 1; }));
        const azl::Slice drawn = screenOf(doc(), right).slices.back();
        CHECK(drawn.inputRect[0] == doctest::Approx(0.6).epsilon(0.02));
        CHECK(drawn.inputRect[2] == doctest::Approx(0.2).epsilon(0.05));
        CHECK(runUntil(harness, [&] { return map.state().item == drawn.id; }));
        map.toolbarButton("Output")->onClick();
        CHECK(!map.state().input);
    }
}

TEST_CASE("map: a frame drags by its title to a saved position, the eye hides a screen from the canvas, and the "
          "screens list reorders by drag, each one undo step")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.displays().attach({"window-1", "Projector", 1920, 1080, 60.0});
    harness.displays().attach({"window-2", "TV", 1920, 1080, 60.0});
    Client client(harness.document());
    for (const auto& [name, display] :
         {std::pair{"Projector", "window-1"}, std::pair{"TV", "window-2"}, std::pair{"Booth", ""}})
        apply(client, azl::AddScreen{std::nullopt, azl::Screen{.name = name, .display = display, .slices = {{}}}});
    apply(client, azl::SetScreenTestCard{0, true});
    harness.step(4);
    const auto screens = harness.document().document().screens;
    const uint64_t projector = screens[0].id, tv = screens[1].id, booth = screens[2].id;

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    MapView& map = window.map();
    azl::ui::MapCanvas& canvas = map.canvas();
    REQUIRE(runUntil(harness, [&] { return session.client().synced() && session.client().displays().size() == 2; }));
    window.setMapMode(true);
    REQUIRE(runUntil(harness, [&] { return session.preview("screen:" + std::to_string(projector)).isValid(); }));
    auto doc = [&] { return harness.document().document(); };
    auto undo = [&] { return session.client().history().undo; };
    auto settle = [&](size_t steps) { return runUntil(harness, [&] { return undo() == steps; }); };
    auto order = [&] {
        std::vector<uint64_t> ids;
        for (const azl::Screen& s : doc().screens)
            ids.push_back(s.id);
        return ids;
    };
    auto shown = [&](uint64_t id) {
        const auto list = canvas.shownScreens();
        return std::any_of(list.begin(), list.end(), [&](const azl::Screen* s) { return s->id == id; });
    };
    auto rowOf = [&](uint64_t id) {
        const auto areas = map.treeRowAreas();
        size_t row = 0;
        for (const azl::Screen& s : session.composition().screens) {
            if (s.id == id)
                return areas[row];
            row += 1 + s.slices.size();
        }
        FAIL("no row for screen " << id);
        return areas.front();
    };
    auto clickEye = [&](uint64_t id) {
        const auto eye = MapView::eyeArea(rowOf(id)).getCentre();
        map.mouseDown(mouse(map, eye, eye));
        map.mouseUp(mouse(map, eye, eye));
    };

    // No saved positions: the grid, three to a row.
    CHECK(canvas.screenPixel(tv, {0, 0}).y == canvas.screenPixel(projector, {0, 0}).y);
    CHECK(canvas.screenPixel(tv, {0, 0}).x > canvas.screenPixel(projector, {0, 0}).x);

    // Dragging the title moves the frame; the whole drag is one undo step.
    size_t before = undo();
    const auto title = canvas.titlePixel(projector);
    const auto from = canvas.screenPixel(projector, {0, 0});
    drag(harness, canvas, title, title + juce::Point<int>(60, 260));
    REQUIRE(settle(before + 1));
    const auto position = screenOf(doc(), projector).mapPosition;
    REQUIRE(position);
    CHECK(!screenOf(doc(), tv).mapPosition);
    CHECK(runUntil(harness, [&] { return std::abs(canvas.screenPixel(projector, {0, 0}).y - (from.y + 260)) <= 2; }));
    CHECK(std::abs(canvas.screenPixel(projector, {0, 0}).x - (from.x + 60)) <= 2);
    session.client().undo();
    CHECK(runUntil(harness, [&] { return !screenOf(doc(), projector).mapPosition; }));
    session.client().redo();
    CHECK(runUntil(harness, [&] { return screenOf(doc(), projector).mapPosition == position; }));

    // The eye on the TV's row hides it from the canvas; its output runs on.
    before = undo();
    clickEye(tv);
    REQUIRE(settle(before + 1));
    CHECK(screenOf(doc(), tv).hidden);
    CHECK(runUntil(harness, [&] { return !shown(tv); }));
    CHECK(shown(projector));
    const uint64_t presents = harness.outputs().find("window-2")->stats().presents.load();
    CHECK(runUntil(harness, [&] { return harness.outputs().find("window-2")->stats().presents.load() > presents + 5; }));

    // Dragging the Booth's row onto the Projector's puts it first.
    before = undo();
    drag(harness, map, rowOf(booth).getCentre(), rowOf(projector).getCentre());
    REQUIRE(settle(before + 1));
    CHECK(order() == std::vector<uint64_t>{booth, projector, tv});
    pump(harness, 300);
    shoot(window, "83-map-arranged.png");

    session.client().undo();
    CHECK(runUntil(harness, [&] { return order() == std::vector<uint64_t>{projector, tv, booth}; }));
    clickEye(tv);
    CHECK(runUntil(harness, [&] { return !screenOf(doc(), tv).hidden && shown(tv); }));
}
