#include "djlink/clock_follower.hpp"
#include "djlink/djlink_packets.hpp"
#include "djlink/djlink_receiver.hpp"
#include "djlink/djlink_service.hpp"
#include "djlink/djlink_state.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

namespace {

namespace dj = azl::djlink;

const fs::path kSource = sourceDir();

std::vector<uint8_t> fixture(const std::string& name)
{
    std::ifstream in(kSource / "tests/djlink" / name, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), {}};
}

template <typename T>
T parsed(uint16_t port, const std::vector<uint8_t>& bytes)
{
    const std::optional<dj::Packet> packet = dj::parse(port, bytes.data(), bytes.size());
    REQUIRE(packet);
    REQUIRE(std::holds_alternative<T>(*packet));
    return std::get<T>(*packet);
}

// Fixture packets with a field changed, for replays.
std::vector<uint8_t> beatFrom(int player, int beatInBar, double bpm = 126.0)
{
    std::vector<uint8_t> b = fixture("beat_cdj2.bin");
    b[0x21] = b[0x5f] = uint8_t(player);
    b[0x5c] = uint8_t(beatInBar);
    const uint16_t hundredths = uint16_t(std::lround(bpm * 100));
    b[0x5a] = uint8_t(hundredths >> 8);
    b[0x5b] = uint8_t(hundredths);
    b[0x54] = 0x00, b[0x55] = 0x10, b[0x56] = 0x00, b[0x57] = 0x00;
    return b;
}

std::vector<uint8_t> onAirFlags(std::array<uint8_t, 4> flags)
{
    std::vector<uint8_t> b = fixture("onair_4ch.bin");
    std::copy(flags.begin(), flags.end(), b.begin() + 0x24);
    return b;
}

void feed(dj::LinkState& state, uint16_t port, const std::vector<uint8_t>& bytes, double now)
{
    const std::optional<dj::Packet> packet = dj::parse(port, bytes.data(), bytes.size());
    REQUIRE(packet);
    state.receive(*packet, now);
}

uint16_t freeUdpPort()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{.sin_len = sizeof(sockaddr_in), .sin_family = AF_INET};
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0);
    socklen_t length = sizeof at;
    getsockname(fd, reinterpret_cast<sockaddr*>(&at), &length);
    close(fd);
    return ntohs(at.sin_port);
}

dj::Receiver::Options loopback()
{
    return {"lo0", "127.0.0.1", {freeUdpPort(), freeUdpPort(), freeUdpPort()}};
}

// A test-side device on loopback: it plays the CDJs' part, and hears anything sent back to it.
struct FakeDeck {
    FakeDeck()
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in at{.sin_len = sizeof(sockaddr_in), .sin_family = AF_INET};
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0);
    }
    ~FakeDeck() { close(fd); }

    void play(uint16_t port, const std::vector<uint8_t>& bytes) const
    {
        sockaddr_in to{.sin_len = sizeof(sockaddr_in), .sin_family = AF_INET, .sin_port = htons(port)};
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) ==
                ssize_t(bytes.size()));
    }

    // Datagrams that arrive within `ms`.
    int heard(int ms) const
    {
        int count = 0;
        pollfd p{fd, POLLIN, 0};
        uint8_t buffer[2048];
        while (poll(&p, 1, ms) > 0 && recv(fd, buffer, sizeof buffer, 0) > 0)
            ++count;
        return count;
    }

    int fd = -1;
};

double steadyNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename Done>
bool waitFor(Done done, double seconds = 5.0)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::pair<int, std::string> runCtl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    const int status = pclose(pipe);
    return {WEXITSTATUS(status), out};
}

} // namespace

TEST_CASE("djlink: announcements, beats, on-air flags and status parse from the documented packet layouts")
{
    const auto cdj = parsed<dj::Announcement>(dj::kAnnouncePort, fixture("keepalive_cdj2.bin"));
    CHECK(cdj.number == 2);
    CHECK(cdj.name == "CDJ-2000NXS2");
    CHECK(cdj.ip == std::array<uint8_t, 4>{169, 254, 10, 2});
    CHECK(cdj.mac == std::array<uint8_t, 6>{0x00, 0xe0, 0x36, 0x01, 0x02, 0x03});
    CHECK(cdj.peers == 3);
    const auto djm = parsed<dj::Announcement>(dj::kAnnouncePort, fixture("keepalive_djm.bin"));
    CHECK(djm.number == 0x21);
    CHECK(dj::kindOf(djm.number) == dj::DeviceKind::Mixer);
    CHECK(dj::kindOf(cdj.number) == dj::DeviceKind::Player);
    CHECK(dj::kindOf(17) == dj::DeviceKind::Computer);

    const auto beat = parsed<dj::Beat>(dj::kBeatPort, fixture("beat_cdj2.bin"));
    CHECK(beat.number == 2);
    CHECK(beat.trackBpm == doctest::Approx(126.0));
    CHECK(dj::pitchPercent(beat.pitch) == doctest::Approx(1.5).epsilon(0.001));
    CHECK(beat.beatInBar == 3);
    CHECK(beat.nextBeatMs == 476);
    CHECK(beat.nextBarMs == 2 * 476);

    const auto four = parsed<dj::OnAir>(dj::kBeatPort, fixture("onair_4ch.bin"));
    CHECK(four.number == 0x21);
    CHECK(four.channels == std::vector<bool>{false, true, false, false});
    const auto six = parsed<dj::OnAir>(dj::kBeatPort, fixture("onair_6ch.bin"));
    CHECK(six.channels == std::vector<bool>{false, false, false, false, false, true});

    const auto status = parsed<dj::PlayerStatus>(dj::kStatusPort, fixture("status_cdj2.bin"));
    CHECK(status.number == 2);
    CHECK(status.trackId == 4242);
    CHECK(status.trackPlayer == 2);
    CHECK(status.trackSlot == 3);
    CHECK(status.trackType == 1);
    CHECK(status.playState == 0x03);
    CHECK(status.playing);
    CHECK(status.master);
    CHECK(status.onAir);
    CHECK_FALSE(status.synced);
    CHECK(status.trackBpm == doctest::Approx(126.0));
    CHECK(status.beat == 517u);
    CHECK(status.beatInBar == 2);
    const auto mixer = parsed<dj::MixerStatus>(dj::kStatusPort, fixture("status_djm.bin"));
    CHECK(mixer.number == 0x21);
    CHECK(mixer.bpm == doctest::Approx(126.0));
    CHECK(mixer.beatInBar == 4);
    CHECK_FALSE(mixer.master);

    // Not Pro DJ Link, cut short, or on the wrong port.
    std::vector<uint8_t> noise(0x60, 0x51);
    CHECK_FALSE(dj::parse(dj::kBeatPort, noise.data(), noise.size()));
    const std::vector<uint8_t> b = fixture("beat_cdj2.bin");
    CHECK_FALSE(dj::parse(dj::kBeatPort, b.data(), 0x40));
    CHECK_FALSE(dj::parse(dj::kStatusPort, b.data(), b.size()));
    CHECK_FALSE(dj::parse(4242, b.data(), b.size()));
    std::vector<uint8_t> noBar = b;
    noBar[0x5c] = 9;
    CHECK_FALSE(dj::parse(dj::kBeatPort, noBar.data(), noBar.size()));
}

