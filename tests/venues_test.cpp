#include "net/websocket.hpp"
#include "protocol/build_info.hpp"
#include "test_client.hpp"
#include "venues/venue_monitor.hpp"

#include <atomic>
#include <fstream>
#include <thread>

namespace {

using azl::Venue;
using azl::VenueDot;
using azl::VenueProbe;

// A Tailscale API v2 device list as the API answers it (trimmed), with a tagged online and a tagged offline venue,
// an untagged laptop and a device with another tag.
const char* const kDevices = R"({"devices": [
  {"addresses": ["100.101.102.103", "fd7a:115c:a1e0::1"], "id": "1001", "nodeId": "nA1", "user": "operator@",
   "name": "venue-mini.tail1234.ts.net", "hostname": "venue-mini", "clientVersion": "1.76.1", "os": "macOS",
   "lastSeen": "2026-10-17T20:00:00Z", "tags": ["tag:nocturne-venue"], "connectedToControl": true},
  {"addresses": ["100.90.1.2"], "id": "1002", "name": "club-a-mini.tail1234.ts.net.", "hostname": "Mac-mini",
   "os": "macOS", "lastSeen": "2026-10-16T03:10:00Z", "tags": ["tag:nocturne-venue", "tag:server"],
   "connectedToControl": false},
  {"addresses": ["100.64.0.9"], "id": "1003", "name": "laptop.tail1234.ts.net", "hostname": "laptop",
   "os": "macOS", "user": "operator@", "connectedToControl": true},
  {"addresses": ["100.64.0.10"], "id": "1004", "name": "nas.tail1234.ts.net", "hostname": "nas", "os": "linux",
   "tags": ["tag:server"], "connectedToControl": true}
]})";

struct FakeApi {
    std::vector<azl::HttpRequest> requests;
    azl::HttpResponse response{200, {}, kDevices};

    azl::HttpTransport transport()
    {
        return [this](const azl::HttpRequest& request) {
            requests.push_back(request);
            return response;
        };
    }
};

azl::TailscaleSettings withKey()
{
    azl::TailscaleSettings settings;
    settings.apiKey = "tskey-api-test";
    return settings;
}

