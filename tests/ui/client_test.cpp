#include "effects/effect_registry.hpp"
#include "harness/engine_harness.hpp"
#include "net/websocket.hpp"
#include "ui/client/connection.hpp"
#include "ui/client/engine_client.hpp"

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace {

using azl::Message;
using azl::ui::EngineClient;

struct Recorder : EngineClient::Listener {
    std::mutex mutex;
    std::condition_variable changed;
    int documents = 0;
    int stills = 0;
    int previews = 0;
    int connections = 0;
    int clipPreviews = 0;
    std::vector<std::string> errors;

    void documentChanged() override { bump(documents); }
    void stillArrived(const std::string&) override { bump(stills); }
    void previewArrived(const std::string&) override { bump(previews); }
    void connectionChanged() override { bump(connections); }
    void clipPreviewsChanged() override { bump(clipPreviews); }
    void engineError(const std::string& message) override
    {
        std::lock_guard lock(mutex);
        errors.push_back(message);
    }

    void bump(int& counter)
    {
        std::lock_guard lock(mutex);
        ++counter;
        changed.notify_all();
    }

    template <typename F>
    bool waitFor(F&& condition, double seconds = 20.0)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::duration<double>(seconds), condition);
    }
};

// A client fed by hand: what it sends is recorded, what the engine would say is pushed in.
struct Fed {
    Recorder recorder;
    EngineClient client{recorder};
    std::vector<Message> sent;

    explicit Fed(bool local = false)
    {
        client.attached([this](const Message& m) { sent.push_back(m); }, local);
    }

    std::vector<Message> sentOfType(const std::string& type) const
    {
        std::vector<Message> found;
        for (const Message& m : sent)
            if (m.value("type", "") == type)
                found.push_back(m);
        return found;
    }

    std::vector<Message> sentWatches() const
    {
        std::vector<Message> found;
        for (const Message& m : sentOfType("preview"))
            if (m.value("op", "") == "watch")
                found.push_back(m);
        return found;
    }
};

Message snapshot(const azl::Composition& composition, uint64_t change)
{
    return {{"type", "snapshot"}, {"change", change}, {"composition", composition}};
}

Message change(const azl::Edit& edit, uint64_t number)
{
    Message m = azl::editToJson(edit);
    m["type"] = "change";
    m["change"] = number;
    return m;
}

azl::Composition gridComposition()
{
    azl::Composition composition = azl::defaultComposition();
    composition.layers[0].clips[1] = azl::ClipRef{std::string(64, 'a'), "one"};
    composition.layers[0].clips[2] = azl::ClipRef{std::string(64, 'b'), "two"};
    return composition;
}

} // namespace

TEST_CASE("the client says hello, asks for the library and follows the document from snapshot and changes")
{
    Fed fed;
    REQUIRE(fed.sent.size() >= 2);
    CHECK(fed.sent[0]["type"] == "hello");
    CHECK(fed.sentOfType("library").at(0)["op"] == "list");
    CHECK_FALSE(fed.client.synced());

    fed.client.receive(snapshot(gridComposition(), 7));
    CHECK(fed.client.synced());
    CHECK(fed.client.composition()->layers[0].clips.size() == 2);
    fed.client.receive(change(azl::SetLayerOpacity{1, 0.25}, 8));
    CHECK(fed.client.composition()->layers[0].opacity == doctest::Approx(0.25));
    CHECK(fed.client.change() == 8);
    CHECK(fed.recorder.documents == 2);

    // A missing change makes it ask for a fresh snapshot.
    fed.sent.clear();
    fed.client.receive(change(azl::SetLayerOpacity{1, 0.5}, 10));
    REQUIRE(fed.sent.size() == 1);
    CHECK(fed.sent[0]["type"] == "snapshot");
    CHECK(fed.client.composition()->layers[0].opacity == doctest::Approx(0.25));
}

