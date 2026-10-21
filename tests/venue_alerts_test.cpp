#include "audio/audio_analysis.hpp"
#include "net/websocket.hpp"
#include "protocol/build_info.hpp"
#include "techno_loop.hpp"
#include "test_client.hpp"
#include "venues/venue_monitor.hpp"

#include <atomic>
#include <thread>

namespace {

using azl::AlertKind;
using azl::OutputHealth;
using azl::Venue;
using azl::VenueAlert;
using azl::VenueDot;
using azl::VenueHealth;
using azl::VenueProbe;

VenueProbe healthy(VenueHealth health)
{
    return {.reachable = true, .answered = true, .build = "b", .health = std::move(health)};
}

std::vector<AlertKind> kinds(const std::vector<VenueAlert>& alerts)
{
    std::vector<AlertKind> kinds;
    for (const VenueAlert& alert : alerts)
        kinds.push_back(alert.kind);
    return kinds;
}

bool has(const std::vector<azl::StripAlert>& strip, const std::string& venue, AlertKind kind)
{
    return std::any_of(strip.begin(), strip.end(), [&](const azl::StripAlert& a) {
        return a.venue.name == venue && a.alert.kind == kind;
    });
}

bool hasVenue(const std::vector<azl::StripAlert>& strip, const std::string& venue)
{
    return std::any_of(strip.begin(), strip.end(), [&](const azl::StripAlert& a) { return a.venue.name == venue; });
}

} // namespace

TEST_CASE("the dot rules over health snapshots: each alert with its dot and words, the dot the worst of them")
{
    const Venue online{"club", "club.tail1234.ts.net", 7400, "tailscale", true};
    Venue dark = online;
    dark.online = false;
    const VenueProbe unreachable{.error = "cannot connect"};
    const VenueProbe silent{.reachable = true, .error = "no answer from the engine"};
    const VenueProbe refused{.reachable = true, .error = "no answer from the engine", .engineError = "send hello first"};
    const VenueProbe older{.reachable = true, .answered = true, .build = "b"};

    struct Row {
        const char* name;
        Venue venue;
        VenueProbe probe;
        azl::AlertContext context;
        std::vector<VenueAlert> alerts;
        VenueDot dot;
    };
    const std::vector<Row> rows = {
        {"all fine", online, healthy({.fps = 60.0, .outputs = {{"Screen 1", "ok"}}, .audioSignal = true}), {}, {},
         VenueDot::Ok},
        {"an engine older than the health status", online, older, {}, {}, VenueDot::Ok},
        {"no engine answers", online, unreachable, {}, {{AlertKind::Offline, VenueDot::Offline, "offline"}},
         VenueDot::Offline},
        {"the engine stays silent", online, silent, {}, {{AlertKind::Offline, VenueDot::Offline, "offline"}},
         VenueDot::Offline},
        {"Tailscale says offline", dark, healthy({}), {}, {{AlertKind::Offline, VenueDot::Offline, "offline"}},
         VenueDot::Offline},
        {"offline inside a Gig window", online, unreachable, {.inGigWindow = true},
         {{AlertKind::Offline, VenueDot::Fault, "offline"}}, VenueDot::Fault},
        {"the engine refuses", online, refused, {},
         {{AlertKind::EngineError, VenueDot::Fault, "send hello first"}}, VenueDot::Fault},
        {"fps just at the low line", online, healthy({.fps = azl::kLowFps}), {}, {}, VenueDot::Ok},
        {"low fps", online, healthy({.fps = 42.4}), {}, {{AlertKind::LowFps, VenueDot::Warning, "42 fps"}},
         VenueDot::Warning},
        {"fps below the floor", online, healthy({.fps = 24.0}), {}, {{AlertKind::LowFps, VenueDot::Fault, "24 fps"}},
         VenueDot::Fault},
        {"frame guard at level 1", online, healthy({.guardLevel = 1}), {}, {}, VenueDot::Ok},
        {"frame guard above level 1", online, healthy({.guardLevel = 2}), {},
         {{AlertKind::FrameGuard, VenueDot::Warning, "frame guard 2"}}, VenueDot::Warning},
        {"no audio signal for a moment", online, healthy({.audioSignal = false, .silentSeconds = 5.0}), {}, {},
         VenueDot::Ok},
        {"no audio signal for long", online, healthy({.audioSignal = false, .silentSeconds = 12.2}), {},
         {{AlertKind::NoAudio, VenueDot::Fault, "no audio 12 s"}}, VenueDot::Fault},
        {"no audio analysis at all", online, healthy({.silentSeconds = 60.0}), {}, {}, VenueDot::Ok},
        {"media missing", online, healthy({.missingMedia = 3}), {},
         {{AlertKind::MissingMedia, VenueDot::Warning, "3 media missing"}}, VenueDot::Warning},
        {"all media there", online, healthy({.missingMedia = 0}), {}, {}, VenueDot::Ok},
        {"outputs missing, mirrored and slow", online,
         healthy({.outputs = {{"Screen 1", "ok"}, {"Screen 2", "missing"}, {"Screen 3", "mirrored"},
                              {"Screen 4", "low rate"}}}),
         {},
         {{AlertKind::OutputMissing, VenueDot::Fault, "Screen 2 missing"},
          {AlertKind::OutputMirrored, VenueDot::Warning, "Screen 3 mirrored"},
          {AlertKind::OutputLowRate, VenueDot::Warning, "Screen 4 low rate"}},
         VenueDot::Fault},
        {"another build than the UI's", online, healthy({}), {.uiBuild = "ui"},
         {{AlertKind::VersionMismatch, VenueDot::Warning, "venue build b, UI build ui"}}, VenueDot::Warning},
        {"the same build as the UI's", online, healthy({}), {.uiBuild = "b"}, {}, VenueDot::Ok},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        const std::vector<VenueAlert> alerts = azl::venueAlerts(row.venue, row.probe, row.context);
        CHECK(alerts == row.alerts);
        CHECK(azl::venueDot(alerts) == row.dot);
        CHECK(azl::venueDot(row.venue, row.probe, row.context) == row.dot);
    }
}

