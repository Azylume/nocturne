#pragma once

#include "desk/desk_server.hpp"
#include "harness/engine_harness.hpp"
#include "http_get.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "venues/gig.hpp"

#include <mutex>
#include <string>
#include <vector>

// Tonight's Gig on a harness engine with the desk server in front of it, for the desk cases.
namespace {

constexpr int64_t kHour = 3600;
constexpr azl::EngineConfig kSize{.width = 320, .height = 180};

int64_t utc(const char* date, int hour, int minute = 0)
{
    return *azl::dateDays(date) * 86400 + hour * kHour + minute * 60;
}

[[maybe_unused]] int cest(int64_t) { return int(2 * kHour); }

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

[[maybe_unused]] Message deskStatus(azl::DocumentServer& server, bool loopback)
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

} // namespace