TEST_CASE("edits go out as commands, playback as trigger and clock messages")
{
    Fed fed;
    fed.sent.clear();
    fed.client.edit(azl::SetLayerBypass{1, true});
    fed.client.undo();
    fed.client.triggerClip(1, 2);
    fed.client.triggerColumn(3);
    fed.client.selectDeck(1);
    fed.client.cancelTrigger(1);
    fed.client.tap();
    fed.client.cancelTrigger();
    REQUIRE(fed.sent.size() == 8);
    CHECK(fed.sent[0] == Message{{"type", "command"}, {"op", "setLayerBypass"}, {"layer", 1}, {"bypass", true}});
    CHECK(fed.sent[1] == Message{{"type", "command"}, {"op", "undo"}});
    CHECK(fed.sent[2] == Message{{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 2}});
    CHECK(fed.sent[3] == Message{{"type", "trigger"}, {"op", "column"}, {"column", 3}});
    CHECK(fed.sent[4] == Message{{"type", "trigger"}, {"op", "deck"}, {"deck", 1}, {"by", "ui"}});
    CHECK(fed.sent[5] == Message{{"type", "trigger"}, {"op", "cancel"}, {"layer", 1}});
    CHECK(fed.sent[6] == Message{{"type", "clock"}, {"op", "tap"}});
    CHECK(fed.sent[7] == Message{{"type", "trigger"}, {"op", "cancel"}});
}

TEST_CASE("a trigger shows as queued until the clock says the layer plays it")
{
    Fed fed;
    fed.client.receive(snapshot(gridComposition(), 1));
    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", 2.0}, {"tempo", 0.0},
                        {"clips", {{{"layer", 1}, {"column", 1}, {"frame", 0}, {"beats", 4}, {"visible", true}}}}});
    fed.client.receive(change(azl::TriggerClip{1, 1, 0.0}, 2));
    CHECK(fed.client.playing(1, 1));
    CHECK_FALSE(fed.client.queued(1, 1));

    // Triggered for beat 4 while the clock (frozen at tempo 0) is at beat 2.
    fed.client.receive(change(azl::TriggerClip{1, 2, 4.0}, 3));
    CHECK(fed.client.queued(1, 2));
    CHECK(fed.client.playing(1, 1));
    CHECK_FALSE(fed.client.playing(1, 2));

    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", 4.5}, {"tempo", 0.0},
                        {"clips", {{{"layer", 1}, {"column", 2}, {"frame", 3}, {"beats", 4}, {"visible", true}}}}});
    CHECK_FALSE(fed.client.queued(1, 2));
    CHECK(fed.client.playing(1, 2));

    // Autopilot steps away from the triggered cell: the clock shows another column, and the cell is not queued.
    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", 9.0}, {"tempo", 0.0},
                        {"clips", {{{"layer", 1}, {"column", 1}, {"frame", 0}, {"beats", 4}, {"visible", true}}}}});
    CHECK_FALSE(fed.client.queued(1, 2));

    // A trigger for a beat already reached is queued until the next clock state.
    fed.client.receive(change(azl::TriggerClip{1, 2, 8.0}, 4));
    CHECK(fed.client.queued(1, 2));
    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", 9.5}, {"tempo", 0.0},
                        {"clips", {{{"layer", 1}, {"column", 1}, {"frame", 0}, {"beats", 4}, {"visible", true}}}}});
    CHECK_FALSE(fed.client.queued(1, 2));
}

TEST_CASE("a clip's still is asked for once and kept")
{
    Fed fed;
    fed.sent.clear();
    CHECK(fed.client.still("aaaa").empty());
    CHECK(fed.client.still("aaaa").empty());
    REQUIRE(fed.sent.size() == 1);
    CHECK(fed.sent[0]["op"] == "still");
    CHECK(fed.sent[0]["hash"] == "aaaa");

    fed.client.receive({{"type", "library"}, {"op", "still"}, {"hash", "aaaa"},
                        {"jpeg", Message::binary({0xFF, 0xD8, 1, 2})}});
    CHECK(fed.client.still("aaaa").size() == 4);
    CHECK(fed.recorder.stills == 1);
    CHECK(fed.sent.size() == 1);
}