void writeFile(const fs::path& path, const std::string& text, fs::perms perms)
{
    std::ofstream(path) << text;
    fs::permissions(path, perms);
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

int maxDifference(const azl::Image& a, const azl::Image& b)
{
    REQUIRE(a.rgba.size() == b.rgba.size());
    int most = 0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        most = std::max(most, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    return most;
}

bool showsSomething(const azl::Image& image)
{
    return std::any_of(image.rgba.begin(), image.rgba.end(), [](uint8_t v) { return v > 40; });
}

} // namespace

TEST_CASE("the Tailscale device list gives the devices with the venue tag, by MagicDNS name, online or not")
{
    const std::vector<Venue> venues = azl::parseTailscaleDevices(kDevices, azl::kVenueTag, 7400);
    REQUIRE(venues.size() == 2);
    CHECK(venues[0] == Venue{"club-a-mini", "club-a-mini.tail1234.ts.net", 7400, "tailscale", false});
    CHECK(venues[1] == Venue{"venue-mini", "venue-mini.tail1234.ts.net", 7400, "tailscale", true});

    const std::vector<Venue> servers = azl::parseTailscaleDevices(kDevices, "tag:server", 7500);
    REQUIRE(servers.size() == 2);
    CHECK(servers[0].name == "club-a-mini");
    CHECK(servers[1].name == "nas");
    CHECK(servers[1].port == 7500);

    CHECK(azl::parseTailscaleDevices(R"({"devices": []})", azl::kVenueTag, 7400).empty());
    CHECK_THROWS(azl::parseTailscaleDevices(R"({"message": "nope"})", azl::kVenueTag, 7400));
    CHECK_THROWS(azl::parseTailscaleDevices("<html>", azl::kVenueTag, 7400));
}

TEST_CASE("the venue list is fetched with the key as a bearer token and says plainly what went wrong")
{
    FakeApi api;
    azl::TailscaleSettings settings = withKey();
    settings.tag = "tag:server";
    azl::TailscaleList list = azl::fetchTailscaleVenues(settings, api.transport());
    CHECK(list.error.empty());
    CHECK(list.venues.size() == 2);
    REQUIRE(api.requests.size() == 1);
    CHECK(api.requests[0].method == "GET");
    CHECK(api.requests[0].url == "https://api.tailscale.com/api/v2/tailnet/-/devices");
    CHECK(api.requests[0].headers.at("authorization") == "Bearer tskey-api-test");

    api.response = {401, {}, R"({"message": "API token invalid"})"};
    list = azl::fetchTailscaleVenues(withKey(), api.transport());
    CHECK(list.venues.empty());
    CHECK(contains(list.error, "Tailscale rejected the API key (HTTP 401)"));

    api.response = {500, {}, ""};
    CHECK(azl::fetchTailscaleVenues(withKey(), api.transport()).error == "the Tailscale API answered HTTP 500");

    api.response = {200, {}, "not json"};
    CHECK(contains(azl::fetchTailscaleVenues(withKey(), api.transport()).error, "cannot read Tailscale's device list"));

    list = azl::fetchTailscaleVenues(withKey(), [](const azl::HttpRequest&) -> azl::HttpResponse {
        throw azl::TransportError("The Internet connection appears to be offline.");
    });
    CHECK(list.error == "cannot reach the Tailscale API: The Internet connection appears to be offline.");

    const size_t asked = api.requests.size();
    azl::TailscaleSettings keyless;
    keyless.keyProblem = "no Tailscale API key: add it";
    CHECK(azl::fetchTailscaleVenues(keyless, api.transport()).error == "no Tailscale API key: add it");
    CHECK(api.requests.size() == asked);
}

TEST_CASE("the API key comes from a file only others cannot read, else the keychain or the environment")
{
    TempDir dir;
    const fs::path file = dir.path / "venues.json";
    const auto none = [] { return std::string(); };

    azl::TailscaleSettings settings = azl::loadTailscaleSettings(file, none);
    CHECK(settings.apiKey.empty());
    CHECK(settings.tag == "tag:nocturne-venue");
    CHECK(settings.tailnet == "-");
    CHECK(contains(settings.keyProblem, "no Tailscale API key"));
    CHECK(contains(settings.keyProblem, "security add-generic-password -s nocturne-tailscale -a api-key -w"));

    settings = azl::loadTailscaleSettings(file, [] { return std::string("tskey-from-keychain"); });
    CHECK(settings.apiKey == "tskey-from-keychain");
    CHECK(settings.keyProblem.empty());

    writeFile(file, R"({"tailnet": "example.com", "tag": "tag:club", "port": 7410})", fs::perms(0644));
    settings = azl::loadTailscaleSettings(file, [] { return std::string("tskey-from-keychain"); });
    CHECK(settings.tailnet == "example.com");
    CHECK(settings.tag == "tag:club");
    CHECK(settings.port == 7410);
    CHECK(settings.apiKey == "tskey-from-keychain");

    writeFile(file, R"({"api_key": "tskey-in-file"})", fs::perms(0644));
    settings = azl::loadTailscaleSettings(file, none);
    CHECK(settings.apiKey.empty());
    CHECK(contains(settings.keyProblem, "others can read it: chmod 600 it"));

    fs::permissions(file, fs::perms(0600));
    settings = azl::loadTailscaleSettings(file, none);
    CHECK(settings.apiKey == "tskey-in-file");
    CHECK(settings.keyProblem.empty());

    writeFile(file, "{tag:", fs::perms(0600));
    CHECK(contains(azl::loadTailscaleSettings(file, none).keyProblem, "cannot be read"));
}

TEST_CASE("Bonjour and typed venues join Tailscale's, one entry per engine; Node Sync followers are left out")
{
    const std::vector<Venue> tailscale = azl::parseTailscaleDevices(kDevices, azl::kVenueTag, 7400);
    const std::vector<Venue> bonjour = azl::bonjourVenues({
        {"venue-mini", "venue-mini.local", 7400, {{"role", "main"}}},
        {"laptop", "laptop.local", 7400, {{"role", "node"}}},
        {"studio", "studio.local", 7400, {}},
    });
    REQUIRE(bonjour.size() == 2);
    const std::vector<Venue> typed = {{"10.0.0.5", "10.0.0.5", 7400, "manual", true},
                                      {"studio.local", "studio.local", 7400, "manual", true},
                                      {"studio.local", "studio.local", 7401, "manual", true}};
    const std::vector<Venue> merged = azl::mergeVenues(tailscale, bonjour, typed);
    std::vector<std::string> names;
    for (const Venue& venue : merged)
        names.push_back(venue.name + "/" + venue.source + "/" + std::to_string(venue.port));
    CHECK(names == std::vector<std::string>{"club-a-mini/tailscale/7400", "venue-mini/tailscale/7400",
                                            "studio/bonjour/7400", "10.0.0.5/manual/7400",
                                            "studio.local/manual/7401"});
}

TEST_CASE("a venue's dot follows its probe: offline, fault, warning, ok")
{
    const Venue online{"club", "club.tail1234.ts.net", 7400, "tailscale", true};
    Venue offline = online;
    offline.online = false;

    VenueProbe unreachable;
    unreachable.error = "cannot connect to ws://club.tail1234.ts.net:7400";
    CHECK(azl::venueDot(online, unreachable) == VenueDot::Offline);

    VenueProbe silent{.reachable = true, .error = "no answer from the engine"};
    CHECK(azl::venueDot(online, silent) == VenueDot::Offline);

    VenueProbe refused{.reachable = true, .error = "no answer from the engine", .engineError = "send hello first"};
    CHECK(azl::venueDot(online, refused) == VenueDot::Fault);

    VenueProbe guarded{
        .reachable = true, .answered = true, .build = "b", .health = azl::VenueHealth{.guardLevel = 2}};
    CHECK(azl::venueDot(online, guarded) == VenueDot::Warning);

    VenueProbe fine{.reachable = true, .answered = true, .build = "b"};
    CHECK(azl::venueDot(online, fine) == VenueDot::Ok);
    CHECK(azl::venueDot(offline, fine) == VenueDot::Offline);

    CHECK(std::string(azl::dotName(VenueDot::Ok)) == "ok");
    CHECK(std::string(azl::dotName(VenueDot::Warning)) == "warning");
    CHECK(std::string(azl::dotName(VenueDot::Fault)) == "fault");
    CHECK(std::string(azl::dotName(VenueDot::Offline)) == "offline");
}

TEST_CASE("a venue on another build gets a warning with both build ids")
{
    CHECK(azl::versionWarning("20261017120000-abc1234", "20261017120000-abc1234").empty());
    CHECK(azl::versionWarning("", "20261017120000-abc1234").empty());
    CHECK(azl::versionWarning("20261016090000-0ff1ce0", "20261017120000-abc1234") ==
          "venue build 20261016090000-0ff1ce0, UI build 20261017120000-abc1234");

    azl::VenueSources sources;
    sources.probe = [](const Venue&) {
        return VenueProbe{.reachable = true, .answered = true, .build = "19700101000000-old"};
    };
    const azl::VenueScan scan =
        azl::scanVenues(sources, {{{"club", "club.local", 7400, "tailscale", true}}, ""}, {});
    REQUIRE(scan.entries.size() == 1);
    CHECK(scan.entries[0].dot == VenueDot::Warning);
    CHECK(scan.entries[0].versionWarning == "venue build 19700101000000-old, UI build " + std::string(azl::buildId()));
}

TEST_CASE("a probe reads a running engine's build and guard level and changes nothing; a closed port is offline")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    harness.step();
    const uint64_t change = harness.document().change();

    const VenueProbe probe = azl::probeVenue("127.0.0.1", server.port());
    CHECK(probe.reachable);
    CHECK(probe.answered);
    CHECK(probe.build == azl::buildId());
    REQUIRE(probe.health);
    CHECK(probe.health->guardLevel == 0);
    CHECK(probe.error.empty());
    CHECK(azl::venueDot({"here", "127.0.0.1", server.port(), "manual", true}, probe) == VenueDot::Ok);
    CHECK(harness.document().change() == change);

    uint16_t closed = 0;
    {
        azl::WebSocketServer gone(harness.document(), 0);
        closed = gone.port();
    }
    const VenueProbe nobody = azl::probeVenue("127.0.0.1", closed, std::chrono::milliseconds(500));
    CHECK_FALSE(nobody.reachable);
    CHECK_FALSE(nobody.error.empty());
    CHECK(azl::venueDot({"gone", "127.0.0.1", closed, "manual", true}, nobody) == VenueDot::Offline);
}

