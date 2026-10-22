#include "desk/desk_server.hpp"
#include "document/cue_edits.hpp"
#include "harness/engine_harness.hpp"
#include "http_get.hpp"
#include "net/lan.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"
#include "venues/gig.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <chrono>
#include <thread>

extern char** environ;

namespace {

constexpr int64_t kHour = 3600;
constexpr azl::EngineConfig kSize{.width = 320, .height = 180};

int64_t utc(const char* date, int hour, int minute = 0)
{
    return *azl::dateDays(date) * 86400 + hour * kHour + minute * 60;
}

int cest(int64_t) { return int(2 * kHour); }

azl::Gig night()
{
    return {"mini", "2026-10-16", 22 * 60, 5 * 60,
            {{"Opener", 23 * 60, 1}, {"Headliner", 3 * 60, 2}, {"Closer", 4 * 60, 1}, {"Ghost", 4 * 60 + 30, 9}}};
}

// Two decks and tonight's Gig, its last slot on a deck the composition does not have.
azl::Composition withNight()
{
    azl::Composition c = azl::defaultComposition();
    c.decks.push_back({2, "Peak", {{11, "P 1"}}});
    c.gigs = {azl::gigWindow(night())};
    c.cues = azl::lineupCues(night());
    for (size_t i = 0; i < c.cues.size(); ++i)
        c.cues[i].id = i + 1;
    return c;
}

Message deskStatus(azl::DocumentServer& server, bool loopback)
{
    std::mutex mutex;
    std::vector<Message> inbox;
    const azl::ClientId id = server.connect(
        [&](const Message& m) {
            std::lock_guard lock(mutex);
            inbox.push_back(m);
        },
        loopback);
    server.receive(id, {{"type", "hello"}, {"watch", false}});
    server.receive(id, {{"type", "desk"}, {"op", "status"}});
    server.disconnect(id);
    std::lock_guard lock(mutex);
    REQUIRE(inbox.size() == 1);
    return inbox[0];
}

// A harness engine on an ephemeral port with the desk server in front of it, on loopback.
struct Desk {
    explicit Desk(azl::Composition composition = withNight()) : harness(kSize, 15.0, {}, std::move(composition))
    {
        harness.setWallClock(double(utc("2026-10-17", 0, 30)), cest);
    }

    Message state()
    {
        const HttpReply reply = httpGet(server.port(), "/state");
        REQUIRE(reply.status == 200);
        return Message::parse(reply.body);
    }

    azl::EngineHarness harness;
    azl::WebSocketServer engine{harness.document(), 0};
    azl::DeskServer server{{.port = 0, .enginePort = engine.port(), .loopbackOnly = true}};
};

void switchDeck(azl::DocumentServer& server, uint64_t deck)
{
    Client hand(server);
    hand.send({{"type", "trigger"}, {"op", "deck"}, {"deck", deck}, {"by", "ui"}});
    Message m = hand.next();
    while (m["type"] == "switches")
        m = hand.next();
    REQUIRE(m["op"] == "selectDeck");
}

std::vector<std::string> flags(const Message& lineup, const char* flag)
{
    std::vector<std::string> named;
    for (const Message& slot : lineup)
        if (slot[flag] == true)
            named.push_back(slot["dj"]);
    return named;
}

// Runs the main run loop until `done` or the timeout.
bool spinUntil(const std::function<bool()>& done, double seconds = 15.0)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        if (done())
            return true;
    }
    return false;
}

std::string evaluate(WKWebView* view, NSString* script)
{
    auto answer = std::make_shared<std::pair<bool, std::string>>();
    [view evaluateJavaScript:script
           completionHandler:^(id value, NSError*) {
               if ([value isKindOfClass:[NSString class]])
                   answer->second = [(NSString*)value UTF8String];
               else if ([value isKindOfClass:[NSNumber class]])
                   answer->second = [[(NSNumber*)value stringValue] UTF8String];
               answer->first = true;
           }];
    spinUntil([&] { return answer->first; }, 5.0);
    return answer->second;
}

// A web view in no window, so nothing shows on the screen.
WKWebView* hiddenPage(uint16_t port)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    WKWebView* view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 390, 844)
                                         configuration:[[WKWebViewConfiguration alloc] init]];
    NSString* url = [NSString stringWithFormat:@"http://127.0.0.1:%u/", unsigned(port)];
    [view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
    return view;
}

} // namespace

