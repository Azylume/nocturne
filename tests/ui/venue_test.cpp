#include "net/websocket.hpp"
#include "protocol/build_info.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <atomic>
#include <functional>

namespace {

using azl::ui::Session;

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

// An engine of its own on a random port, with a tempo to tell it by and a column playing.
struct Venue {
    explicit Venue(double bpm)
    {
        Client client(harness.document());
        apply(client, azl::SetTempo{bpm});
        apply(client, azl::SetQuantise{0});
        column = harness.document().document().decks[0].columns[1].id;
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
        client.next();
        harness.step();
    }

    azl::EngineHarness harness{{.width = 640, .height = 360}};
    azl::WebSocketServer server{harness.document(), 0};
    uint64_t column = 0;
};

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

bool plays(const Session& session, uint64_t column)
{
    const auto& layers = session.composition().layers;
    return !layers.empty() && layers[0].playing && layers[0].playing->column == column;
}

// Four venues as the menu would list them: two engines here, one Tailscale says is offline, one that refuses.
azl::VenueSources fakeVenues(const Venue& clubA, const Venue& clubB)
{
    azl::VenueSources sources;
    sources.tailscale = [m = clubA.server.port(), a = clubB.server.port()] {
        return azl::TailscaleList{{{"club-a", "127.0.0.1", m, "tailscale", true},
                                   {"club-b", "127.0.0.1", a, "tailscale", true},
                                   {"club-c", "club-c.tail1234.ts.net", 7400, "tailscale", false}},
                                  ""};
    };
    sources.bonjour = [] { return std::vector<azl::Venue>{{"studio", "studio.local", 7400, "bonjour", true}}; };
    sources.probe = [](const azl::Venue& venue) {
        if (venue.name == "studio")
            return azl::VenueProbe{.reachable = true, .error = "no answer from the engine",
                                   .engineError = "send hello first"};
        azl::VenueProbe probe = azl::probeVenue(venue.host, venue.port);
        if (venue.name == "club-b")
            probe.build = "20261016030000-0ff1ce0";
        return probe;
    };
    return sources;
}

} // namespace

TEST_CASE("switching venues: the UI drops the old venue at once, shows the new one's live state, and the old plays on")
{
    Venue clubA(128.0), clubB(140.0);
    Session session("127.0.0.1", clubA.server.port());
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 128.0; }));
    CHECK(plays(session, clubA.column));
    const uint64_t clubAChange = clubA.harness.document().change();

    const auto picked = std::chrono::steady_clock::now();
    session.connectTo("127.0.0.1", clubB.server.port());
    CHECK(session.port() == clubB.server.port());
    CHECK(session.composition().bpm != 128.0);
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 140.0; }));
    CHECK(std::chrono::steady_clock::now() - picked < std::chrono::seconds(3));
    CHECK(plays(session, clubB.column));

    clubA.harness.step(10);
    CHECK(clubA.harness.document().change() == clubAChange);
    CHECK(clubA.harness.document().document().bpm == 128.0);

    session.connectTo("127.0.0.1", clubA.server.port());
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 128.0; }));
    CHECK(plays(session, clubA.column));
}

TEST_CASE("the venue menu lists venues with dots, warns of another build, and a pick switches every tab")
{
    Venue clubA(128.0), clubB(140.0);
    Session session("127.0.0.1", clubA.server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    session.startVenues(fakeVenues(clubA, clubB));
    REQUIRE(runUntil([&] { return session.client().synced() && session.venues()->scans() > 0; }));

    azl::ui::TransportBar& bar = main.transport();
    CHECK(bar.venueButton().label() == "club-a");
    CHECK(bar.venueButton().dot() == azl::VenueDot::Ok);
    bar.openVenues();
    azl::ui::VenueMenu* menu = bar.venueMenu();
    REQUIRE(menu);
    REQUIRE(runUntil([&] { return menu->rows().size() == 4; }));
    std::vector<std::string> dots;
    for (const azl::VenueEntry& entry : menu->rows())
        dots.push_back(entry.venue.name + ":" + azl::dotName(entry.dot));
    CHECK(dots == std::vector<std::string>{"club-a:ok", "club-b:warning","club-c:offline", "studio:fault"});
    CHECK(menu->rows()[1].versionWarning ==
          "venue build 20261016030000-0ff1ce0, UI build " + std::string(azl::buildId()));
    CHECK(menu->note().isEmpty());
    writePng(main, "venue-menu.png");

    menu->pick(1);
    CHECK_FALSE(menu->isVisible());
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 140.0; }));
    CHECK(session.port() == clubB.server.port());
    REQUIRE(runUntil([&] { return bar.venueButton().label() == "club-b"; }));
    main.setMapMode(true);
    CHECK(main.map().isVisible());
    CHECK(plays(session, clubB.column));
}