TEST_CASE("previews are watched by JPEG from a remote engine and by surface from a local one, again after a reconnect")
{
    Fed remote(false);
    remote.client.watchPreviews({"composition", "layer:1"});
    auto watches = remote.sentWatches();
    REQUIRE(watches.size() == 2);
    CHECK(watches.back()["via"] == "jpeg");
    CHECK(watches.back()["sources"] == Message{"composition", "layer:1"});

    remote.client.receive({{"type", "preview"}, {"op", "frame"}, {"source", "composition"}, {"frame", 5},
                           {"width", 320}, {"height", 180}, {"jpeg", Message::binary({0xFF, 0xD8})}});
    auto picture = remote.client.preview("composition");
    REQUIRE(picture);
    CHECK(picture->frame == 5);
    CHECK(picture->jpeg.size() == 2);

    remote.client.detached();
    std::vector<Message> again;
    remote.client.attached([&](const Message& m) { again.push_back(m); }, true);
    bool watched = false;
    for (const Message& m : again)
        if (m["type"] == "preview" && m["op"] == "watch") {
            watched = true;
            CHECK(m["via"] == "surface");
            CHECK(m["sources"] == Message{"composition", "layer:1"});
        }
    CHECK(watched);
}

TEST_CASE("clip previews are a watch of their own, naming the selected cell, kept beside the monitors' watch and sent again after a reconnect")
{
    Fed remote(false);
    remote.client.watchPreviews({"composition"});
    remote.client.watchClipPreviews({"aaaa", "bbbb"});
    const auto watches = remote.sentWatches();
    REQUIRE(watches.size() == 3);
    const Message& clips = watches.back();
    CHECK(clips["watch"] == "clips");
    CHECK(clips["sources"] == Message{"clip:aaaa", "clip:bbbb"});
    CHECK(clips["via"] == "jpeg");
    CHECK(clips["width"] == EngineClient::kClipWidth);
    CHECK(clips["fps"] == EngineClient::kClipFps);

    remote.client.receive({{"type", "preview"}, {"op", "frame"}, {"source", "clip:aaaa"}, {"frame", 2},
                           {"width", 160}, {"height", 90}, {"jpeg", Message::binary({0xFF, 0xD8})}});
    REQUIRE(remote.client.preview("clip:aaaa"));
    remote.client.watchPreviews({"composition", "layer:1"});
    CHECK(remote.client.preview("clip:aaaa"));
    remote.client.watchClipPreviews({"bbbb"}, std::pair<uint64_t, uint64_t>{1, 3});
    CHECK_FALSE(remote.client.preview("clip:aaaa"));
    CHECK(remote.sentWatches().back()["clip"] == Message{{"layer", 1}, {"column", 3}});

    remote.client.detached();
    std::vector<Message> again;
    remote.client.attached([&](const Message& m) { again.push_back(m); }, true);
    std::vector<Message> sentWatches;
    for (const Message& m : again)
        if (m["type"] == "preview" && m["op"] == "watch")
            sentWatches.push_back(m);
    REQUIRE(sentWatches.size() == 2);
    CHECK_FALSE(sentWatches[0].contains("watch"));
    CHECK(sentWatches[1]["watch"] == "clips");
    CHECK(sentWatches[1]["via"] == "surface");
    CHECK(sentWatches[1]["sources"] == Message{"clip:bbbb"});
    CHECK(sentWatches[1]["clip"]["column"] == 3);
}

