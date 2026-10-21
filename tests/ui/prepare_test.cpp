#include "document/wall_clock.hpp"
#include "net/websocket.hpp"
#include "protocol/prepare.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"
#include "ui/client/engine_client.hpp"

#include <algorithm>
#include <deque>
#include <functional>

namespace {

using azl::ui::EngineClient;

constexpr azl::EngineConfig kSize{.width = 320, .height = 180};
constexpr double kFps = 15.0;

struct Notices : EngineClient::Listener {
    std::mutex mutex;
    std::vector<std::string> notices;
    std::vector<std::string> errors;

    void prepareNotice(const std::string& notice) override
    {
        std::lock_guard lock(mutex);
        notices.push_back(notice);
    }
    void engineError(const std::string& message) override
    {
        std::lock_guard lock(mutex);
        errors.push_back(message);
    }
};

// A Director's client wired to the venue's document in-process. What it sends waits in a queue until pump(), so it
// reaches the document off the document's lock; every message it ever sent is kept.
class Director {
public:
    explicit Director(azl::DocumentServer& venue)
        : venue_(venue), id_(venue.connect([this](const Message& m) { client.receive(m); }))
    {
        client.attached(
            [this](const Message& m) {
                std::lock_guard lock(mutex_);
                queue_.push_back(m);
                sent_.push_back(m);
            },
            false);
        pump();
        REQUIRE(client.synced());
    }
    ~Director() { leave(); }

    void pump()
    {
        for (;;) {
            Message next;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty())
                    return;
                next = std::move(queue_.front());
                queue_.pop_front();
            }
            venue_.receive(id_, next);
        }
    }

    // Commands sent from the `from`-th message on.
    size_t commandsSince(size_t from)
    {
        std::lock_guard lock(mutex_);
        return size_t(std::count_if(sent_.begin() + ptrdiff_t(from), sent_.end(),
                                    [](const Message& m) { return m.value("type", "") == "command"; }));
    }
    size_t sentCount()
    {
        std::lock_guard lock(mutex_);
        return sent_.size();
    }

    void leave()
    {
        if (left_)
            return;
        left_ = true;
        client.detached();
        venue_.disconnect(id_);
    }

    Notices notices;
    EngineClient client{notices};

private:
    azl::DocumentServer& venue_;
    azl::ClientId id_;
    std::mutex mutex_;
    std::deque<Message> queue_;
    std::vector<Message> sent_;
    bool left_ = false;
};

double venueOpacity(azl::EngineHarness& h)
{
    return h.document().document().layers[0].opacity;
}

bool hasPreparedCue(const azl::Composition& c)
{
    return std::any_of(c.cues.begin(), c.cues.end(), [](const azl::Cue& cue) { return cue.prepared.has_value(); });
}

int64_t utc(const char* date, int hour, int minute = 0, int second = 0)
{
    return *azl::dateDays(date) * 86400 + hour * 3600 + minute * 60 + second;
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

} // namespace

TEST_CASE("prepare: edits change only the local copy and send no commands; Send now applies them as one undo step")
{
    azl::EngineHarness h(kSize, kFps);
    Director director(h.document());
    director.client.prepare();
    REQUIRE(director.client.preparing());
    const size_t from = director.sentCount();

    director.client.beginGesture();
    for (double opacity : {0.8, 0.5, 0.3})
        director.client.edit(azl::SetLayerOpacity{1, opacity});
    director.client.endGesture();
    director.client.edit(azl::SetQuantise{8});
    director.client.edit(azl::SetTempo{150.0});
    director.client.undo();
    director.pump();

    CHECK(director.commandsSince(from) == 0);
    CHECK(venueOpacity(h) == doctest::Approx(1.0));
    CHECK(h.document().document().quantise == 4);
    CHECK(director.client.composition()->layers[0].opacity == doctest::Approx(0.3));
    CHECK(director.client.composition()->quantise == 8);
    CHECK(director.client.composition()->bpm == doctest::Approx(120.0));
    CHECK(director.client.history().undo == 2);
    CHECK(director.client.history().redo == 1);

    // Playback still goes live.
    director.client.selectDeck(1);
    director.pump();
    CHECK(director.commandsSince(from) == 0);

    director.client.sendPrepared(false);
    director.pump();
    CHECK_FALSE(director.client.preparing());
    CHECK(venueOpacity(h) == doctest::Approx(0.3));
    CHECK(h.document().document().quantise == 8);
    CHECK(h.document().document().bpm == doctest::Approx(120.0));
    CHECK(director.client.history().undo == 1);
    {
        std::lock_guard lock(director.notices.mutex);
        REQUIRE(director.notices.notices.size() == 1);
        CHECK(director.notices.notices[0] == "Sent 2 edits");
    }

    // One undo takes the whole change back.
    director.client.undo();
    director.pump();
    CHECK(venueOpacity(h) == doctest::Approx(1.0));
    CHECK(h.document().document().quantise == 4);
}

