#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"
#include "upload/media_sync.hpp"

#include <atomic>
#include <fstream>
#include <functional>

namespace {

using azl::ui::Session;
using Tab = azl::ui::TransportBar::Tab;

fs::path shotsDir()
{
    const char* dir = std::getenv("NOCTURNE_UI_SHOTS");
    return dir ? dir : NOCTURNE_UI_SHOTS;
}

void writePng(juce::Component& component, const std::string& name)
{
    fs::create_directories(shotsDir());
    juce::File file(juce::String((shotsDir() / name).string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(
        component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f), out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

bool runUntil(const std::function<bool()>& done, double seconds = 20.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        if (done())
            return true;
    }
    return false;
}

// A venue machine with four screens: proj-a healthy, proj-b starved, proj-c mirroring the laptop, proj-d unplugged.
azl::Composition fourScreens()
{
    azl::Composition composition = azl::defaultComposition();
    const char* const displays[] = {"proj-a", "proj-b", "proj-c", "proj-d"};
    for (uint64_t id = 1; id <= 4; ++id)
        composition.screens.push_back({.id = id,
                                       .name = "Screen " + std::to_string(id),
                                       .display = displays[id - 1],
                                       .slices = {{.id = id, .name = "Slice " + std::to_string(id)}}});
    composition.cues = {{.id = 1, .name = "Headliner", .bar = 9}};
    return composition;
}

} // namespace

TEST_CASE("the Machines tab watches the machine only while it is visible, and flags low-rate, mirrored and missing outputs")
{
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, fourScreens());
    for (const char* id : {"laptop", "proj-a", "proj-b", "proj-c", "proj-d"})
        harness.displays().attach({id, std::string("Epson ") + id, 640, 360, 60.0});
    harness.outputs().setDelay("proj-b", 60);
    harness.displays().mirror("proj-c", "laptop");
    harness.displays().detach("proj-d");
    harness.setThermal({.cpuSpeedLimit = 100.0, .temperatureC = 61.0, .state = "nominal"});
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    CHECK(harness.machine().subscribers() == 0);
    CHECK_FALSE(session.client().watchingMachine());

    harness.startLive();
    main.transport().tabButton(Tab::Machines).triggerClick();
    REQUIRE(runUntil([&] { return main.machines().isVisible() && harness.machine().subscribers() == 1; }));
    CHECK(main.transport().tabButton(Tab::Machines).getToggleState());
    CHECK_FALSE(main.grid().isVisible());
    CHECK_FALSE(main.map().isVisible());

    juce::StringArray lines;
    const bool flagged = runUntil([&] {
        lines = main.machines().outputLines();
        return lines.size() == 4 && lines[0].endsWith("ok") && lines[1].endsWith("low rate");
    });
    INFO(lines.joinIntoString("\n"));
    REQUIRE(flagged);
    CHECK(lines[0].startsWith(juce::String::fromUTF8("Screen 1 \xc2\xb7 Epson proj-a")));
    CHECK(lines[2].endsWith("mirrored"));
    CHECK(lines[3].endsWith("missing"));
    CHECK(lines[3].contains("Epson proj-d"));
    const std::optional<azl::ui::MachineStatus> status = session.client().machine();
    REQUIRE(status);
    CHECK(status->nextCue->name == "Headliner");
    CHECK(status->temperatureC == 61.0);

    main.machines().toggleOutput(1);
    CHECK(main.machines().expanded(1));
    runUntil([] { return false; }, 1.5);
    CHECK(session.client().machine()->fps);
    writePng(main, "machines-tab.png");
    harness.stopLive();

    main.setTab(Tab::Perform);
    CHECK_FALSE(main.machines().isVisible());
    CHECK(main.grid().isVisible());
    REQUIRE(runUntil([&] { return harness.machine().subscribers() == 0; }));
    CHECK_FALSE(session.client().watchingMachine());
}

TEST_CASE("Restart engine and Safe mode act only when held: a short press sends nothing, a full hold sends once")
{
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, fourScreens());
    for (const char* id : {"proj-a", "proj-b", "proj-c", "proj-d"})
        harness.displays().attach({id, std::string("Epson ") + id, 640, 360, 60.0});
    std::atomic<int> restarts{0}, safeModes{0};
    harness.onRestart([&](bool safeMode) {
        if (safeMode)
            ++safeModes;
        else
            ++restarts;
    });
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    main.transport().tabButton(Tab::Machines).triggerClick();
    azl::ui::MachinesView& machines = main.machines();
    REQUIRE(runUntil([&] { return machines.restartButton().isVisible(); }));
    const auto wait = [](double seconds) { runUntil([] { return false; }, seconds); };