TEST_CASE("live clip previews: the client asks their state on attach, follows the engine's state and sends a toggle")
{
    Fed remote(false);
    const auto asked = remote.sentOfType("preview");
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.front() == Message{{"type", "preview"}, {"op", "clips"}});
    CHECK_FALSE(remote.client.clipPreviews());

    remote.client.receive({{"type", "preview"}, {"op", "clips"}, {"on", true}});
    CHECK(remote.client.clipPreviews());
    CHECK(remote.recorder.clipPreviews == 1);

    remote.client.setClipPreviews(false);
    CHECK(remote.sent.back() == Message{{"type", "preview"}, {"op", "clips"}, {"on", false}});
    // The engine's answer is what counts.
    CHECK(remote.client.clipPreviews());
    remote.client.receive({{"type", "preview"}, {"op", "clips"}, {"on", false}});
    CHECK_FALSE(remote.client.clipPreviews());
}

TEST_CASE("media dropped from this machine is added to the engine's library, and each entry comes back to its drop")
{
    Fed fed(true);
    fed.sent.clear();
    std::vector<std::string> added;
    fed.client.addMedia({"/tmp/a/../clip.mp4"}, [&](const azl::ui::LibraryItem& item) { added.push_back(item.hash); });
    REQUIRE(fed.sent.size() == 1);
    CHECK(fed.sent[0]["path"] == "/tmp/clip.mp4");
    fed.client.receive({{"type", "library"},
                        {"op", "add"},
                        {"entry", {{"hash", "cccc"}, {"name", "clip.mp4"}, {"path", "/tmp/clip.mp4"}, {"status", "ok"}}}});
    CHECK(added == std::vector<std::string>{"cccc"});
    CHECK(fed.client.library().back().hash == "cccc");
}