TEST_CASE("prepare: Discard leaves the venue untouched and shows it again")
{
    azl::EngineHarness h(kSize, kFps);
    Director director(h.document());
    const azl::Composition before = h.document().document();
    const uint64_t change = h.document().change();
    director.client.prepare();
    const size_t from = director.sentCount();
    director.client.edit(azl::SetLayerOpacity{1, 0.2});
    director.client.edit(azl::AddLayer{});
    director.pump();
    CHECK(director.client.composition()->layers.size() == 2);

    director.client.discardPrepared();
    director.pump();
    CHECK(director.commandsSince(from) == 0);
    CHECK(h.document().change() == change);
    CHECK(azl::documentJson(h.document().document()) == azl::documentJson(before));
    CHECK(director.client.composition()->layers.size() == 1);
    CHECK(director.client.composition()->layers[0].opacity == doctest::Approx(1.0));
}

TEST_CASE("prepare: a concurrent edit from a second client survives Send, and Send says what it left out")
{
    azl::EngineHarness h(kSize, kFps);
    Director director(h.document());
    director.client.prepare();
    director.client.edit(azl::SetLayerOpacity{1, 0.3});
    director.client.edit(azl::SetQuantise{8});
    director.pump();

    {
        Client other(h.document());
        ::apply(other, azl::SetLayerOpacity{1, 0.7});
        ::apply(other, azl::SetTempo{140.0});
    }
    // The copy is the venue with Prepare's edits on top: the other client's opacity wins already.
    CHECK(director.client.composition()->layers[0].opacity == doctest::Approx(0.7));
    CHECK(director.client.composition()->bpm == doctest::Approx(140.0));
    CHECK(director.client.composition()->quantise == 8);

    director.client.sendPrepared(false);
    director.pump();
    const azl::Composition venue = h.document().document();
    CHECK(venue.layers[0].opacity == doctest::Approx(0.7));
    CHECK(venue.bpm == doctest::Approx(140.0));
    CHECK(venue.quantise == 8);
    std::lock_guard lock(director.notices.mutex);
    REQUIRE(director.notices.notices.size() == 1);
    const std::string& notice = director.notices.notices[0];
    CHECK(notice.find("Sent 1 edits") == 0);
    CHECK(notice.find("left out setLayerOpacity (layers/#1/opacity): the venue changed it meanwhile") !=
          std::string::npos);
}

TEST_CASE("prepare: Send at the next deck switch lands on the switch on a fake clock, with the Director gone")
{
    azl::Composition show = azl::defaultComposition();
    show.decks.push_back({2, "Late", {{11, "Late 1"}}});
    // A deck switch at 23:00 every day, on the venue's clock (UTC here).
    show.cues.push_back({.id = 1, .name = "Late", .at = azl::CueAt{23 * 60}, .deck = 2});
    azl::EngineHarness h(kSize, kFps, {}, show);
    const auto zone = [](int64_t) { return 0; };
    h.setWallClock(double(utc("2026-10-16", 20)), zone);
    h.step();

    {
        Director director(h.document());
        director.client.prepare();
        director.client.edit(azl::SetLayerOpacity{1, 0.4});
        director.client.sendPrepared(true);
        director.pump();
        CHECK(hasPreparedCue(h.document().document()));
        CHECK(venueOpacity(h) == doctest::Approx(1.0));
    }

    h.step(int(kFps));
    CHECK(venueOpacity(h) == doctest::Approx(1.0));
    h.setWallClock(double(utc("2026-10-16", 22, 59, 58)), zone);
    for (int i = 0; i < int(5 * kFps) && h.document().document().deck != 2; ++i)
        h.step();
    const azl::Composition venue = h.document().document();
    REQUIRE(venue.deck == 2);
    CHECK(venue.layers[0].opacity == doctest::Approx(0.4));
    CHECK_FALSE(hasPreparedCue(venue));
}

TEST_CASE("prepare: the top bar's toggle shows Send and Discard only in Prepare")
{
    azl::EngineHarness h(kSize, kFps);
    azl::WebSocketServer server(h.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent main(session);
    main.setSize(1600, 950);
    REQUIRE(runUntil([&] { return session.client().synced(); }));
    azl::ui::TransportBar& bar = main.transport();
    CHECK_FALSE(bar.sendButton(false).isVisible());
    CHECK_FALSE(bar.discardButton().isVisible());

    bar.prepareButton().triggerClick();
    REQUIRE(runUntil([&] { return bar.sendButton(false).isVisible(); }));
    CHECK(bar.prepareButton().getToggleState());
    CHECK(bar.sendButton(true).isVisible());
    CHECK(bar.discardButton().isVisible());
    CHECK(bar.statusText().first.startsWith("Prepare"));

    session.client().edit(azl::SetLayerOpacity{1, 0.5});
    runUntil([] { return false; }, 0.3);
    CHECK(venueOpacity(h) == doctest::Approx(1.0));
    // With edits prepared the toggle does not drop them.
    bar.prepareButton().triggerClick();
    runUntil([] { return false; }, 0.2);
    CHECK(session.client().preparing());

    bar.sendButton(false).triggerClick();
    REQUIRE(runUntil([&] { return venueOpacity(h) == 0.5 && !bar.sendButton(false).isVisible(); }));
    CHECK_FALSE(bar.prepareButton().getToggleState());
}
