#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <cmath>
#include <functional>
#include <thread>

namespace {

using MapPoint = azl::Point;

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

void drag(azl::EngineHarness& harness, juce::Component& component, juce::Point<int> from, juce::Point<int> to,
          juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    component.mouseDown(mouse(component, from, from, mods));
    for (int i = 1; i <= 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        component.mouseDrag(mouse(component, from + (to - from) * i / 10, from, mods));
    }
    component.mouseUp(mouse(component, to, from, mods));
}

} // namespace

TEST_CASE("monitor handles: the selected layer's Transform moves, scales and rotates, a text clip moves and resizes, "
          "on the composition or a screen's view, each drag one undo step")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_100.mp4", media.path / "count_100.mp4"));
    const auto columns = harness.document().document().decks[0].columns;
    apply(client, azl::SetSlot{1, columns[0].id, azl::ClipRef{added["entry"]["hash"], added["entry"]["name"]}});
    azl::Layer textLayer;
    textLayer.name = "Words";
    const uint64_t words = apply(client, azl::AddLayer{0, std::nullopt, textLayer})["layer"]["id"];
    azl::ClipRef text{.name = "text"};
    text.text = azl::TextClip{.text = "HELLO", .size = 0.15, .x = 0.5, .y = 0.3};
    apply(client, azl::SetSlot{words, columns[0].id, text});
    apply(client, azl::AddScreen{std::nullopt,
                                 azl::Screen{.name = "Keystone",
                                             .slices = {azl::Slice{.corners = {0.1, 0.05, 0.9, 0.12, 0.95, 0.95, 0.05, 0.9}}}}});
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
    harness.step(4);

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    azl::ui::CompositionMonitor& monitor = window.monitor();
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    auto doc = [&] { return harness.document().document(); };
    auto undo = [&] { return session.client().history().undo; };
    auto settle = [&](size_t steps) { return runUntil(harness, [&] { return undo() == steps; }); };
    auto transformOf = [&]() -> std::optional<azl::Effect> {
        for (const azl::Layer& l : doc().layers)
            if (l.id == 1)
                for (const azl::Effect& e : l.effects)
                    if (e.type == "transform")
                        return e;
        return std::nullopt;
    };
    auto boxCentre = [&] {
        const auto h = monitor.handles();
        return ((h[0] + h[2]) / 2.0f).roundToInt();
    };

    session.selectLayer(1);
    REQUIRE(runUntil(harness, [&] { return monitor.handles().size() == 5; }));

    // Without a Transform the box is the whole picture; a drag inside adds one at the front of the chain and moves it.
    size_t before = undo();
    const float width = monitor.handles()[1].x - monitor.handles()[0].x;
    drag(harness, monitor, boxCentre(), boxCentre() + juce::Point<int>(int(width * 0.2f), 0));
    REQUIRE(settle(before + 1));
    REQUIRE(transformOf());
    CHECK(doc().layers[0].effects[0].type == "transform");
    CHECK(transformOf()->params.at("positionX") == doctest::Approx(0.2).epsilon(0.05));
    CHECK(transformOf()->params.at("positionY") == doctest::Approx(0.0));

    // A corner scales.
    REQUIRE(runUntil(harness, [&] { return monitor.handles()[0].x > monitor.pixelOf({0.0, 0.0})->x + 5; }));
    before = undo();
    const auto corner = monitor.handles()[2].roundToInt();
    drag(harness, monitor, corner, boxCentre() + (corner - boxCentre()) / 2);
    REQUIRE(settle(before + 1));
    CHECK(transformOf()->params.at("scale") == doctest::Approx(0.5).epsilon(0.05));

    // The round handle rotates; Shift snaps to 15 degrees, counter-clockwise positive.
    REQUIRE(runUntil(harness, [&] { return monitor.handles()[1].x - monitor.handles()[0].x < width * 0.6f; }));
    before = undo();
    const auto pts = monitor.handles();
    const auto c = (pts[0] + pts[2]) / 2.0f;
    const auto v = pts[4] - c;
    const float a = float(40.0 * 3.14159265358979323846 / 180.0);
    const juce::Point<float> to{c.x + v.x * std::cos(a) - v.y * std::sin(a), c.y + v.x * std::sin(a) + v.y * std::cos(a)};
    drag(harness, monitor, pts[4].roundToInt(), to.roundToInt(),
         juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    REQUIRE(settle(before + 1));
    CHECK(transformOf()->params.at("rotation") == doctest::Approx(-45.0));
    pump(harness, 300);
    shoot(window, "24-monitor-transform.png");

    // On a screen's view the handles follow its mapping, and a drag still moves the Transform.
    session.setMonitorSource("screen:" + std::to_string(doc().screens[0].id));
    const azl::Slice keystone = doc().screens[0].slices[0];
    const auto r0 = *monitor.pixelOf({0.0, 0.0}), r1 = *monitor.pixelOf({1.0, 1.0});
    const MapPoint expected = azl::ui::sliceToScreen(keystone, {0.0, 0.0});
    CHECK(r0.x < r1.x);
    const auto full = monitor.handles();
    CHECK(full.size() == 5);
    CHECK(expected[0] == doctest::Approx(0.1));
    before = undo();
    const double px = transformOf()->params.at("positionX");
    drag(harness, monitor, boxCentre(), boxCentre() + juce::Point<int>(0, 20));
    REQUIRE(settle(before + 1));
    CHECK(transformOf()->params.at("positionY") < -0.02);
    CHECK(transformOf()->params.at("positionX") == doctest::Approx(px).epsilon(0.05));
    session.setMonitorSource("composition");

    // A text clip's handles move it and change its size.
    session.selectLayer(words);
    REQUIRE(runUntil(harness, [&] { return monitor.handles().size() == 4; }));
    auto textNow = [&] {
        for (const azl::Layer& l : doc().layers)
            if (l.id == words)
                return *l.clips.at(columns[0].id).text;
        return azl::TextClip{};
    };
    before = undo();
    drag(harness, monitor, boxCentre(), boxCentre() + juce::Point<int>(0, 30));
    REQUIRE(settle(before + 1));
    CHECK(textNow().y > 0.35);
    CHECK(textNow().x == doctest::Approx(0.5));
    REQUIRE(runUntil(harness, [&] { return monitor.handles()[0].y > monitor.pixelOf({0.5, 0.3})->y - 5; }));
    before = undo();
    const auto textCorner = monitor.handles()[2].roundToInt();
    drag(harness, monitor, textCorner, boxCentre() + (textCorner - boxCentre()) * 2);
    REQUIRE(settle(before + 1));
    CHECK(textNow().size == doctest::Approx(0.3).epsilon(0.08));
    pump(harness, 300);
    shoot(window, "24-monitor-text.png");
}