TEST_CASE("djlink: the on-air player follows the mixer, keeps through a transition and is lost when beats stop")
{
    const double i1 = 60.0 / 124.0;
    const double i2 = 60.0 / 126.0;
    dj::LinkState state;
    CHECK(state.onAir(0.0) == 0);
    CHECK_FALSE(state.onAirBeat(0.0));

    // One beat is not a beat grid; regular beats are. No on-air flags yet: the one player with a grid.
    feed(state, dj::kBeatPort, beatFrom(1, 1, 124.0), 0.0);
    CHECK(state.onAir(0.0) == 0);
    feed(state, dj::kBeatPort, beatFrom(1, 2, 124.0), i1);
    feed(state, dj::kBeatPort, beatFrom(1, 3, 124.0), 2 * i1);
    CHECK(state.onAir(2 * i1) == 1);
    // A second one, still no flags: the one followed stays.
    for (int n = 0; n < 3; ++n)
        feed(state, dj::kBeatPort, beatFrom(2, n + 1), 0.1 + n * i2);
    CHECK(state.onAir(0.1 + 2 * i2) == 1);

    // The mixer puts channel 2 on air.
    feed(state, dj::kBeatPort, onAirFlags({0, 1, 0, 0}), 1.2);
    CHECK(state.onAir(1.2) == 2);
    const std::optional<dj::OnAirBeat> beat = state.onAirBeat(1.2);
    REQUIRE(beat);
    CHECK(beat->player == 2);
    CHECK(beat->bpm == doctest::Approx(126.0));
    CHECK(beat->time == 0.1 + 2 * i2);
    CHECK(beat->beatInBar == 3);

    // Both up during the transition: player 2 stays followed; channel 2 down: player 1 takes over.
    feed(state, dj::kBeatPort, onAirFlags({1, 1, 0, 0}), 1.5);
    feed(state, dj::kBeatPort, beatFrom(1, 4, 124.0), 3 * i1);
    feed(state, dj::kBeatPort, beatFrom(2, 4), 0.1 + 3 * i2);
    CHECK(state.onAir(1.55) == 2);
    feed(state, dj::kBeatPort, onAirFlags({1, 0, 0, 0}), 1.6);
    CHECK(state.onAir(1.6) == 1);
    CHECK(state.onAirBeat(1.6)->bpm == doctest::Approx(124.0));

    // Player 1 stops: a beat or so later its grid has run out and nothing is followed.
    const double lastBeat = 3 * i1;
    CHECK(state.onAir(lastBeat + 2.2 * i1) == 1);
    CHECK(state.onAir(lastBeat + 2.3 * i1) == 0);
    CHECK_FALSE(state.onAirBeat(10.0));

    // Starting fresh with two grids and no flags, neither is followed; both on air, the tempo master is chosen.
    dj::LinkState fresh;
    for (int n = 0; n < 3; ++n) {
        feed(fresh, dj::kBeatPort, beatFrom(1, n + 1), n * i2);
        feed(fresh, dj::kBeatPort, beatFrom(2, n + 1), n * i2);
    }
    CHECK(fresh.onAir(2 * i2) == 0);
    feed(fresh, dj::kBeatPort, onAirFlags({1, 1, 0, 0}), 2 * i2);
    feed(fresh, dj::kStatusPort, fixture("status_cdj2.bin"), 2 * i2);
    CHECK(fresh.onAir(2 * i2) == 2);

    // A beat's pitch bends its BPM.
    dj::LinkState pitched;
    const double pitchedInterval = 60.0 / (126.0 * 1.015);
    for (int n = 0; n < 3; ++n) {
        std::vector<uint8_t> b = fixture("beat_cdj2.bin");
        b[0x5c] = uint8_t(n + 1);
        feed(pitched, dj::kBeatPort, b, n * pitchedInterval);
    }
    CHECK(pitched.onAirBeat(2 * pitchedInterval)->bpm == doctest::Approx(126.0 * 1.015).epsilon(0.0001));
}