TEST_CASE("desk: only LAN sources and the machine's own names get through")
{
    const std::vector<std::string> own = {"192.168.0.10"};
    const std::string name = "venue-mini.local";
    struct Row {
        const char* source;
        const char* host;
        bool loopback;
        bool allowed;
    };
    const Row rows[] = {
        {"192.168.0.20", "192.168.0.10:7480", false, true},
        {"10.1.2.3", "venue-mini.local:7480", false, true},
        {"172.20.0.5", "VENUE-MINI.LOCAL", false, true},
        {"169.254.10.10", "192.168.0.10", false, true},
        {"fe80::1%en0", "192.168.0.10", false, true},
        {"100.101.102.103", "192.168.0.10", false, false},
        {"100.64.0.1", "venue-mini.local", false, false},
        {"8.8.8.8", "192.168.0.10", false, false},
        {"172.32.0.1", "192.168.0.10", false, false},
        {"127.0.0.1", "192.168.0.10", false, false},
        {"::ffff:127.0.0.1", "localhost", false, false},
        {"192.168.0.20", "evil.example.com", false, false},
        {"192.168.0.20", "192.168.0.99:7480", false, false},
        {"192.168.0.20", "", false, false},
        {"127.0.0.1", "127.0.0.1:5000", true, true},
        {"127.0.0.1", "localhost", true, true},
        {"127.0.0.1", "evil.example.com", true, false},
        {"100.64.0.1", "127.0.0.1", true, false},
    };
    for (const Row& row : rows) {
        CAPTURE(row.source);
        CAPTURE(row.host);
        CHECK(!azl::deskRefusal(row.source, row.host, own, name, row.loopback).has_value() == row.allowed);
    }
    CHECK(azl::loopbackAddress("::1"));
    CHECK(azl::loopbackAddress("127.0.0.53"));
    CHECK_FALSE(azl::loopbackAddress("192.168.0.1"));
    CHECK(azl::privateAddress("fd12::1"));
    CHECK_FALSE(azl::privateAddress("2001:db8::1"));
    CHECK_FALSE(azl::privateAddress("not an address"));
    for (const std::string& address : azl::lanAddresses())
        CHECK(azl::privateAddress(address));
    CHECK(azl::bonjourName().ends_with(".local"));
}

TEST_CASE("desk: the desk protocol answers loopback clients only, with tonight's lineup and the live deck")
{
    TempDir root;
    azl::EngineHarness h(kSize, 15.0, root.path, withNight());
    h.setWallClock(double(utc("2026-10-17", 0, 30)), cest);
    CHECK(deskStatus(h.document(), false)["type"] == "error");

    const Message status = deskStatus(h.document(), true);
    REQUIRE(status["type"] == "desk");
    CHECK(status["enabled"] == true);
    CHECK(status["port"] == 7480);
    CHECK(status["host"].get<std::string>().ends_with(".local"));
    CHECK(status["deck"] == 1);
    const Message& tonight = status["tonight"];
    CHECK(tonight["date"] == "2026-10-16");
    CHECK(tonight["start"] == "2026-10-16T22:00:00.000+02:00");
    REQUIRE(tonight["lineup"].size() == 4);
    CHECK(tonight["lineup"][1]["time"] == "03:00");
    CHECK(tonight["lineup"][1]["deckName"] == "Peak");
    CHECK(flags(tonight["lineup"], "live") == std::vector<std::string>{"Opener"});
    CHECK(flags(tonight["lineup"], "next") == std::vector<std::string>{"Headliner"});
    CHECK(flags(tonight["lineup"], "played").empty());
    CHECK(flags(tonight["lineup"], "missing") == std::vector<std::string>{"Ghost"});
    CHECK(tonight["next"]["dj"] == "Headliner");
    CHECK(azl::readJsonFile(root.path / "desk.json") == Message{{"enabled", true}, {"port", 7480}});

    // An early changeover by hand: Headliner is live and the opener has played.
    switchDeck(h.document(), 2);
    const Message early = deskStatus(h.document(), true)["tonight"];
    CHECK(flags(early["lineup"], "live") == std::vector<std::string>{"Headliner"});
    CHECK(flags(early["lineup"], "played") == std::vector<std::string>{"Opener"});

    // Tonight is the next night within 12 hours, and none further ahead.
    h.setWallClock(double(utc("2026-10-16", 8, 30)), cest);
    CHECK(deskStatus(h.document(), true)["tonight"]["date"] == "2026-10-16");
    h.setWallClock(double(utc("2026-10-16", 7, 30)), cest);
    CHECK(deskStatus(h.document(), true)["tonight"].is_null());
}

TEST_CASE("desk: GET /state follows a hand switch and a slot shift; foreign hosts and paths are refused")
{
    Desk desk;
    Message state = desk.state();
    CHECK(state["connected"] == true);
    CHECK(state["deckName"] == azl::defaultComposition().decks[0].name);
    CHECK(flags(state["tonight"]["lineup"], "live") == std::vector<std::string>{"Opener"});
    CHECK(state["tonight"]["next"]["time"] == "03:00");

    switchDeck(desk.harness.document(), 2);
    state = desk.state();
    CHECK(state["deck"] == 2);
    CHECK(flags(state["tonight"]["lineup"], "live") == std::vector<std::string>{"Headliner"});

    azl::Gig late = night();
    late.lineup.pop_back();
    azl::shiftSlot(late, 1, 15);
    {
        Client director(desk.harness.document());
        ::apply(director, azl::gigEdit(late, desk.harness.document().document()));
    }
    state = desk.state();
    CHECK(state["tonight"]["lineup"].size() == 3);
    CHECK(state["tonight"]["lineup"][1]["time"] == "03:15");
    CHECK(state["tonight"]["next"]["time"] == "03:15");

    const HttpReply page = httpGet(desk.server.port(), "/");
    CHECK(page.status == 200);
    CHECK(page.body.find("No connection to the show machine") != std::string::npos);
    CHECK(page.body.find("http") == std::string::npos);
    CHECK(httpGet(desk.server.port(), "/state", "evil.example.com").status == 403);
    CHECK(httpGet(desk.server.port(), "/state", "localhost:1234").status == 200);
    CHECK(httpGet(desk.server.port(), "/secret").status == 404);
    CHECK(desk.server.respond("POST", "/state", "127.0.0.1", "127.0.0.1").status == 405);
}