    azl::ui::HoldButton& restart = machines.restartButton();
    restart.press();
    wait(1.0);
    CHECK(restart.progress() > 0.5);
    restart.release();
    CHECK(restart.progress() == 0.0);
    wait(0.8);
    CHECK(restarts == 0);

    restart.press();
    REQUIRE(runUntil([&] { return restarts > 0; }, 5.0));
    wait(1.0);
    restart.release();
    wait(0.3);
    CHECK(restarts == 1);

    machines.safeModeButton().press();
    wait(0.5);
    machines.safeModeButton().release();
    wait(1.5);
    CHECK(safeModes == 0);

    machines.testCardButton().triggerClick();
    REQUIRE(runUntil([&] { return harness.document().document().testCard; }));
    REQUIRE(runUntil([&] { return machines.testCardButton().getToggleState(); }));

    machines.lockButton().triggerClick();
    REQUIRE(runUntil([&] { return machines.unlockButton().isVisible(); }));
    CHECK_FALSE(machines.lockButton().isVisible());
    CHECK_FALSE(restart.isEnabled());
    CHECK_FALSE(machines.safeModeButton().isEnabled());
    restart.press();
    CHECK(restart.progress() == 0.0);
    writePng(main, "machines-actions.png");

    machines.unlockButton().press();
    wait(0.5);
    machines.unlockButton().release();
    wait(0.5);
    CHECK(harness.machine().actions().locked());
    machines.unlockButton().press();
    REQUIRE(runUntil([&] { return restart.isEnabled(); }, 5.0));
    machines.unlockButton().release();
    CHECK_FALSE(harness.machine().actions().locked());
    CHECK(restarts == 1);
}

TEST_CASE("Update is held to send the bundle beside the UI, shows its progress and the installer's result, and is "
          "greyed while the Show lock or a Gig window refuses it")
{
    TempDir root, laptop;
    const fs::path app = laptop.path / "Nocturne.app";
    fs::create_directories(app / "Contents/MacOS");
    std::ofstream(app / "Contents/MacOS/nocturne") << "#!/bin/bash\n# nocturne-build 20261018120000-abc123\n";
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, root.path, fourScreens());
    std::atomic<int> installs{0};
    harness.onInstall([&](const fs::path&) -> std::optional<std::string> {
        ++installs;
        return std::nullopt;
    });
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    main.transport().tabButton(Tab::Machines).triggerClick();
    azl::ui::MachinesView& machines = main.machines();
    azl::ui::HoldButton& update = machines.updateButton();
    machines.setUpdateBundle(laptop.path / "Missing.app");
    REQUIRE(runUntil([&] { return update.isVisible(); }));
    CHECK_FALSE(update.isEnabled());
    CHECK(update.getTooltip().contains("No engine bundle"));
    machines.setUpdateBundle(app);
    REQUIRE(runUntil([&] { return update.isEnabled(); }));
    const auto wait = [](double seconds) { runUntil([] { return false; }, seconds); };

    machines.lockButton().triggerClick();
    REQUIRE(runUntil([&] { return !update.isEnabled(); }));
    CHECK(update.getTooltip().contains("Show lock is on"));
    machines.unlockButton().press();
    REQUIRE(runUntil([&] { return update.isEnabled(); }, 5.0));
    machines.unlockButton().release();

    update.press();
    wait(0.5);
    update.release();
    wait(0.5);
    CHECK(installs == 0);
    CHECK(machines.updateLine().isEmpty());

    update.press();
    REQUIRE(runUntil([&] { return installs == 1; }, 10.0));
    update.release();
    REQUIRE(runUntil([&] { return machines.updateLine().contains("installing build 20261018120000-abc123"); }));
    const fs::path tmp = root.path / "update.json.tmp";
    std::ofstream(tmp) << R"({"result": "rolled back", "build": "20261018120000-abc123", "previous": "x", )"
                       << R"("reason": "the engine renders no frames", "at": 1792300000})";
    fs::rename(tmp, root.path / "update.json");
    REQUIRE(runUntil([&] { return machines.updateLine() == "Update rolled back: the engine renders no frames"; }));
    writePng(main, "machines-update.png");

    const azl::UtcOffset utc = [](int64_t) { return 0; };
    harness.setWallClock(double(azl::unixAt(*azl::dateDays("2026-10-16"), 21 * 60, utc)), utc);
    Client director(harness.document());
    apply(director, azl::SetGigWindow{"2026-10-16", azl::GigWindow{"2026-10-16", 20 * 60, 3 * 60}});
    REQUIRE(runUntil([&] { return !update.isEnabled(); }));
    CHECK(update.getTooltip().contains("Gig window 2026-10-16 20:00-03:00"));
    update.press();
    wait(2.0);
    update.release();
    CHECK(installs == 1);
}