TEST_CASE("djlink: a cued, scrubbing or paused player never becomes the beat source")
{
    const double interval = 60.0 / 126.0;
    dj::LinkState state;
    // Player 2 plays on air; player 3, also up on the mixer, is scrubbed: beats at odd intervals and out of order.
    feed(state, dj::kBeatPort, onAirFlags({0, 1, 1, 0}), 0.0);
    const double scrub[] = {0.05, 0.2, 0.95, 1.1, 1.3};
    const int scrubInBar[] = {1, 3, 2, 2, 4};
    for (int n = 0; n < 5; ++n) {
        feed(state, dj::kBeatPort, beatFrom(2, n % 4 + 1), n * interval);
        feed(state, dj::kBeatPort, beatFrom(3, scrubInBar[n], 128.0), scrub[n]);
    }
    const double now = 4 * interval;
    CHECK(state.grid(state.players().at(2), now));
    CHECK_FALSE(state.grid(state.players().at(3), now));
    CHECK(state.onAir(now) == 2);

    // Only the scrubbed one on air: nothing is followed.
    feed(state, dj::kBeatPort, onAirFlags({0, 0, 1, 0}), now);
    CHECK(state.onAir(now) == 0);

    // A beat with no sane BPM is no grid either.
    dj::LinkState stopped;
    for (int n = 0; n < 3; ++n)
        feed(stopped, dj::kBeatPort, beatFrom(1, n + 1, 0.0), n * interval);
    CHECK(stopped.onAir(2 * interval) == 0);

    // Its own status says paused (where status arrives): not followed, beats or not.
    dj::LinkState paused;
    std::vector<uint8_t> status = fixture("status_cdj2.bin");
    status[0x89] &= uint8_t(~0x40);
    for (int n = 0; n < 3; ++n)
        feed(paused, dj::kBeatPort, beatFrom(2, n + 1), n * interval);
    feed(paused, dj::kStatusPort, status, 2 * interval);
    CHECK(paused.onAir(2 * interval) == 0);
}

TEST_CASE("djlink: the followed timeline lands on the player's beats, numbered to the clock's bar")
{
    const dj::OnAirBeat beat{2, 10.0, 3, 120.0};
    // The clock is at beat 41.3 at 10.25 s: at the player's beat (10.0 s) it was at 40.8; the nearest beat third in
    // its bar is 42.
    const azl::BeatTimeline t = dj::followTimeline(beat, 41.3, 10.25);
    CHECK(t.time == 10.0);
    CHECK(t.beat == doctest::Approx(42.0));
    CHECK(t.bpm == 120.0);
    CHECK(t.beatAt(10.5) == doctest::Approx(43.0));
    CHECK(std::fmod(t.beat, 4.0) == doctest::Approx(2.0));
    // Within two beats either way, the same bar.
    CHECK(dj::followTimeline(beat, 43.9, 10.0).beat == doctest::Approx(42.0));
    CHECK(dj::followTimeline(beat, 44.1, 10.0).beat == doctest::Approx(46.0));
    CHECK(dj::followTimeline({1, 0.0, 1, 128.0}, 0.0, 0.0).beat == doctest::Approx(0.0));
}

TEST_CASE("djlink: driving the engine's follow with the on-air beat locks the clock, and letting go never jumps")
{
    azl::EngineHarness harness({.width = 64, .height = 36});
    harness.step(30);
    dj::LinkState state;
    for (int n = 0; n < 3; ++n)
        feed(state, dj::kBeatPort, beatFrom(1, n + 1, 126.0), harness.time() - (2 - n) * 60.0 / 126.0);
    const std::optional<dj::OnAirBeat> beat = state.onAirBeat(harness.time());
    REQUIRE(beat);
    harness.engine().follow(dj::followTimeline(*beat, harness.transport().beat, harness.time()));
    harness.step(60);
    const azl::TransportState locked = harness.transport();
    CHECK(locked.bpm == doctest::Approx(126.0));
    CHECK(std::fmod(locked.beat - beat->beatsSince(harness.time()), 4.0) == doctest::Approx(2.0).epsilon(0.03));

    // The link is lost: the clock runs on from where it is; no frame moves by more than a frame's worth of beats.
    harness.engine().unfollow();
    double previous = harness.transport().beat;
    for (int i = 0; i < 30; ++i) {
        harness.step();
        const double now = harness.transport().beat;
        CHECK(now - previous == doctest::Approx(126.0 / 60.0 / 60.0).epsilon(0.1));
        previous = now;
    }
}

TEST_CASE("djlink: nothing in the module can send onto the network")
{
    // The receive-only promise in code: no source in src/djlink calls anything that puts a packet on a socket,
    // connects one, or joins a multicast group.
    const std::regex sending(R"(\b(send|sendto|sendmsg|sendmsg_x|sendfile|sendSome|sendAll|connect|connectx|write|)"
                             R"(writev|pwrite)\s*\(|)"
                             R"(\b(nw_connection_\w+|CFSocket\w*|IP_ADD_MEMBERSHIP|IP_MULTICAST_\w+|SO_BROADCAST)\b)");
    int files = 0;
    for (const auto& entry : fs::directory_iterator(kSource / "src/djlink")) {
        std::ifstream in(entry.path());
        std::stringstream text;
        text << in.rdbuf();
        std::smatch found;
        const std::string source = text.str();
        const bool sends = std::regex_search(source, found, sending);
        const std::string why = entry.path().filename().string() + " uses " + found.str();
        CHECK_MESSAGE(!sends, why);
        ++files;
    }
    CHECK(files >= 8);
}