TEST_CASE("over a real WebSocket, the client syncs, triggers, and reads previews from a local engine's surfaces")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    azl::WebSocketServer server(harness.document(), 0);
    harness.step();
    Recorder recorder;
    EngineClient client(recorder);
    client.watchPreviews({"composition"});
    azl::ui::Connection connection(client, "127.0.0.1", server.port());

    REQUIRE(recorder.waitFor([&] { return client.synced(); }));
    CHECK(client.local());
    const uint64_t column = client.composition()->decks[0].columns[1].id;
    client.triggerColumn(column);
    REQUIRE(recorder.waitFor([&] {
        const auto& playing = client.composition()->layers[0].playing;
        return playing && playing->column == column;
    }));

    std::thread stepper([&] {
        for (int i = 0; i < 200 && !client.preview("composition"); ++i) {
            harness.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const bool previewed = recorder.waitFor([&] { return client.preview("composition").has_value(); });
    stepper.join();
    REQUIRE(previewed);
    const auto picture = *client.preview("composition");
    CHECK(picture.width == EngineClient::kSurfaceWidth);
    CHECK(picture.bgra.size() == size_t(picture.width) * picture.height * 4);
    CHECK(picture.jpeg.empty());
}

TEST_CASE("the connection reaches an engine by host name and says why a name does not resolve")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    azl::WebSocketServer server(harness.document(), 0);
    Recorder recorder;
    EngineClient client(recorder);
    {
        azl::ui::Connection connection(client, "localhost", server.port());
        REQUIRE(recorder.waitFor([&] { return client.synced(); }));
        CHECK(client.local());
        CHECK(connection.lastError().empty());
    }

    Recorder lost;
    EngineClient nowhere(lost);
    azl::ui::Connection connection(nowhere, "nocturne-no-such-host.invalid", server.port());
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (connection.lastError().empty() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(connection.lastError().find("cannot resolve host \"nocturne-no-such-host.invalid\"") != std::string::npos);
    CHECK_FALSE(nowhere.connected());
}

TEST_CASE("edits between beginGesture and endGesture carry one gesture, a new gesture another")
{
    Fed fed;
    fed.sent.clear();
    fed.client.edit(azl::SetLayerOpacity{1, 0.1});
    fed.client.beginGesture();
    fed.client.edit(azl::SetLayerOpacity{1, 0.2});
    fed.client.edit(azl::SetLayerOpacity{1, 0.3});
    fed.client.endGesture();
    fed.client.beginGesture();
    fed.client.edit(azl::SetLayerOpacity{1, 0.4});
    fed.client.endGesture();
    fed.client.edit(azl::SetLayerOpacity{1, 0.5});
    REQUIRE(fed.sent.size() == 5);
    CHECK_FALSE(fed.sent[0].contains("gesture"));
    CHECK(fed.sent[1]["gesture"] == fed.sent[2]["gesture"]);
    CHECK(fed.sent[3]["gesture"] != fed.sent[1]["gesture"]);
    CHECK_FALSE(fed.sent[4].contains("gesture"));
}

TEST_CASE("the client keeps the engine's history, presets, Macro Sets and live modulation")
{
    Fed fed;
    fed.client.receive(snapshot(gridComposition(), 1));
    CHECK(fed.client.history().undo == 0);
    Message changed = change(azl::SetLayerOpacity{1, 0.5}, 2);
    changed["history"] = {{"undo", 3}, {"redo", 1}, {"undoOp", "setLayerOpacity"}, {"redoOp", "addEffect"}};
    fed.client.receive(changed);
    const azl::ui::History history = fed.client.history();
    CHECK(history.undo == 3);
    CHECK(history.redo == 1);
    CHECK(history.undoOp == "setLayerOpacity");
    CHECK(history.redoOp == "addEffect");

    CHECK(fed.sentOfType("preset").at(0)["op"] == "list");
    fed.client.receive({{"type", "preset"}, {"op", "list"}, {"presets", {{"trails", {{"soft", {{"feedback", 0.5}}}}}}}});
    CHECK(fed.client.presets()["trails"]["soft"]["feedback"] == 0.5);
    fed.sent.clear();
    fed.client.savePreset(4, "hard");
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "save"}, {"effect", 4}, {"name", "hard"}});
    fed.client.receive({{"type", "preset"}, {"op", "save"}, {"effectType", "trails"}, {"name", "hard"}});
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "list"}});
    fed.client.applyPreset(4, "soft");
    CHECK(fed.sent.back()["op"] == "apply");
    fed.client.importPresets();
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "import"}});
    fed.client.receive({{"type", "preset"}, {"op", "import"}, {"presets", Message::object()}});
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "list"}});
    // An older engine's list counts as the user's own; a newer one names each preset's group.
    REQUIRE(fed.client.presetEntries().size() == 1);
    CHECK(fed.client.presetEntries()[0].group == "mine");
    fed.client.receive({{"type", "preset"},
                        {"op", "list"},
                        {"presets", {{"trails", {{"soft", {{"feedback", 0.5}}}}}}},
                        {"entries",
                         {{{"effectType", "trails"}, {"name", "soft"}, {"group", "builtin"}, {"params", {{"feedback", 0.2}}}},
                          {{"effectType", "trails"}, {"name", "soft"}, {"group", "mine"}, {"params", {{"feedback", 0.5}}}}}}});
    REQUIRE(fed.client.presetEntries().size() == 2);
    CHECK(fed.client.presetEntries()[0].group == "builtin");
    CHECK(fed.client.presetEntries()[0].params.at("feedback") == 0.2);
    fed.client.applyPreset(4, "soft", "builtin");
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "apply"}, {"effect", 4}, {"name", "soft"}, {"group", "builtin"}});
    fed.client.renamePreset("trails", "soft", "softer");
    CHECK(fed.sent.back() ==
          Message{{"type", "preset"}, {"op", "rename"}, {"effectType", "trails"}, {"name", "soft"}, {"to", "softer"}});
    fed.client.deletePreset("trails", "softer");
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "delete"}, {"effectType", "trails"}, {"name", "softer"}});
    fed.client.receive({{"type", "preset"}, {"op", "delete"}, {"effectType", "trails"}, {"name", "softer"}});
    CHECK(fed.sent.back() == Message{{"type", "preset"}, {"op", "list"}});

    const Message sets = Message::object({{"Techno", Message::object({{"macros", 2}})}, {"Ambient", Message::object()}});
    fed.client.receive({{"type", "modulation"}, {"op", "list"}, {"sets", sets}});
    CHECK(fed.client.macroSets() == std::vector<std::string>{"Ambient", "Techno"});
    fed.client.previewMacroSet("Techno");
    CHECK(fed.sent.back() == Message{{"type", "modulation"}, {"op", "preview"}, {"name", "Techno"}});
    CHECK_FALSE(fed.client.macroSetPreview());
    fed.client.receive({{"type", "modulation"}, {"op", "preview"}, {"name", "Techno"},
                        {"items", {{{"key", "m0"}, {"kind", "macro"}, {"label", "Energy"}, {"status", "ok"}},
                                   {{"key", "m1"}, {"kind", "macro"}, {"label", "Strobe"}, {"status", "new"}}}},
                        {"notes", {"Fade is not in the set: it keeps its targets, without modulators"}},
                        {"replaces", {{"macros", 1}, {"modulators", 2}}}});
    const auto preview = fed.client.macroSetPreview();
    REQUIRE(preview);
    CHECK(preview->name == "Techno");
    CHECK(preview->modulators == 2);
    REQUIRE(preview->items.size() == 2);
    CHECK(preview->items[1].status == "new");
    CHECK(preview->notes == std::vector<std::string>{"Fade is not in the set: it keeps its targets, without modulators"});
    fed.client.loadMacroSet("Techno", {"d0"});
    CHECK(fed.sent.back() == Message{{"type", "modulation"}, {"op", "load"}, {"name", "Techno"}, {"drop", {"d0"}}});

    fed.client.receive({{"type", "modulation"},
                        {"op", "state"},
                        {"macros", {{{"id", 1}, {"value", 0.75}}}},
                        {"modulators", {{{"id", 2}, {"value", 0.25}}}},
                        {"params", {{{"effect", 4}, {"param", "amount"}, {"value", 2.5}}}}});
    const azl::ui::ModulationState state = fed.client.modulation();
    CHECK(state.macros.at(1) == 0.75);
    CHECK(state.modulators.at(2) == 0.25);
    CHECK(state.params.at(std::make_tuple(uint64_t(0), uint64_t(4), std::string("amount"))) == 2.5);

    fed.client.receive({{"type", "audio"}, {"op", "state"}, {"live", true},
                        {"values", Message::object({{"kick", 0.8}})}});
    CHECK(fed.client.audio().values.at("kick") == doctest::Approx(0.8));
    fed.client.receive({{"type", "error"}, {"message", "unknown message type audio"}});
    CHECK(fed.recorder.errors.empty());
}