TEST_CASE("the strip lists every venue's alerts but the one in view's, and no build mismatch")
{
    const auto entry = [](const char* name, const char* host, std::vector<VenueAlert> alerts) {
        azl::VenueEntry e;
        e.venue = {name, host, 7400, "tailscale", true};
        e.alerts = std::move(alerts);
        return e;
    };
    const std::vector<azl::VenueEntry> entries = {
        entry("club-a", "club-a.ts.net", {{AlertKind::OutputMissing, VenueDot::Fault, "Screen 2 missing"}}),
        entry("club-b", "club-b.ts.net",
              {{AlertKind::VersionMismatch, VenueDot::Warning, "venue build x, UI build y"},
               {AlertKind::NoAudio, VenueDot::Fault, "no audio 12 s"}}),
        entry("club-c", "Club-C.ts.net", {{AlertKind::Offline, VenueDot::Offline, "offline"}}),
    };
    const std::vector<azl::StripAlert> strip = azl::otherVenueAlerts(entries, "club-c.ts.net", 7400);
    REQUIRE(strip.size() == 2);
    CHECK(strip[0].venue.name == "club-a");
    CHECK(strip[0].alert.text == "Screen 2 missing");
    CHECK(strip[1].venue.name == "club-b");
    CHECK(strip[1].alert.kind == AlertKind::NoAudio);
    CHECK(azl::otherVenueAlerts(entries, "club-c.ts.net", 7401).size() == 3);
    CHECK(azl::otherVenueAlerts({}, "club-c.ts.net", 7400).empty());
}

TEST_CASE("a Director polling three venue engines: a starved output and silent audio show in the strip, a stopped "
          "engine shows offline, the venue in view stays out")
{
    constexpr azl::EngineConfig size{.width = 320, .height = 180};
    azl::Composition starvedShow = azl::defaultComposition();
    starvedShow.screens.push_back(
        {.id = 1, .name = "Screen 1", .display = "proj-a", .slices = {{.id = 1, .name = "Slice 1"}}});
    azl::EngineHarness starved(size, 60.0, {}, starvedShow);
    starved.displays().attach({"proj-a", "Epson proj-a", size.width, size.height, 60.0});
    starved.outputs().setDelay("proj-a", 60);
    azl::EngineHarness quiet(size);
    azl::EngineHarness fine(size);

    double audioNow = 10.0;
    azl::AudioAnalysis analysis([&] { return audioNow; }, {.thread = false});
    const std::vector<float> samples = TechnoLoop{.bars = 1}.render();
    analysis.push(samples.data(), 4096, 9.9, TechnoLoop::kRate);
    analysis.pump();
    quiet.setAudio(&analysis);

    auto starvedServer = std::make_unique<azl::WebSocketServer>(starved.document(), 0);
    azl::WebSocketServer quietServer(quiet.document(), 0);
    azl::WebSocketServer fineServer(fine.document(), 0);
    const azl::TailscaleList list{{{"starved", "127.0.0.1", starvedServer->port(), "tailscale", true},
                                   {"quiet", "127.0.0.1", quietServer.port(), "tailscale", true},
                                   {"fine", "127.0.0.1", fineServer.port(), "tailscale", true}},
                                  ""};
    const uint64_t quietChange = quiet.document().change();

    double now = 100.0;
    azl::VenueSources sources;
    sources.probe = [](const Venue& venue) {
        return azl::probeVenue(venue.host, venue.port, std::chrono::milliseconds(2000));
    };
    sources.now = [&] { return now; };
    azl::SilentSince silent;
    const auto strip = [&] {
        return azl::otherVenueAlerts(azl::scanVenues(sources, list, {}, &silent).entries, "127.0.0.1",
                                     fineServer.port());
    };

    starved.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::vector<azl::StripAlert> shown = strip();
    CHECK(has(shown, "starved", AlertKind::OutputLowRate));
    CHECK_FALSE(hasVenue(shown, "quiet"));

    analysis.pump(14.0);
    REQUIRE_FALSE(analysis.latest().live);
    CHECK_FALSE(hasVenue(strip(), "quiet"));
    now += azl::kNoAudioSeconds;
    shown = strip();
    CHECK(has(shown, "starved", AlertKind::OutputLowRate));
    CHECK(has(shown, "quiet", AlertKind::NoAudio));
    CHECK_FALSE(hasVenue(shown, "fine"));

    fine.setAudio(&analysis);
    strip();
    now += azl::kNoAudioSeconds;
    starved.stopLive();
    starvedServer.reset();
    const azl::VenueScan scan = azl::scanVenues(sources, list, {}, &silent);
    REQUIRE(scan.entries.size() == 3);
    CHECK(kinds(scan.entries[2].alerts) == std::vector<AlertKind>{AlertKind::NoAudio});
    shown = azl::otherVenueAlerts(scan.entries, "127.0.0.1", fineServer.port());
    CHECK_FALSE(hasVenue(shown, "fine"));
    CHECK(has(shown, "starved", AlertKind::Offline));
    CHECK_FALSE(has(shown, "starved", AlertKind::OutputLowRate));
    CHECK(has(shown, "quiet", AlertKind::NoAudio));
    CHECK(scan.entries[0].dot == VenueDot::Offline);

    shown = azl::otherVenueAlerts(scan.entries, "127.0.0.1", quietServer.port());
    CHECK(has(shown, "fine", AlertKind::NoAudio));
    CHECK_FALSE(hasVenue(shown, "quiet"));
    CHECK(quiet.document().change() == quietChange);
    fine.setAudio(nullptr);
    quiet.setAudio(nullptr);
}