TEST_CASE("djlink: the receiver hears a loopback feed, parses it, and sends nothing back")
{
    azl::DocumentServer document(azl::defaultComposition(), [](auto) {});
    TempDir dir;
    const dj::Receiver::Options options = loopback();
    dj::saveSettings(dir.path / "djlink.json", {true, "lo0"});
    dj::Service service(document, steadyNow, dir.path / "djlink.json", options);
    REQUIRE(service.status()["listening"] == true);

    FakeDeck deck;
    deck.play(options.ports[0], fixture("keepalive_cdj2.bin"));
    deck.play(options.ports[0], fixture("keepalive_djm.bin"));
    deck.play(options.ports[1], onAirFlags({0, 1, 0, 0}));
    deck.play(options.ports[2], fixture("status_cdj2.bin"));
    deck.play(options.ports[2], fixture("status_djm.bin"));
    deck.play(options.ports[1], std::vector<uint8_t>(0x30, 0));
    for (int n = 1; n <= 3; ++n) {
        if (n > 1)
            std::this_thread::sleep_for(std::chrono::duration<double>(60.0 / 126.0));
        deck.play(options.ports[1], beatFrom(2, n));
    }
    REQUIRE(waitFor([&] { return service.status()["datagrams"] == 9; }));
    // Not a reply, not a keep-alive, not a claim: the deck hears nothing while the engine listens.
    CHECK(deck.heard(400) == 0);

    const Message status = service.status();
    CHECK(status["unknown"] == 1);
    CHECK(status["time"].get<double>() == doctest::Approx(steadyNow()).epsilon(1e-6));
    CHECK(status["devices"].size() == 2);
    CHECK(status["devices"][0]["number"] == 2);
    CHECK(status["devices"][0]["ip"] == "169.254.10.2");
    CHECK(status["devices"][1]["kind"] == "mixer");
    CHECK(status["onAir"] == 2);
    CHECK(status["bpm"].get<double>() == doctest::Approx(126.0));
    CHECK(status["beat"]["inBar"] == 3);
    REQUIRE(status["players"].size() == 1);
    const Message& player = status["players"][0];
    CHECK(player["onAir"] == true);
    CHECK(player["master"] == true);
    CHECK(player["playing"] == true);
    CHECK(player["track"]["id"] == 4242);
    CHECK(status["mixer"]["bpm"].get<double>() == doctest::Approx(126.0));
    CHECK(service.onAirBeat(steadyNow())->player == 2);
}

TEST_CASE("djlink: the setting is remembered on the machine and set over the protocol")
{
    azl::DocumentServer document(azl::defaultComposition(), [](auto) {});
    TempDir dir;
    const fs::path file = dir.path / "djlink.json";
    CHECK(dj::loadSettings(file) == dj::Settings{});
    {
        std::ofstream(dir.path / "broken.json") << "{nope";
        CHECK(dj::loadSettings(dir.path / "broken.json") == dj::Settings{});
    }
    const dj::Receiver::Options options = loopback();
    {
        dj::Service service(document, steadyNow, file, options);
        CHECK(service.status()["enabled"] == false);
        Client a(document);
        Client b(document);
        a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}});
        CHECK(a.next()["type"] == "error");
        a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}, {"interface", "lo0"}});
        const Message changed = b.next();
        CHECK(changed["op"] == "status");
        CHECK(changed["enabled"] == true);
        CHECK(changed["interface"] == "lo0");
        CHECK(changed["listening"] == true);
        CHECK(a.next() == changed);
        a.send({{"type", "djlink"}, {"op", "frobnicate"}});
        CHECK(a.next()["type"] == "error");
    }
    CHECK(dj::loadSettings(file) == dj::Settings{true, "lo0"});
    {
        // After a restart it listens again by itself; off keeps the interface for the next on.
        dj::Service service(document, steadyNow, file, options);
        CHECK(service.status()["listening"] == true);
        Client a(document);
        a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", false}});
        CHECK(a.next()["listening"] == false);
        a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}});
        CHECK(a.next()["interface"] == "lo0");
        a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", false}});
        a.next();
    }
    CHECK(dj::loadSettings(file) == dj::Settings{false, "lo0"});

    // A missing interface is kept and reported, so a USB adapter can be plugged in later.
    dj::Service service(document, steadyNow, {}, options);
    Client a(document);
    a.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}, {"interface", "azl-none0"}});
    const Message missing = a.next();
    CHECK(missing["listening"] == false);
    CHECK(missing["error"] == "no interface azl-none0");
}