TEST_CASE("the outputs list's mirroring and the unmirror setting reach the client; a displays notice is passed on")
{
    struct Notices : EngineClient::Listener {
        std::vector<std::string> notices;
        void outputsNotice(const std::string& notice) override { notices.push_back(notice); }
    } listener;
    EngineClient client(listener);
    std::vector<Message> sent;
    client.attached([&](const Message& m) { sent.push_back(m); }, false);
    CHECK(client.unmirror());

    client.receive({{"type", "outputs"},
                    {"op", "list"},
                    {"displays",
                     {{{"id", "a"}, {"name", "A"}, {"width", 1920}, {"height", 1080}},
                      {{"id", "b"}, {"name", "B"}, {"width", 1920}, {"height", 1080}, {"mirrorOf", "a"}}}},
                    {"outputs", Message::array()},
                    {"unmirror", false}});
    REQUIRE(client.displays().size() == 2);
    CHECK(client.displays()[0].mirrorOf.empty());
    CHECK(client.displays()[1].mirrorOf == "a");
    CHECK_FALSE(client.unmirror());

    sent.clear();
    client.setUnmirror(true);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == Message{{"type", "outputs"}, {"op", "unmirror"}, {"on", true}});
    client.receive({{"type", "outputs"}, {"op", "notice"}, {"kind", "rebound"}, {"text", "Screen 2 took back its display"}});
    CHECK(listener.notices == std::vector<std::string>{"Screen 2 took back its display"});
}