TEST_CASE("desk: no Gig tonight shows the live deck and no lineup; an engine away answers 503")
{
    azl::Composition empty = azl::defaultComposition();
    std::optional<Desk> desk(std::in_place, empty);
    const Message state = desk->state();
    CHECK(state["tonight"].is_null());
    CHECK(state["deck"] == 1);

    // The engine goes away: the desk says so instead of hanging.
    azl::DeskServer orphan{{.port = 0, .enginePort = desk->engine.port(), .loopbackOnly = true}};
    const uint16_t port = orphan.port();
    desk.reset();
    const HttpReply away = httpGet(port, "/state");
    CHECK(away.status == 503);
    CHECK(Message::parse(away.body)["connected"] == false);
}

TEST_CASE("desk: a status with a name that is not UTF-8 still gets an answer")
{
    Desk desk;
    desk.harness.document().handle("desk", [](const Message&, const azl::DocumentServer::Sink& reply) {
        reply({{"type", "desk"}, {"op", "status"}, {"deck", 1}, {"name", "caf\xe9"}});
    });
    const HttpReply reply = httpGet(desk.server.port(), "/state");
    CHECK((reply.status == 200 || reply.status == 503));
    CHECK_NOTHROW(Message::parse(reply.body));
}

TEST_CASE("desk: the page draws a button per slot and marks the live one, in a web view no window shows")
{
    Desk desk;
    WKWebView* view = hiddenPage(desk.server.port());
    REQUIRE(spinUntil([&] { return evaluate(view, @"document.querySelectorAll('#lineup .slot').length") == "4"; }));
    CHECK(evaluate(view, @"document.querySelector('.slot.live .dj').textContent") == "Opener");
    CHECK(evaluate(view, @"document.querySelector('.slot.next .dj').textContent") == "Headliner");
    CHECK(evaluate(view, @"document.querySelector('.slot.missing .meta').textContent").find("deck missing") !=
          std::string::npos);
    CHECK(evaluate(view, @"document.getElementById('status').textContent").starts_with("Next: Headliner at 03:00"));
    CHECK(evaluate(view, @"String(document.body.classList.contains('offline'))") == "false");
    const bool windowless = view.window == nil;
    CHECK(windowless);

    Desk none(azl::defaultComposition());
    WKWebView* empty = hiddenPage(none.server.port());
    REQUIRE(spinUntil([&] { return evaluate(empty, @"String(document.body.classList.contains('empty'))") == "true"; }));
    CHECK(evaluate(empty, @"document.querySelectorAll('#lineup .slot').length") == "0");
    CHECK(evaluate(empty, @"document.getElementById('liveDeck').textContent").starts_with("Live: "));
}

TEST_CASE("desk: killing the desk process leaves the engine's frames running")
{
    azl::EngineHarness harness(kSize, 60.0, {}, withNight());
    azl::WebSocketServer engine(harness.document(), 0);
    const std::string binary = bundleBinary("nocturne-desk").string();
    const std::string enginePort = std::to_string(engine.port());
    std::vector<std::string> args = {binary, "--port", "0", "--loopback-only", "--engine-port", enginePort};
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    int out[2];
    REQUIRE(::pipe(out) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    pid_t pid = 0;
    REQUIRE(posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);

    std::string printed;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (printed.find("listening on 127.0.0.1:") == std::string::npos ||
           printed.find('\n', printed.find("listening on 127.0.0.1:")) == std::string::npos) {
        REQUIRE(std::chrono::steady_clock::now() < until);
        pollfd fd{out[0], POLLIN, 0};
        if (::poll(&fd, 1, 100) > 0) {
            char buffer[256];
            const ssize_t n = ::read(out[0], buffer, sizeof buffer);
            REQUIRE(n > 0);
            printed.append(buffer, size_t(n));
        }
    }
    ::close(out[0]);
    const uint16_t port = uint16_t(std::atoi(printed.c_str() + printed.find("127.0.0.1:") + 10));
    CHECK(httpGet(port, "/state").status == 200);

    harness.startLive(60.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    harness.stats().maxGapMicros = 0;
    const uint64_t before = harness.stats().rendered;
    REQUIRE(::kill(pid, SIGKILL) == 0);
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t after = harness.stats().rendered;
    const uint64_t gap = harness.stats().maxGapMicros;
    harness.stopLive();
    CHECK(WIFSIGNALED(status));
    CHECK(after - before >= 15);
    CHECK(gap < 250000);
    CHECK(httpGet(port, "/state").status == 0);
}