TEST_CASE("the venue monitor lists, probes and calls back; a typed host joins at once; Tailscale's error passes on")
{
    std::atomic<int> listed{0}, changed{0};
    azl::VenueSources sources;
    sources.tailscale = [&] {
        ++listed;
        return azl::TailscaleList{{{"club", "club.tail1234.ts.net", 7400, "tailscale", true},
                                   {"dark", "dark.tail1234.ts.net", 7400, "tailscale", false}},
                                  "Tailscale rejected the API key (HTTP 401)"};
    };
    sources.bonjour = [] { return std::vector<Venue>{{"lan", "lan.local", 7400, "bonjour", true}}; };
    std::atomic<int> probes{0};
    sources.probe = [&](const Venue& venue) {
        ++probes;
        if (venue.name == "lan")
            return VenueProbe{.reachable = true,
                              .answered = true,
                              .build = azl::buildId(),
                              .health = azl::VenueHealth{.guardLevel = 2}};
        return VenueProbe{.reachable = true, .answered = true, .build = azl::buildId()};
    };
    auto monitor = std::make_unique<azl::VenueMonitor>(sources, [&] { ++changed; }, std::chrono::milliseconds(50),
                                                       std::chrono::hours(1));
    const auto waitFor = [](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    };
    REQUIRE(waitFor([&] { return monitor->scans() > 0; }));
    std::vector<azl::VenueEntry> entries = monitor->entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].dot == VenueDot::Ok);
    CHECK(entries[1].dot == VenueDot::Offline);
    CHECK(entries[2].dot == VenueDot::Warning);
    CHECK(monitor->tailscaleError() == "Tailscale rejected the API key (HTTP 401)");
    CHECK(changed > 0);
    REQUIRE(waitFor([&] { return monitor->scans() > 3; }));
    CHECK(listed == 1);

    monitor->addManual("10.0.0.5");
    REQUIRE(waitFor([&] { return monitor->entries().size() == 4; }));
    CHECK(monitor->entries()[3].venue.source == "manual");
    monitor->refresh();
    REQUIRE(waitFor([&] { return listed == 2; }));

    const auto start = std::chrono::steady_clock::now();
    monitor.reset();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    const int after = changed;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(changed == after);
}