TEST_CASE("the Machines tab shows the venue's media readiness by deck and the alert, and sets the sync override")
{
    azl::Composition composition = fourScreens();
    composition.decks = {{1, "Opening", {{1, "A"}}}, {2, "Headliner", {{2, "B"}}}};
    composition.layers[0].clips = {{1, {.hash = "aa", .name = "a.mp4"}}, {2, {.hash = "bb", .name = "b.mp4"}}};
    composition.gigs = {{"2026-10-16", 22 * 60, 5 * 60}};
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, composition);
    const auto utc = [](int64_t) { return 0; };
    const double start = double(azl::unixAt(*azl::dateDays("2026-10-16"), 22 * 60, utc));
    azl::MediaSync sync(harness.document(), harness.library(), [&] { return harness.engine().composition(); },
                        [start] { return start - 3600.0; },
                        {.folder = harness.library().root() / "Downloads", .clientError = "no R2 settings", .zone = utc});
    harness.onMediaStatus([&] { return sync.status(); });
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    main.transport().tabButton(Tab::Machines).triggerClick();
    azl::ui::MachinesView& machines = main.machines();

    juce::StringArray lines;
    const bool shown = runUntil([&] {
        lines = machines.mediaLines();
        return lines.size() == 3 && lines[0].startsWith("2 missing");
    });
    INFO(lines.joinIntoString("\n"));
    REQUIRE(shown);
    CHECK(lines[0] == juce::String::fromUTF8("2 missing \xc2\xb7 Opening 1, Headliner 1"));
    CHECK(lines[1] == "no R2 settings");
    CHECK(lines[2] == "media not ready, gig at 22:00");
    CHECK(machines.syncButton("auto").getToggleState());
    writePng(main, "machines-media.png");

    machines.syncButton("block").triggerClick();
    REQUIRE(runUntil([&] { return sync.mode() == azl::SyncOverride::Block; }));
    REQUIRE(runUntil([&] { return machines.syncButton("block").getToggleState(); }));
    CHECK_FALSE(machines.syncButton("auto").getToggleState());
    harness.onMediaStatus(nullptr);
}

