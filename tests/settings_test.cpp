#include "clips.hpp"
#include "engine/frame_guard.hpp"

#include <string>
#include <map>
#include <vector>

using azl::GuardProfile;

namespace {

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

// The first answer of that type and op, or an error.
Message request(Client& client, Message message, const std::string& op = {})
{
    const std::string type = message["type"];
    const std::string wanted = op.empty() ? message["op"].get<std::string>() : op;
    client.send(message);
    for (;;)
        if (Message m = client.next(); m["type"] == "error" || (m["type"] == type && m["op"] == wanted))
            return m;
}

Message settings(Client& client, Message fields = {})
{
    if (fields.is_null())
        return request(client, {{"type", "settings"}, {"op", "state"}});
    fields["type"] = "settings";
    fields["op"] = "set";
    return request(client, fields);
}

Message input(Client& client)
{
    return request(client, {{"type", "audio"}, {"op", "input"}});
}

// Frames of a constant GPU time until the guard first steps, or -1 within `frames`.
int framesToStep(double gpu, const azl::GuardThresholds& thresholds, int frames = 200)
{
    azl::FrameGuard guard;
    for (int n = 1; n <= frames; ++n)
        if (guard.step(gpu, 1.0, 4, thresholds))
            return n;
    return -1;
}

// The guard fed as the engine feeds it: each frame's GPU time through the engine's average (7/8 of the last), against
// a 60 fps budget, with nine layers to reduce, on the Smooth profile. Counts the frames at each level.
struct GuardRun {
    azl::FrameGuard guard;
    double average = 0.0;
    int frames = 0;
    std::map<size_t, int> framesAt;
    static constexpr double kBudget = 1.0 / 60.0;

    void frame(double gpu)
    {
        average = (average * 7.0 + gpu) / 8.0;
        guard.step(average, kBudget, 9, azl::guardThresholds(GuardProfile::Smooth, false, false));
        frames++;
        framesAt[guard.level]++;
    }
    void run(int frames, double gpu)
    {
        for (int i = 0; i < frames; ++i)
            frame(gpu);
    }
    // Frames until the level is `level`, or -1 within `within`.
    int framesUntil(size_t level, double gpu, int within)
    {
        for (int i = 1; i <= within; ++i) {
            frame(gpu);
            if (guard.level == level)
                return i;
        }
        return -1;
    }
};

} // namespace

TEST_CASE("settings: read, set and kept across a restart of the engine; bad values refused")
{
    TempDir root;
    {
        azl::EngineHarness harness(kSize, 60.0, root.path);
        Client client(harness.document());
        const Message state = settings(client);
        CHECK(state["profile"] == "smooth");
        CHECK(state["protectTransitions"] == false);
        CHECK(state["sections"] == Message::array({"audio", "displays", "performance", "caches"}));
        CHECK(state["overridden"].empty());
        CHECK(state["use"].contains("preloadMB"));

        Client other(harness.document());
        const Message set = settings(client, {{"profile", "sharp"}, {"protectTransitions", true}, {"clipPreviews", 3}});
        CHECK(set["profile"] == "sharp");
        const Message broadcast = other.next();
        CHECK(broadcast["type"] == "settings");
        CHECK(broadcast["op"] == "state");
        CHECK(broadcast["protectTransitions"] == true);

        CHECK(settings(client, {{"profile", "fast"}})["type"] == "error");
        CHECK(settings(client, {{"frameCacheMB", 1}})["type"] == "error");
        CHECK(request(client, {{"type", "settings"}, {"op", "frobnicate"}})["type"] == "error");
    }
    azl::EngineHarness restarted(kSize, 60.0, root.path);
    Client client(restarted.document());
    const Message state = settings(client);
    CHECK(state["profile"] == "sharp");
    CHECK(state["protectTransitions"] == true);
    CHECK(state["clipPreviews"] == 3);
}

TEST_CASE("settings: baking static passes is off by default, switches the engine's bake and is kept")
{
    TempDir root;
    {
        azl::EngineHarness harness(kSize, 60.0, root.path);
        Client client(harness.document());
        CHECK(settings(client)["bakeStatic"] == false);
        CHECK_FALSE(harness.engine().bakeStatic());
        CHECK(settings(client, {{"bakeStatic", true}})["bakeStatic"] == true);
        CHECK(harness.engine().bakeStatic());
        CHECK(settings(client, {{"bakeStatic", "on"}})["type"] == "error");
    }
    azl::EngineHarness restarted(kSize, 60.0, root.path);
    Client client(restarted.document());
    CHECK(settings(client)["bakeStatic"] == true);
    CHECK(restarted.engine().bakeStatic());
    CHECK(settings(client, {{"bakeStatic", false}})["bakeStatic"] == false);
    CHECK_FALSE(restarted.engine().bakeStatic());
}