TEST_CASE("a venue plays on exactly as it was when its UI goes away mid-show")
{
    TempDir media;
    const fs::path clip = copyMedia("count_100.mp4", media.path / "count_100.mp4");
    azl::EngineHarness venue({.width = 320, .height = 180});
    azl::EngineHarness reference({.width = 320, .height = 180});
    {
        Client setup(venue.document());
        playMedia(setup, clip);
    }
    {
        Client setup(reference.document());
        playMedia(setup, clip);
    }
    const auto stepBoth = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            venue.step();
            reference.step();
            REQUIRE(maxDifference(venue.readScreen(), reference.readScreen()) <= 2);
        }
    };
    stepBoth(10);

    azl::WebSocketServer server(venue.document(), 0);
    {
        std::atomic<int> received{0};
        azl::WebSocketClient ui("127.0.0.1", server.port(), azl::Encoding::MessagePack,
                                [&](const Message&) { ++received; });
        ui.send(msg::hello());
        ui.send({{"type", "audio"}, {"op", "subscribe"}, {"rate", 20}});
        ui.send({{"type", "preview"}, {"op", "watch"}, {"sources", {"composition"}}, {"via", "jpeg"},
                 {"width", 320}, {"height", 180}, {"fps", 12}});
        stepBoth(20);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (received == 0 && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(received > 0);
    }
    const uint64_t change = venue.document().change();
    const Message document = venue.document().document();

    stepBoth(60);
    CHECK(showsSomething(venue.readScreen()));
    CHECK(venue.stats().rendered == reference.stats().rendered);
    CHECK(venue.document().change() == change);
    CHECK(Message(venue.document().document()) == document);
    CHECK(venue.transport().beat == doctest::Approx(reference.transport().beat));
}