TEST_CASE("the Machines tab shows one DJ link row per player: on air, BPM and pitch, play state and beat in bar")
{
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, fourScreens());
    std::atomic<bool> enabled{true};
    harness.onDjlinkStatus([&enabled] {
        if (!enabled)
            return azl::Message{{"enabled", false}, {"following", false}, {"onAir", 0}, {"players", azl::Message::array()}};
        const azl::Message unknown;
        return azl::Message{
            {"enabled", true},
            {"following", true},
            {"onAir", 2},
            {"bpm", 128.0},
            {"players",
             {{{"number", 2}, {"onAir", true}, {"bpm", 128.0}, {"pitch", 0.0}, {"play", "playing"}, {"beatInBar", 3}},
              {{"number", 3}, {"onAir", false}, {"bpm", 127.5}, {"pitch", 2.0}, {"play", "playing"}, {"beatInBar", 1}},
              {{"number", 4}, {"onAir", false}, {"bpm", unknown}, {"pitch", unknown}, {"play", "cued"},
               {"beatInBar", unknown}}}}};
    });
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    main.transport().tabButton(Tab::Machines).triggerClick();
    azl::ui::MachinesView& machines = main.machines();

    juce::StringArray lines;
    const bool shown = runUntil([&] {
        lines = machines.djlinkLines();
        return lines.size() == 3;
    });
    INFO(lines.joinIntoString("\n"));
    REQUIRE(shown);
    CHECK(lines[0] == juce::String::fromUTF8("Player 2 \xc2\xb7 on air \xc2\xb7 128.00 BPM +0.00% \xc2\xb7 playing \xc2\xb7 3/4"));
    CHECK(lines[1] == juce::String::fromUTF8("Player 3 \xc2\xb7 off air \xc2\xb7 127.50 BPM +2.00% \xc2\xb7 playing \xc2\xb7 1/4"));
    CHECK(lines[2] == juce::String::fromUTF8("Player 4 \xc2\xb7 off air \xc2\xb7 - \xc2\xb7 cued"));
    writePng(main, "machines-djlink.png");

    enabled = false;
    REQUIRE(runUntil([&] { return machines.djlinkLines().isEmpty(); }));
    harness.onDjlinkStatus(nullptr);
}

TEST_CASE("the Machines tab shows every output's return video beside its state, only while it is visible")
{
    azl::Composition composition = azl::defaultComposition();
    composition.screens.push_back({.id = 1, .name = "Projector", .display = "proj-a", .slices = {{.id = 1}}});
    composition.screens.push_back({.id = 2, .name = "TV", .display = "tv-b", .testCard = true, .slices = {{.id = 2}}});
    azl::EngineHarness harness({.width = 640, .height = 360}, 60.0, {}, composition);
    for (const char* id : {"proj-a", "tv-b"})
        harness.displays().attach({id, std::string("Epson ") + id, 640, 360, 60.0});
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    harness.step(2);
    CHECK(harness.returnVideo().watchers() == 0);

    main.transport().tabButton(Tab::Machines).triggerClick();
    REQUIRE(runUntil([&] { return harness.returnVideo().watchers() == 1; }));
    std::vector<juce::Image> tiles;
    REQUIRE(runUntil([&] {
        tiles = main.machines().tiles();
        return tiles.size() == 2 && tiles[0].isValid() && tiles[1].isValid();
    }));
    CHECK(tiles[0].getWidth() == 426);
    CHECK(tiles[0].getHeight() == 240);
    const auto mean = [](const juce::Image& image) {
        double sum = 0.0;
        int count = 0;
        for (int y = 0; y < image.getHeight(); y += 4)
            for (int x = 0; x < image.getWidth(); x += 4, ++count)
                sum += image.getPixelAt(x, y).getBrightness();
        return sum / count;
    };
    MESSAGE("tile brightness: " << mean(tiles[0]) << " and " << mean(tiles[1]));
    CHECK(std::abs(mean(tiles[0]) - mean(tiles[1])) > 0.02);
    writePng(main, "machines-return-video.png");

    main.setTab(Tab::Perform);
    REQUIRE(runUntil([&] { return harness.returnVideo().watchers() == 0; }));
    CHECK_FALSE(session.client().watchingVideo());
    runUntil([] { return false; }, 0.3);
    const uint64_t bytes = session.client().videoBytes();
    runUntil([] { return false; }, 0.7);
    CHECK(session.client().videoBytes() == bytes);
}