TEST_CASE("the record button's client side: start and stop go out, the state comes back, a notice is passed on")
{
    struct Notices : EngineClient::Listener {
        int changes = 0;
        std::vector<std::string> notices;
        void recordChanged() override { ++changes; }
        void recordNotice(const std::string& notice) override { notices.push_back(notice); }
    } listener;
    EngineClient client(listener);
    std::vector<Message> sent;
    client.attached([&](const Message& m) { sent.push_back(m); }, false);
    CHECK(std::count(sent.begin(), sent.end(), Message{{"type", "record"}, {"op", "state"}}) == 1);

    sent.clear();
    client.record(true);
    client.record(false);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == Message{{"type", "record"}, {"op", "start"}});
    CHECK(sent[1] == Message{{"type", "record"}, {"op", "stop"}});

    client.receive({{"type", "record"}, {"op", "state"}, {"recording", true}, {"codec", "hevc"},
                    {"target", "composition"}, {"file", "/tmp/a.mov"}, {"frames", 120}, {"dropped", 0}});
    CHECK(client.record().recording);
    CHECK(client.record().file == "/tmp/a.mov");
    CHECK(client.record().frames == 120);
    client.receive({{"type", "record"}, {"op", "state"}, {"recording", true}, {"space", "low"}, {"freeMB", 30000},
                    {"stopsAtMB", 20480}});
    CHECK(client.record().space == "low");
    CHECK(client.record().stopsAtMB == 20480);
    client.receive({{"type", "record"}, {"op", "stopped"}, {"reason", "disk full"},
                    {"notice", "Recording stopped: the disk is full. The show goes on."}});
    CHECK_FALSE(client.record().recording);
    CHECK(client.record().space == "full");
    CHECK(listener.notices == std::vector<std::string>{"Recording stopped: the disk is full. The show goes on."});
    CHECK(listener.changes == 3);
}

TEST_CASE("what the client cannot understand does not crash it: it says why and resyncs from a snapshot")
{
    Fed fed;
    fed.client.receive(snapshot(azl::defaultComposition(), 1));
    REQUIRE(fed.client.synced());

    auto resyncs = [&](const Message& message) {
        fed.sent.clear();
        fed.recorder.errors.clear();
        fed.client.receive(message);
        CHECK_FALSE(fed.client.synced());
        CHECK(fed.sentOfType("snapshot").size() == 1);
        CHECK(fed.recorder.errors.size() == 1);
    };
    resyncs(change(azl::AddEffect{1, std::nullopt, {0, "trails", {{"fromTheFuture", 1.0}}}}, 2));

    azl::Composition newer = azl::defaultComposition();
    newer.layers[0].effects.push_back({7, "trails", {{"fromTheFuture", 1.0}}});
    fed.client.receive(snapshot(newer, 2));
    CHECK(fed.client.synced());
    CHECK(fed.client.composition()->layers[0].effects.size() == 1);

    resyncs(change(azl::AddEffect{1, std::nullopt, {0, "hologram", {}}}, 3));
    fed.client.receive(snapshot(newer, 3));
    resyncs({{"type", "change"}, {"change", 4}, {"op", "fromTheFuture"}});

    fed.recorder.errors.clear();
    fed.client.receive({{"type", "hologram"}});
    fed.client.receive({{"type", 7}});
    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", "soon"}});
    CHECK(fed.recorder.errors.size() == 2);
}