TEST_CASE("settings: the render scale is 100 % by default, switches the engine's and is kept")
{
    TempDir root;
    {
        azl::EngineHarness harness(kSize, 60.0, root.path);
        Client client(harness.document());
        CHECK(settings(client)["renderScale"] == 100);
        CHECK(harness.engine().renderScale() == 100);
        CHECK(settings(client, {{"renderScale", 83}})["renderScale"] == 83);
        CHECK(harness.engine().renderScale() == 83);
        CHECK(settings(client, {{"renderScale", 75}})["type"] == "error");
    }
    azl::EngineHarness restarted(kSize, 60.0, root.path);
    Client client(restarted.document());
    CHECK(settings(client)["renderScale"] == 83);
    CHECK(restarted.engine().renderScale() == 83);
}

TEST_CASE("audio input: the engine's devices are listed, the one picked is open again after a restart and a replug")
{
    TempDir root;
    {
        azl::EngineHarness harness(kSize, 60.0, root.path);
        Client client(harness.document());
        const Message devices = request(client, {{"type", "audio"}, {"op", "devices"}});
        REQUIRE(devices["devices"].size() == 2);
        CHECK(devices["devices"][1]["uid"] == "fake-interface");
        CHECK(devices["devices"][1]["channels"] == 4);
        CHECK(input(client)["uid"] == "fake-mic");

        client.send({{"type", "audio"}, {"op", "select"}, {"device", "fake-interface"}, {"channels", {3, 4}}});
        const Message picked = input(client);
        CHECK(picked["uid"] == "fake-interface");
        CHECK(picked["state"] == "running");
        CHECK(picked["channels"] == Message::array({3, 4}));
    }
    azl::EngineHarness restarted(kSize, 60.0, root.path);
    Client client(restarted.document());
    Message open = input(client);
    CHECK(open["uid"] == "fake-interface");
    CHECK(open["channels"] == Message::array({3, 4}));

    restarted.audioInput().setDevices({{"fake-mic", "Fake Mic", 1, 48000.0, true}});
    CHECK(restarted.audioInput().status().uid == "fake-mic");
    CHECK(restarted.audioInput().status().state == "fallback");
    restarted.audioInput().setDevices(
        {{"fake-mic", "Fake Mic", 1, 48000.0, true}, {"fake-interface", "Fake Interface", 4, 48000.0, false}});
    const azl::AudioInputStatus back = restarted.audioInput().status();
    CHECK(back.uid == "fake-interface");
    CHECK(back.state == "running");
    CHECK(back.channels == std::vector<int>{3, 4});
}

TEST_CASE("displays: the engine machine's displays are listed and a screen moves to another with the screen op")
{
    azl::EngineHarness harness(kSize);
    harness.displays().attach({.id = "dell", .name = "DELL", .width = 1920, .height = 1080});
    harness.displays().attach({.id = "projector", .name = "Projector", .width = 1280, .height = 720});
    Client client(harness.document());
    REQUIRE(apply(client, azl::AddScreen{std::nullopt, {.name = "Screen 1", .display = "dell", .slices = {{}}}})
                ["type"] == "change");
    const Message list = request(client, {{"type", "outputs"}, {"op", "list"}});
    REQUIRE(list["displays"].size() == 2);
    CHECK(list["displays"][1]["name"] == "Projector");
    const uint64_t screen = harness.engine().composition()->screens.back().id;
    apply(client, azl::SetScreenDisplay{screen, "projector"});
    CHECK(harness.engine().composition()->screens.back().display == "projector");
}