TEST_CASE("djlink: nocturne-ctl turns the DJ link on and off and prints its status")
{
    azl::EngineHarness harness({.width = 64, .height = 36});
    TempDir dir;
    dj::Service service(harness.document(), steadyNow, dir.path / "djlink.json", loopback());
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();

    auto [code, out] = runCtl(port, "djlink on lo0");
    CHECK(code == 0);
    CHECK(out.find("\"listening\":true") != std::string::npos);
    CHECK(dj::loadSettings(dir.path / "djlink.json") == dj::Settings{true, "lo0"});
    std::tie(code, out) = runCtl(port, "djlink status");
    CHECK(code == 0);
    CHECK(out.find("\"op\":\"status\"") != std::string::npos);
    CHECK(out.find("\"interface\":\"lo0\"") != std::string::npos);
    std::tie(code, out) = runCtl(port, "djlink off");
    CHECK(code == 0);
    CHECK(dj::loadSettings(dir.path / "djlink.json") == dj::Settings{false, "lo0"});
    std::tie(code, out) = runCtl(port, "djlink on");
    CHECK(code == 0);
    CHECK(service.settings() == dj::Settings{true, "lo0"});
    CHECK(runCtl(port, "djlink sideways").first == 2);
    CHECK(runCtl(port, "djlink off en7").first == 2);
}

namespace {

// A CDJ on the fake deck: regular beats at `bpm` from `next`, or, scrubbed, beats at odd intervals and out of order.
struct Deck {
    int player = 1;
    double bpm = 126.0;
    double next = 0.0;
    int inBar = 1;
    bool scrubbed = false;
    double sent = 0.0;
    int sentInBar = 0;
    int count = 0;

    void advance()
    {
        static constexpr double kScrub[] = {0.3, 1.7, 0.6, 1.1};
        static constexpr int kScrubBar[] = {3, 2, 4, 2};
        const double interval = 60.0 / bpm;
        next += scrubbed ? interval * kScrub[count % 4] : interval;
        inBar = scrubbed ? kScrubBar[count % 4] : inBar % 4 + 1;
        ++count;
    }
};

// A venue engine with the DJ link on over loopback and the clock follower stepped by the test, on the harness's
// frame clock.
struct DjRig {
    DjRig()
        : service(harness.document(), [this] { return now.load(); }, {}, options),
          follower(service, harness.engine(), harness.document(), [this] { return now.load(); },
                   {.thread = false, .allowed = [this] { return allowed.load(); }})
    {
        Client director(harness.document());
        director.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}, {"interface", "lo0"}});
        REQUIRE(director.next()["listening"] == true);
    }

    // Sends a packet from the deck and waits for the engine to hear it.
    void play(uint16_t port, const std::vector<uint8_t>& bytes)
    {
        const uint64_t before = service.status()["datagrams"];
        deck.play(port, bytes);
        REQUIRE(waitFor([&] { return service.status()["datagrams"].get<uint64_t>() > before; }));
    }

    void setOnAir(std::array<uint8_t, 4> flags)
    {
        onAir = flags;
        play(options.ports[1], onAirFlags(flags));
    }

    // Steps one frame, the decks playing their due beats first (and the mixer its flags once a second); returns how
    // far the beat moved.
    double frame()
    {
        for (Deck& d : decks) {
            if (now < d.next)
                continue;
            play(options.ports[1], beatFrom(d.player, d.inBar, d.bpm));
            d.sent = now;
            d.sentInBar = d.inBar;
            d.advance();
        }
        if (onAir && ++frames % 60 == 0)
            play(options.ports[1], onAirFlags(*onAir));
        follower.step(now);
        const double before = harness.transport().beat;
        harness.step();
        now = harness.time();
        follower.step(now);
        const double moved = harness.transport().beat - before;
        largest = std::max(largest, moved);
        smallest = std::min(smallest, moved);
        return moved;
    }

    void run(double seconds)
    {
        for (int i = 0; i < int(seconds * 60); ++i)
            frame();
    }

    void resetMoves()
    {
        largest = 0.0;
        smallest = 1.0;
    }

    // How far the clock's bar is from the deck's at its last beat, in beats (-2..2).
    double barOffset(const Deck& d) const
    {
        const double beat = harness.transport().timeline.beatAt(d.sent);
        return std::fmod(beat - (d.sentInBar - 1) + 6.0, 4.0) - 2.0;
    }

    azl::EngineHarness harness{{.width = 64, .height = 36}};
    std::atomic<double> now{0.0};
    std::atomic<bool> allowed{true};
    dj::Receiver::Options options = loopback();
    dj::Service service;
    dj::ClockFollower follower;
    FakeDeck deck;
    std::vector<Deck> decks;
    std::optional<std::array<uint8_t, 4>> onAir;
    int frames = 0;
    double largest = 0.0;
    double smallest = 1.0;
};

constexpr double kPerFrame126 = 126.0 / 60.0 / 60.0;
// Beats leave the deck on frame boundaries, up to a frame (0.035 beats) late.
constexpr double kAligned = 0.04;

} // namespace

TEST_CASE("djlink: the beat clock follows the on-air CDJ without a jump and keeps its tempo when the link is lost")
{
    DjRig rig;
    REQUIRE(rig.harness.document().document().bpm == 120.0);
    rig.run(0.75);
    rig.decks.push_back({.player = 1, .next = rig.now, .inBar = 2});
    rig.run(14.0);
    CHECK(rig.follower.following());
    CHECK_FALSE(rig.follower.aligning());
    CHECK(rig.largest < kPerFrame126 * (1.0 + dj::ClockFollower::kAlignBend) * 1.02);
    CHECK(rig.harness.transport().bpm == doctest::Approx(126.0));
    CHECK(std::abs(rig.barOffset(rig.decks[0])) < kAligned);

    // The deck goes quiet: once the beat is lost the document keeps 126 BPM and the clock runs on, frame by frame.
    rig.decks.clear();
    rig.resetMoves();
    rig.run(4.0);
    CHECK_FALSE(rig.follower.following());
    CHECK(rig.harness.document().document().bpm == doctest::Approx(126.0));
    CHECK(rig.harness.transport().bpm == doctest::Approx(126.0));
    CHECK(rig.smallest > kPerFrame126 * 0.95);
    CHECK(rig.largest < kPerFrame126 * 1.05);
}