TEST_CASE("an engine with another effect registry shows which side to update")
{
    CHECK(azl::msg::hello()["effects"] == azl::effectRegistryVersion());
    CHECK_FALSE(azl::msg::hello()["build"].get<std::string>().empty());

    Fed fed;
    Message engine = snapshot(azl::defaultComposition(), 1);
    engine["effects"] = azl::effectRegistryVersion();
    engine["build"] = "99999999999999-later";
    fed.client.receive(engine);
    CHECK(fed.client.versionSkew().empty());

    engine["effects"] = "0000000000000000";
    fed.client.receive(engine);
    CHECK(fed.client.versionSkew() == "UI older than the engine: update it");
    CHECK(fed.client.synced());

    engine["build"] = "19700101000000-earlier";
    fed.client.receive(engine);
    CHECK(fed.client.versionSkew() == "UI newer than the engine: update the engine");
}

TEST_CASE("the clock message carries the frame guard's level")
{
    Fed fed;
    CHECK(fed.client.clock().budgetLevel == 0);
    fed.client.receive({{"type", "clock"}, {"op", "state"}, {"beat", 1.0}, {"budgetLevel", 2}});
    CHECK(fed.client.clock().budgetLevel == 2);
}

TEST_CASE("a load's notice of dropped removed effects is passed on; a load without one passes nothing")
{
    struct Notices : EngineClient::Listener {
        std::vector<std::string> notices;
        void compositionNotice(const std::string& notice) override { notices.push_back(notice); }
    } listener;
    EngineClient client(listener);
    const std::string notice = "Bloom and Classic Bloom are removed; dropped from this show: Bloom (effect 9) on base";
    client.receive({{"type", "composition"}, {"op", "load"}, {"path", "/shows/a.json"}, {"warnings", Message::array()}});
    CHECK(listener.notices.empty());
    client.receive({{"type", "composition"}, {"op", "load"}, {"path", "/shows/b.json"}, {"warnings", Message::array()},
                    {"notice", notice}});
    CHECK(listener.notices == std::vector<std::string>{notice});
}

TEST_CASE("the modulation state's running range of an Auto range modulator reaches the client")
{
    Fed fed;
    fed.client.receive({{"type", "modulation"},
                        {"op", "state"},
                        {"modulators", {{{"id", 2}, {"value", 0.4}, {"range", {0.05, 0.3}}}, {{"id", 3}, {"value", 0.1}}}}});
    const azl::ui::ModulationState state = fed.client.modulation();
    CHECK(state.modulators.at(2) == doctest::Approx(0.4));
    REQUIRE(state.ranges.contains(2));
    CHECK(state.ranges.at(2).first == doctest::Approx(0.05));
    CHECK(state.ranges.at(2).second == doctest::Approx(0.3));
    CHECK_FALSE(state.ranges.contains(3));
}

TEST_CASE("a change's notice of what went with a removed effect is passed on, after the document it changed")
{
    struct Notices : Recorder {
        std::vector<std::string> notices;
        void compositionNotice(const std::string& notice) override
        {
            notices.push_back(notice);
            CHECK(documents == 3);
        }
    } listener;
    EngineClient client(listener);
    client.attached([](const Message&) {}, false);
    azl::Composition composition = gridComposition();
    composition.layers[0].effects = {{1, "reveal", {{"progress", 1.0}}}};
    client.receive(snapshot(composition, 7));
    client.receive(change(azl::SetLayerOpacity{1, 0.25}, 8));
    CHECK(listener.notices.empty());

    const std::string notice = "Gone with what the edit removed: modulator Modulator 2 on effect 1 evolve";
    Message removed = change(azl::RemoveEffect{1}, 9);
    removed["notice"] = notice;
    client.receive(removed);
    CHECK(client.composition()->layers[0].effects.empty());
    CHECK(listener.notices == std::vector<std::string>{notice});
}