TEST_CASE("the top bar warns when the venue runs another build, with both build ids")
{
    Venue clubA(128.0);
    Session session("127.0.0.1", clubA.server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    CHECK(main.transport().statusText().first.startsWith("local engine"));

    Message older = clubA.harness.document().document();
    session.client().receive({{"type", "snapshot"},
                              {"change", clubA.harness.document().change()},
                              {"composition", older},
                              {"build", "19700101000000-older"}});
    REQUIRE(runUntil([&] { return !session.client().engineBuild().empty(); }));
    runUntil([] { return false; }, 0.1);
    CHECK(main.transport().statusText().first ==
          juce::String("venue build 19700101000000-older, UI build ") + azl::buildId());
}

TEST_CASE("without a venue list the menu says so, and a typed host connects and stays in the list")
{
    CHECK(azl::ui::hostAndPort("venue-mini") == std::pair<std::string, uint16_t>{"venue-mini", 7400});
    CHECK(azl::ui::hostAndPort("venue-mini:7410") == std::pair<std::string, uint16_t>{"venue-mini", 7410});
    CHECK(azl::ui::hostAndPort("fd7a:115c::1") == std::pair<std::string, uint16_t>{"fd7a:115c::1", 7400});
    CHECK(azl::ui::hostAndPort("mini:http") == std::pair<std::string, uint16_t>{"mini:http", 7400});

    Venue clubA(128.0);
    uint16_t closed = 0;
    {
        azl::WebSocketServer gone(clubA.harness.document(), 0);
        closed = gone.port();
    }
    Session session("127.0.0.1", closed);
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    main.transport().openVenues();
    azl::ui::VenueMenu* menu = main.transport().venueMenu();
    REQUIRE(menu);
    CHECK(menu->note() == "No venue list in this run: type a host");
    CHECK(main.transport().venueButton().dot() == azl::VenueDot::Offline);

    azl::VenueSources none;
    none.probe = [](const azl::Venue& venue) { return azl::probeVenue(venue.host, venue.port); };
    session.startVenues(none);
    menu->hostField().setText("localhost:" + juce::String(clubA.server.port()));
    menu->connectButton().triggerClick();
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 128.0; }));
    CHECK(session.host() == "localhost");
    CHECK(session.port() == clubA.server.port());
    REQUIRE(runUntil([&] {
        const auto entries = session.venues()->entries();
        return entries.size() == 1 && entries[0].venue.source == "manual" && entries[0].dot == azl::VenueDot::Ok;
    }));
    CHECK(main.transport().venueButton().label() == "localhost");
}

TEST_CASE("an engine that wants a pairing token: the token typed beside the host connects, is kept for that host and "
          "is presented again on the next start")
{
    const std::string token(64, 'c');
    Venue club(128.0);
    azl::WebSocketServer paired(club.harness.document(), 0, {token, false});
    TempDir dir;
    juce::PropertiesFile settings(juce::File(juce::String((dir.path / "UI.settings").string())),
                                  juce::PropertiesFile::Options());
    {
        Session session("127.0.0.1", paired.port());
        azl::ui::MainComponent main(session);
        main.setSize(1600, 950);
        session.useSettings(&settings);
        CHECK_FALSE(runUntil([&] { return session.client().synced(); }, 1.5));

        main.transport().openVenues();
        azl::ui::VenueMenu* menu = main.transport().venueMenu();
        REQUIRE(menu);
        menu->hostField().setText("127.0.0.1:" + juce::String(paired.port()));
        menu->tokenField().setText(juce::String(token));
        menu->connectButton().triggerClick();
        REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 128.0; }));
        CHECK(session.tokenFor("127.0.0.1") == token);
        CHECK(session.tokenFor("other-venue").empty());
        CHECK(settings.getValue("token.127.0.0.1").toStdString() == token);
        session.useSettings(nullptr);
    }
    Session again("127.0.0.1", paired.port());
    again.useSettings(&settings);
    REQUIRE(runUntil([&] { return again.client().synced() && again.composition().bpm == 128.0; }));
    again.useSettings(nullptr);
}

TEST_CASE("the alert strip shows the other venues' alerts under the top bar, takes no space without any, and a click "
          "switches venue")
{
    Venue clubA(128.0), clubB(140.0);
    Session session("127.0.0.1", clubA.server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    std::atomic<bool> trouble{false};
    azl::VenueSources sources;
    sources.tailscale = [m = clubA.server.port(), a = clubB.server.port()] {
        return azl::TailscaleList{{{"club-a", "127.0.0.1", m, "tailscale", true},
                                   {"club-b", "127.0.0.1", a, "tailscale", true}},
                                  ""};
    };
    sources.probe = [&](const azl::Venue& venue) {
        azl::VenueProbe probe = azl::probeVenue(venue.host, venue.port);
        if (trouble && probe.health)
            probe.health->outputs = {{"Screen 2", "missing"}};
        return probe;
    };
    session.startVenues(sources);
    REQUIRE(runUntil([&] { return session.client().synced() && session.venues()->scans() > 0; }));
    const int top = azl::ui::Look::metrics().transportHeight + 1;
    CHECK_FALSE(main.alerts().isVisible());
    CHECK(main.alerts().getHeight() == 0);
    CHECK(main.grid().getY() == top);

    trouble = true;
    session.venues()->refresh();
    REQUIRE(runUntil([&] { return main.alerts().isVisible(); }));
    CHECK(main.alerts().lines() == juce::StringArray{"club-b: Screen 2 missing"});
    CHECK(main.alerts().getY() == top - 1);
    CHECK(main.alerts().getHeight() == azl::ui::AlertStrip::kHeight);
    CHECK(main.grid().getY() == top + azl::ui::AlertStrip::kHeight);
    writePng(main, "alert-strip.png");

    main.alerts().pick(0);
    REQUIRE(runUntil([&] { return session.client().synced() && session.composition().bpm == 140.0; }));
    CHECK(session.port() == clubB.server.port());
    REQUIRE(runUntil([&] { return main.alerts().lines() == juce::StringArray{"club-a: Screen 2 missing"}; }));

    trouble = false;
    session.venues()->refresh();
    REQUIRE(runUntil([&] { return !main.alerts().isVisible(); }));
    CHECK(main.grid().getY() == top);
}