TEST_CASE("djlink: an on-air handover to a player half a bar out bends onto its bar without a jump")
{
    DjRig rig;
    rig.setOnAir({0, 1, 0, 0});
    rig.decks.push_back({.player = 2, .next = rig.now});
    rig.run(14.0);
    REQUIRE(rig.follower.following());
    REQUIRE(std::abs(rig.barOffset(rig.decks[0])) < kAligned);

    // CDJ 3 starts in the headphones with its downbeats half a bar from CDJ 2's; the clock stays with CDJ 2.
    const Deck two = rig.decks[0];
    rig.decks.push_back({.player = 3, .next = two.next + 0.05, .inBar = (two.inBar + 1) % 4 + 1});
    rig.run(3.0);
    CHECK_FALSE(rig.follower.aligning());
    CHECK(std::abs(rig.barOffset(rig.decks[0])) < kAligned);
    CHECK(std::abs(std::abs(rig.barOffset(rig.decks[1])) - 2.0) < 0.2);

    // The mixer hands over to CDJ 3: no frame moves more than the alignment bend allows, and the clock is on CDJ 3's
    // bar within twelve beats (half a bar at a quarter of the tempo is eight, then it settles).
    rig.resetMoves();
    rig.setOnAir({0, 0, 1, 0});
    const double handover = rig.now;
    bool pulled = false;
    bool aligned = false;
    double alignedAt = 0.0;
    for (int i = 0; i < 60 * 8; ++i) {
        rig.frame();
        pulled = pulled || rig.follower.aligning();
        if (pulled && !aligned && !rig.follower.aligning()) {
            aligned = true;
            alignedAt = rig.now;
        }
    }
    CHECK(rig.largest < kPerFrame126 * (1.0 + dj::ClockFollower::kAlignBend) * 1.02);
    CHECK(rig.smallest > kPerFrame126 * (1.0 - dj::ClockFollower::kAlignBend) * 0.98);
    REQUIRE(pulled);
    REQUIRE(aligned);
    const double beats = (alignedAt - handover) * 126.0 / 60.0;
    MESSAGE("aligned " << beats << " beats after the handover");
    CHECK(beats < 12.0);
    CHECK(std::abs(rig.barOffset(rig.decks[1])) < kAligned);
    CHECK(rig.harness.transport().bpm == doctest::Approx(126.0));
}

TEST_CASE("djlink: a cued player scrubbing while another plays on air leaves the clock alone")
{
    DjRig rig;
    rig.setOnAir({0, 1, 0, 0});
    rig.decks.push_back({.player = 2, .next = rig.now});
    rig.run(14.0);
    REQUIRE(std::abs(rig.barOffset(rig.decks[0])) < kAligned);

    // CDJ 3 is scrubbed through its track, cued and then up on the mixer too, still scrubbing.
    rig.decks.push_back({.player = 3, .bpm = 128.0, .next = rig.now + 0.1, .scrubbed = true});
    rig.resetMoves();
    rig.run(4.0);
    rig.setOnAir({0, 1, 1, 0});
    rig.run(4.0);
    CHECK(rig.service.onAirBeat(rig.now)->player == 2);
    CHECK_FALSE(rig.follower.aligning());
    CHECK(rig.largest < kPerFrame126 * 1.03);
    CHECK(rig.smallest > kPerFrame126 * 0.97);
    CHECK(std::abs(rig.barOffset(rig.decks[0])) < kAligned);
    CHECK(rig.harness.transport().bpm == doctest::Approx(126.0));

    // CDJ 2 off air, only the scrubbed one up: the beat is lost and the clock carries on at CDJ 2's tempo.
    rig.decks.erase(rig.decks.begin());
    rig.setOnAir({0, 0, 1, 0});
    rig.run(3.0);
    CHECK_FALSE(rig.follower.following());
    CHECK(rig.harness.transport().bpm == doctest::Approx(126.0));
}

TEST_CASE("djlink: the beat clock is left alone while the engine follows another engine")
{
    DjRig rig;
    rig.allowed = false;
    rig.decks.push_back({.player = 1, .next = 0.5});
    rig.run(3.0);
    CHECK_FALSE(rig.follower.following());
    CHECK(rig.harness.transport().bpm == 120.0);
}

namespace {

// tools/djlink-sim playing a scenario file in real time to loopback ports, never a broadcast address.
struct Simulator {
    static fs::path script() { return kSource / "tools/djlink-sim/djlink_sim.py"; }

    Simulator(const std::array<uint16_t, 3>& ports, const fs::path& scenario)
    {
        const std::string command = "python3 '" + script().string() + "' --broadcast 127.0.0.1 --quiet --ports " +
                                    std::to_string(ports[0]) + "," + std::to_string(ports[1]) + "," +
                                    std::to_string(ports[2]) + " --scenario '" + scenario.string() + "' 2>&1";
        pipe = popen(command.c_str(), "r");
        REQUIRE(pipe);
    }
    ~Simulator()
    {
        if (pipe)
            pclose(pipe);
    }
    // Waits for the scenario to end; its exit code and output.
    std::pair<int, std::string> finish()
    {
        std::string out;
        char buffer[512];
        while (fgets(buffer, sizeof buffer, pipe))
            out += buffer;
        const int status = pclose(pipe);
        pipe = nullptr;
        return {WEXITSTATUS(status), out};
    }