TEST_CASE("a health probe gets no document: the engine answers its hello with no snapshot and sends it no changes")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    std::vector<std::string> types;
    const azl::ClientId probe = harness.document().connect([&](const Message& m) {
        types.push_back(m.value("type", ""));
    });
    Message hello = msg::hello();
    hello["watch"] = false;
    harness.document().receive(probe, hello);
    Client editor(harness.document());
    apply(editor, azl::SetTempo{133.0});
    CHECK(types.empty());
    harness.document().disconnect(probe);
}

TEST_CASE("a probe reads the venue's media readiness: missing media is a warning, ready media none")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    const Venue venue{"club", "127.0.0.1", server.port(), "manual", true};
    std::atomic<int> missing{2};
    harness.onMediaStatus([&] { return Message{{"ready", missing == 0}, {"missing", missing.load()}}; });

    VenueProbe probe = azl::probeVenue(venue.host, venue.port);
    REQUIRE(probe.health);
    CHECK(probe.health->missingMedia == 2);
    CHECK(azl::venueAlerts(venue, probe) ==
          std::vector<VenueAlert>{{AlertKind::MissingMedia, VenueDot::Warning, "2 media missing"}});

    missing = 0;
    probe = azl::probeVenue(venue.host, venue.port);
    REQUIRE(probe.health);
    CHECK(probe.health->missingMedia == 0);
    CHECK(azl::venueDot(venue, probe) == VenueDot::Ok);

    harness.onMediaStatus({});
    probe = azl::probeVenue(venue.host, venue.port);
    REQUIRE(probe.health);
    CHECK_FALSE(probe.health->missingMedia);
}

TEST_CASE("ctl's venue lines: the dot, then each alert's text; a stopped engine reads offline")
{
    constexpr azl::EngineConfig size{.width = 320, .height = 180};
    azl::Composition starvedShow = azl::defaultComposition();
    starvedShow.screens.push_back(
        {.id = 1, .name = "Screen 1", .display = "proj-a", .slices = {{.id = 1, .name = "Slice 1"}}});
    azl::EngineHarness starved(size, 60.0, {}, starvedShow);
    starved.displays().attach({"proj-a", "Epson proj-a", size.width, size.height, 60.0});
    starved.outputs().setDelay("proj-a", 60);
    azl::EngineHarness gone(size);
    auto goneServer = std::make_unique<azl::WebSocketServer>(gone.document(), 0);
    azl::WebSocketServer starvedServer(starved.document(), 0);
    const azl::TailscaleList list{{{"gone", "127.0.0.1", goneServer->port(), "tailscale", true},
                                   {"starved", "127.0.0.1", starvedServer.port(), "tailscale", true}},
                                  ""};
    goneServer.reset();

    azl::VenueSources sources;
    sources.probe = [](const Venue& venue) {
        return azl::probeVenue(venue.host, venue.port, std::chrono::milliseconds(2000));
    };
    starved.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const azl::VenueScan scan = azl::scanVenues(sources, list, {});
    starved.stopLive();

    REQUIRE(scan.entries.size() == 2);
    const std::vector<std::string> goneLines = azl::venueLines(scan.entries[0]);
    REQUIRE(goneLines.size() == 2);
    CHECK(goneLines[0].find("\toffline\t") != std::string::npos);
    CHECK(goneLines[1] == "  offline: offline");

    const std::vector<std::string> starvedLines = azl::venueLines(scan.entries[1]);
    REQUIRE(starvedLines.size() == 2);
    CHECK(starvedLines[0].find("\twarning\t") != std::string::npos);
    CHECK(starvedLines[1] == "  warning: Screen 1 low rate");
}