TEST_CASE("frame guard: Smooth steps earlier than Sharp; Protect transitions steps at once during a transition")
{
    // Under the budget but near it: Smooth steps, Sharp never does.
    CHECK(framesToStep(0.95, azl::guardThresholds(GuardProfile::Smooth, false, false)) == 15);
    CHECK(framesToStep(0.95, azl::guardThresholds(GuardProfile::Sharp, false, false)) == -1);
    // Overrunning: both step, Sharp after twice as many frames.
    CHECK(framesToStep(1.2, azl::guardThresholds(GuardProfile::Smooth, false, false)) == 15);
    CHECK(framesToStep(1.2, azl::guardThresholds(GuardProfile::Sharp, false, false)) == 30);
    // A transition's extra passes bring the GPU near the budget: without protection a frame drops before any step;
    // with it the guard steps within a few frames, before the time reaches the budget.
    CHECK(framesToStep(0.85, azl::guardThresholds(GuardProfile::Sharp, false, true)) == -1);
    CHECK(framesToStep(0.85, azl::guardThresholds(GuardProfile::Smooth, false, true)) == -1);
    CHECK(framesToStep(0.85, azl::guardThresholds(GuardProfile::Sharp, true, true)) == 3);
    CHECK(framesToStep(0.85, azl::guardThresholds(GuardProfile::Smooth, true, false)) == -1);
}

TEST_CASE("frame guard: a load that spikes then settles under budget comes back to level 0 within seconds")
{
    constexpr double ms = 1e-3;
    GuardRun run;
    run.run(300, 7.5 * ms);
    CHECK(run.guard.level == 0);

    SUBCASE("one long frame, as a chain compile makes, is no step")
    {
        run.frame(60.0 * ms);
        run.run(60, 7.5 * ms);
        CHECK(run.guard.level == 0);
        CHECK(run.framesAt[1] == 0);
    }
    SUBCASE("a sustained overrun steps up a level at a time, and once it is over every level comes off in a second")
    {
        // The average crosses 90 % of the budget after eleven frames, then a level every 15.
        CHECK(run.framesUntil(1, 17.0 * ms, 60) == 26);
        run.run(46, 17.0 * ms);
        CHECK(run.guard.level == 4);
        // Each level takes 60 calm frames; the first waits a dozen for the average to settle under 75 % too.
        CHECK(run.framesUntil(0, 7.5 * ms, 300) <= 4 * 60 + 15);
        run.run(600, 7.5 * ms);
        CHECK(run.guard.level == 0);
    }
    SUBCASE("spikes every second while the frames are under budget delay the step down a little, never hold it up")
    {
        run.run(72, 17.0 * ms);
        REQUIRE(run.guard.level == 4);
        int frames = 0;
        while (run.guard.level > 0 && frames < 600) {
            run.frame(frames % 60 == 0 ? 60.0 * ms : 7.5 * ms);
            frames++;
        }
        CHECK(run.guard.level == 0);
        // A spike costs a few calm frames while the average is over 75 %; in a second there is one.
        CHECK(frames <= 4 * 60 + 60);
    }
}

TEST_CASE("frame guard: steady edits under budget keep the level at 0; a level that bounces back waits longer")
{
    constexpr double ms = 1e-3;
    SUBCASE("a script adding effects and editing parameters: a long frame every few, the rest under budget")
    {
        GuardRun run;
        for (int i = 0; i < 3600; ++i)
            run.frame(i % 4 == 0 ? 40.0 * ms : 7.5 * ms);
        CHECK(run.guard.level == 0);
        CHECK(run.framesAt[1] == 0);
    }
    SUBCASE("frames in the budget only with the level on: it comes back, and each bounce doubles the calm needed")
    {
        // Without the level 17 ms, with it 11 ms: under the 12.5 ms the step down waits for, so it bounces.
        GuardRun run;
        auto load = [&] { return run.guard.level > 0 ? 11.0 * ms : 17.0 * ms; };
        std::vector<int> onFor;
        int since = 0;
        for (int i = 0; i < 60 * 60 * 2; ++i) {
            const size_t was = run.guard.level;
            run.frame(load());
            since++;
            if (run.guard.level < was) {
                onFor.push_back(since);
                since = 0;
            }
        }
        // Each round is the frames at level 0 until the average is over 90 % for 15 (about 27), then at level 1 until
        // it is under 75 % (about 11) plus the calm needed: 60 the first time, then 120, 240, ...
        REQUIRE(onFor.size() >= 5);
        CHECK(onFor[0] <= 27 + 11 + 60 + 5);
        for (size_t i = 1; i < onFor.size(); ++i) {
            INFO("round " << i << ": " << onFor[i] << " after " << onFor[i - 1]);
            CHECK(onFor[i] - 38 >= 2 * (onFor[i - 1] - 38) - 12);
        }
        CHECK(run.guard.calmNeeded >= 60 * 16);
        // Most of the two minutes the frames were in the budget.
        CHECK(run.framesAt[1] > run.frames * 9 / 10);
    }
}