    FILE* pipe = nullptr;
};

// The DJ link on over loopback with the clock follower, the harness paced to the wall clock since the simulator plays
// in real time.
struct LiveRig {
    LiveRig()
        : service(harness.document(), [this] { return now(); }, {}, options),
          follower(service, harness.engine(), harness.document(), [this] { return now(); }, {.thread = false})
    {
        Client director(harness.document());
        director.send({{"type", "djlink"}, {"op", "set"}, {"enabled", true}, {"interface", "lo0"}});
        REQUIRE(director.next()["listening"] == true);
    }

    double now() const { return base + steadyNow() - start; }

    // Renders frames until engine time reaches `until`, each once the wall clock reaches its time.
    void runTo(double until)
    {
        while (harness.time() < until) {
            while (now() < harness.time())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            follower.step(harness.time());
            const double before = harness.transport().beat;
            harness.step();
            const double moved = harness.transport().beat - before;
            largest = std::max(largest, moved);
            smallest = std::min(smallest, moved);
            aligned = aligned || follower.aligning();
            const std::optional<dj::OnAirBeat> beat = service.onAirBeat(now());
            const int player = beat ? beat->player : 0;
            handovers += player != 0 && followed != 0 && player != followed;
            followed = player != 0 ? player : followed;
            if (watchBar && beat)
                worstBar = std::max(worstBar, std::abs(statusBarOffset()));
        }
    }

    void resetMoves()
    {
        largest = 0.0;
        smallest = 1.0;
        aligned = false;
    }

    // How far the clock's bar is from the on-air player's at its last beat, in beats (-2..2).
    double barOffset()
    {
        const std::optional<dj::OnAirBeat> beat = service.onAirBeat(now());
        REQUIRE(beat);
        const double clock = harness.transport().timeline.beatAt(beat->time);
        return std::fmod(clock - (beat->beatInBar - 1) + 6.0, 4.0) - 2.0;
    }

    // The same from the status message, as a Director or the Mini check reads it: its beat in bar and phase against
    // the clock at the status's time.
    double statusBarOffset()
    {
        const Message status = service.status();
        const double dj = status["beat"]["inBar"].get<int>() - 1 + status["beat"]["phase"].get<double>();
        const double clock = harness.transport().timeline.beatAt(status["time"].get<double>());
        return std::fmod(clock - dj + 6.0, 4.0) - 2.0;
    }

    azl::EngineHarness harness{{.width = 64, .height = 36}};
    double base = harness.time();
    double start = steadyNow();
    dj::Receiver::Options options = loopback();
    dj::Service service;
    dj::ClockFollower follower;
    double largest = 0.0;
    double smallest = 1.0;
    bool aligned = false;
    int followed = 0;
    int handovers = 0;
    bool watchBar = false;
    double worstBar = 0.0;
};

// CDJ 2 on air at 128; CDJ 3 starts at 127.5 half a bar out, both up, the mixer hands over to CDJ 3, CDJ 2 stops,
// CDJ 3's pitch ramps it to 129, then the link goes quiet.
constexpr const char* kSimScenario = R"({
  "players": {"2": {"bpm": 128.0, "playing": true}, "3": {"bpm": 127.5}},
  "onAir": [2], "master": 2,
  "events": [
    {"at": 6, "player": 3, "play": true, "phaseFrom": 2, "offsetBeats": 2},
    {"at": 8, "onAir": [2, 3]},
    {"at": 11, "onAir": [3], "master": 3},
    {"at": 12, "player": 2, "play": false},
    {"at": 19, "player": 3, "rampTo": 129.0, "seconds": 3},
    {"at": 23, "loss": true},
    {"at": 27.5, "end": true}]})";

// Packets leave the simulator on its own clock and are stamped on arrival: allow a few ms either way.
constexpr double kSimAligned = 0.06;

} // namespace