TEST_CASE("frame guard: the profile set over the protocol changes when the engine lowers resolution")
{
    const auto levelAfter = [](const char* profile, int frames) {
        azl::EngineHarness harness(kSize);
        Client client(harness.document());
        TempDir media;
        playMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
        REQUIRE(settings(client, {{"profile", profile}})["profile"] == profile);
        harness.engine().setFrameBudget(1e-6);
        harness.step(frames);
        return harness.stats().budgetLevel.load();
    };
    CHECK(levelAfter("smooth", 25) == 1);
    CHECK(levelAfter("sharp", 25) == 0);
    CHECK(levelAfter("sharp", 60) == 1);
}

TEST_CASE("caches: what they hold is reported, and lowering the preload bound frees the preloads at once")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    TempDir media;
    const auto add = [&](const char* name, const char* as) {
        const Message added = addMedia(client, copyMedia(name, media.path / as));
        REQUIRE(added["type"] == "library");
        return added["entry"]["hash"].get<std::string>();
    };
    const std::string a = add("count_240.mp4", "a.mp4");
    const std::string b = add("count_709_tv.mp4", "b.mp4");
    apply(client, azl::SetSlot{1, 1, azl::ClipRef{a, "a", {}}});
    apply(client, azl::SetSlot{1, 2, azl::ClipRef{b, "b", {}}});
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    REQUIRE(client.next()["type"] != "error");
    harness.step(30);
    const Message before = settings(client)["use"];
    CHECK(before["frameCacheMB"].get<uint64_t>() > 0);
    REQUIRE(before["preloadMB"].get<uint64_t>() > 0);

    REQUIRE(settings(client, {{"preloadMB", 0}})["preloadMB"] == 0);
    // The set is also broadcast as a state, with what the preloads held before their frames were dropped.
    for (Message m = client.next(); m["type"] != "settings" || m["op"] != "state"; m = client.next()) {
    }
    for (int frames = 0; frames < 60 && harness.engine().cacheUse().preloadBytes > 0; ++frames)
        harness.step();
    CHECK(harness.engine().cacheUse().preloadBytes == 0);
    CHECK(settings(client)["use"]["preloadMB"] == 0);
}

TEST_CASE("frame guard: its calm and bounce windows are seconds at the engine's rate, not frames")
{
    // At 30 fps the budget is a 30th: a level comes off after 30 calm frames, and a step up 61 frames after a step
    // down is no bounce; at 120 fps after 120 calm frames, and 240 frames later is still a bounce.
    const auto calmFrames = [](double fps) {
        azl::FrameGuard guard;
        const azl::GuardThresholds t = azl::guardThresholds(GuardProfile::Smooth, false, false);
        for (int i = 0; i < 100; ++i)
            guard.step(1.0 / fps, 1.0 / fps, 1, t);
        REQUIRE(guard.level == 1);
        int n = 0;
        while (guard.level == 1 && n < 1000) {
            guard.step(0.0, 1.0 / fps, 1, t);
            n++;
        }
        return n;
    };
    CHECK(calmFrames(30.0) == 30);
    CHECK(calmFrames(60.0) == 60);
    CHECK(calmFrames(120.0) == 120);

    const auto bounceAt = [](double fps, int after) {
        azl::FrameGuard guard;
        const azl::GuardThresholds t = azl::guardThresholds(GuardProfile::Smooth, false, false);
        for (int i = 0; i < 100; ++i)
            guard.step(1.0 / fps, 1.0 / fps, 1, t);
        while (guard.level == 1)
            guard.step(0.0, 1.0 / fps, 1, t);
        for (int i = 0; i < after; ++i)
            guard.step(0.0, 1.0 / fps, 1, t);
        while (guard.level == 0)
            guard.step(1.0 / fps, 1.0 / fps, 1, t);
        return guard.calmNeeded;
    };
    CHECK(bounceAt(30.0, 40) == 60);
    CHECK(bounceAt(30.0, 70) == 30);
    CHECK(bounceAt(120.0, 220) == 240);
    CHECK(bounceAt(120.0, 250) == 120);
}