TEST_CASE("djlink: the simulator's scenario drives the beat clock: follow, handover without a jump, tempo held on loss")
{
    if (!fs::exists(Simulator::script()) || std::system("python3 -c '' > /dev/null 2>&1") != 0) {
        MESSAGE("skipped: needs the source tree's tools/djlink-sim and python3");
        return;
    }
    TempDir dir;
    std::ofstream(dir.path / "scenario.json") << kSimScenario;
    LiveRig rig;
    const double s = rig.now();
    Simulator sim(rig.options.ports, dir.path / "scenario.json");
    const double perFrame = 128.0 / 60.0 / 60.0;

    // CDJ 2 on air; CDJ 3 comes up half a bar out and the clock stays with CDJ 2, bar for bar in every frame.
    rig.runTo(s + 7.0);
    rig.watchBar = true;
    rig.runTo(s + 10.9);
    rig.watchBar = false;
    CHECK(rig.worstBar < kSimAligned);
    REQUIRE(rig.follower.following());
    CHECK(rig.service.onAirBeat(rig.now())->player == 2);
    CHECK(rig.harness.transport().bpm == doctest::Approx(128.0));
    CHECK(std::abs(rig.barOffset()) < kSimAligned);

    // The handover: the clock bends onto CDJ 3's bar, never faster or slower than the alignment bend.
    rig.resetMoves();
    rig.runTo(s + 17.0);
    rig.worstBar = 0.0;
    rig.watchBar = true;
    rig.runTo(s + 18.5);
    CHECK(rig.worstBar < kSimAligned);
    CHECK(rig.handovers == 1);
    CHECK(rig.aligned);
    CHECK_FALSE(rig.follower.aligning());
    CHECK(rig.largest < perFrame * (1.0 + dj::ClockFollower::kAlignBend) * 1.02);
    CHECK(rig.smallest > perFrame * (1.0 - dj::ClockFollower::kAlignBend) * 0.98);
    CHECK(rig.service.onAirBeat(rig.now())->player == 3);
    CHECK(rig.harness.transport().bpm == doctest::Approx(127.5));
    CHECK(std::abs(rig.barOffset()) < kSimAligned);

    // The tempo ramp: the clock's tempo and bar follow.
    rig.runTo(s + 22.7);
    rig.watchBar = false;
    CHECK(rig.worstBar < kSimAligned);
    CHECK(rig.handovers == 1);
    CHECK(rig.harness.transport().bpm == doctest::Approx(129.0).epsilon(0.001));
    CHECK(std::abs(rig.barOffset()) < kSimAligned);

    // The link is lost: the document keeps 129 BPM and the clock runs on, frame by frame.
    rig.runTo(s + 25.0);
    CHECK_FALSE(rig.follower.following());
    rig.resetMoves();
    rig.runTo(s + 27.0);
    CHECK(rig.harness.document().document().bpm == doctest::Approx(129.0).epsilon(0.001));
    CHECK(rig.harness.transport().bpm == doctest::Approx(129.0).epsilon(0.001));
    CHECK(rig.smallest > 129.0 / 3600.0 * 0.99);
    CHECK(rig.largest < 129.0 / 3600.0 * 1.01);

    const auto [code, out] = sim.finish();
    CHECK_MESSAGE(code == 0, out);
}

namespace {

// CDJ 2 on air at 128, CDJ 3 playing at 127.5 with its pitch at +2 %, CDJ 4 cued.
constexpr const char* kRowsScenario = R"({
  "players": {"2": {"bpm": 128.0, "playing": true}, "3": {"bpm": 127.5, "trackBpm": 125.0, "playing": true},
              "4": {"bpm": 124.0}},
  "onAir": [2], "master": 2,
  "events": [{"at": 8, "end": true}]})";

Message machineStatus(azl::DocumentServer& document)
{
    Client client(document);
    client.send({{"type", "machine"}, {"op", "status"}});
    for (;;)
        if (Message m = client.next(); m["type"] == "machine" || m["type"] == "error")
            return m;
}

} // namespace

TEST_CASE("djlink: the machine status and the per-second line carry a row per player the simulator plays")
{
    if (!fs::exists(Simulator::script()) || std::system("python3 -c '' > /dev/null 2>&1") != 0) {
        MESSAGE("skipped: needs the source tree's tools/djlink-sim and python3");
        return;
    }
    CHECK(dj::statusLine({{"enabled", false}}) == "djlink off");
    TempDir dir;
    std::ofstream(dir.path / "scenario.json") << kRowsScenario;
    LiveRig rig;
    rig.harness.onDjlinkStatus([&rig] {
        Message state = rig.service.summary();
        state["following"] = rig.follower.following();
        return state;
    });
    const double s = rig.now();
    Simulator sim(rig.options.ports, dir.path / "scenario.json");
    rig.runTo(s + 6.0);

    const Message djlink = machineStatus(rig.harness.document())["clock"]["djlink"];
    INFO(djlink.dump());
    CHECK(djlink["enabled"] == true);
    CHECK(djlink["listening"] == true);
    CHECK(djlink["following"] == true);
    CHECK(djlink["onAir"] == 2);
    CHECK(djlink["bpm"].get<double>() == doctest::Approx(128.0));
    const Message players = djlink["players"];
    REQUIRE(players.size() == 3);
    CHECK(players[0]["number"] == 2);
    CHECK(players[0]["onAir"] == true);
    CHECK(players[0]["bpm"].get<double>() == doctest::Approx(128.0));
    CHECK(players[0]["pitch"].get<double>() == doctest::Approx(0.0));
    CHECK(players[0]["play"] == "playing");
    CHECK(players[0]["beatInBar"].get<int>() >= 1);
    CHECK(players[0]["beatInBar"].get<int>() <= 4);
    CHECK(players[1]["number"] == 3);
    CHECK(players[1]["onAir"] == false);
    CHECK(players[1]["bpm"].get<double>() == doctest::Approx(127.5).epsilon(0.0005));
    CHECK(players[1]["pitch"].get<double>() == doctest::Approx(2.0).epsilon(0.005));
    CHECK(players[1]["play"] == "playing");
    CHECK(players[2]["number"] == 4);
    CHECK(players[2]["onAir"] == false);
    CHECK(players[2]["bpm"].is_null());
    CHECK(players[2]["play"] == "cued");
    CHECK(players[2]["beatInBar"].is_null());

    const std::string line = dj::statusLine(djlink);
    INFO(line);
    CHECK(line.starts_with("djlink lo0, listening, "));
    CHECK(line.find(", on air 2 at 128.00 BPM, following | player 2 128.00 BPM +0.00% playing beat ") !=
          std::string::npos);
    CHECK(line.find(" on air | player 3 127.50 BPM +2.00% playing beat ") != std::string::npos);
    CHECK(line.ends_with(" off air | player 4 cued off air"));

    const auto [code, out] = sim.finish();
    CHECK_MESSAGE(code == 0, out);
    rig.harness.onDjlinkStatus(nullptr);
}
