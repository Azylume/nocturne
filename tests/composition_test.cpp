#include "document/composition_file.hpp"
#include "effects/effect_registry.hpp"
#include "media/composition_media.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <fstream>
#include <set>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace {

using azl::ClipRef;

constexpr azl::EngineConfig kSize{.width = 320, .height = 180};

azl::DisplayInfo display(const std::string& id)
{
    return {id, id, 1920, 1080, 60.0};
}

Message composition(Client& client, const std::string& op, const fs::path& file, const fs::path& root = {},
                    bool remapDisplays = false)
{
    Message m{{"type", "composition"}, {"op", op}, {"path", file.string()}};
    if (!root.empty())
        m["mediaRoot"] = root.string();
    if (remapDisplays)
        m["remapDisplays"] = true;
    client.send(m);
    const Message reply = client.next();
    REQUIRE_MESSAGE(reply["type"] == "composition", reply.dump());
    if (op == "load")
        REQUIRE(client.next()["op"] == "loadComposition");
    return reply;
}

Message snapshot(Client& client)
{
    client.send(msg::snapshotRequest());
    return client.next()["composition"];
}

void clearBeats(Message& layers)
{
    for (Message& layer : layers) {
        if (layer["playing"].is_object())
            layer["playing"]["beat"] = nullptr;
        if (layer.contains("layers"))
            clearBeats(layer["layers"]);
    }
}

Message withoutBeats(Message composition)
{
    clearBeats(composition["layers"]);
    return composition;
}

bool mentions(const Message& reply, const std::string& text)
{
    for (const Message& w : reply["warnings"])
        if (w.get<std::string>().find(text) != std::string::npos)
            return true;
    return false;
}

std::string hashOf(Client& client, const fs::path& file)
{
    const Message added = addMedia(client, file);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

// Two layers and a group, effects, Autopilot, transitions, a macro, a modulator and two screens, on media under
// `root`: clips/a.mp4 and b.mp4.
azl::Composition show(const std::string& a, const std::string& b)
{
    azl::Composition c = azl::defaultComposition();
    c.bpm = 133.0;
    c.quantise = 8;
    c.column = 1;
    azl::Layer& bottom = c.layers.front();
    bottom.name = "Bottom";
    bottom.clips[1] = ClipRef{a, "a.mp4", {}};
    bottom.clips[2] = ClipRef{b, "b.mp4", {azl::SyncMode::Timeline, azl::PlayMode::Bounce}, 4, 1.0};
    bottom.playing = azl::Play{1, 8.0};
    bottom.autopilot = {azl::AutopilotMode::Next, 2};
    bottom.transition = {azl::TransitionMode::Dissolve, 2.0};
    bottom.effects = {{1,
                       "colorize",
                       {{"hue", 0.3}, {"saturation", 1.0}, {"brightness", 1.0}, {"invert", 0.0}, {"alpha", 1.0},
                        {"mix", 1.0}}},
                      {2, "transform", {{"positionX", 0}, {"positionY", 0}, {"rotation", 0}, {"scale", 1.2}, {"mix", 1}}}};
    azl::Layer top{.id = 3, .name = "Top", .opacity = 0.5, .blend = azl::BlendMode::Screen};
    top.clips[1] = ClipRef{a, "a.mp4", {}};
    top.effects = {{3, "edgeDetection", {{"multiplier", 0.25}, {"mix", 1.0}}}};
    c.layers.push_back({.id = 2, .name = "Group", .group = true, .layers = {top}});
    c.macros.push_back({1, "Hue", 0, 0.5, {{{0, 1, "hue"}, {}, 0.0, 0.5}}});
    azl::Modulator fade;
    fade.id = 1;
    fade.name = "Fade";
    fade.shape = azl::LfoShape::Saw;
    fade.beats = 4.0;
    fade.target.param = {3, 0, "opacity"};
    c.modulators.push_back(fade);
    c.screens.push_back({.id = 1,
                         .name = "Main",
                         .display = "window-1",
                         .slices = {{.id = 1, .name = "Keystone", .corners = {0.1, 0, 0.9, 0.05, 1, 1, 0, 0.95}}}});
    c.screens.push_back({.id = 2, .name = "Side", .display = "window-2", .slices = {{.id = 2, .input = 2}}});
    return c;
}

// A harness that built the show on media under `root` and saved it to root/show.json.
struct Authored {
    explicit Authored(const fs::path& root) : harness(kSize)
    {
        harness.displays().attach(display("window-1"));
        harness.displays().attach(display("window-2"));
        Client client(harness.document());
        copyMedia("count_240.mp4", root / "clips/a.mp4");
        copyMedia("count_100.mp4", root / "b.mp4");
        apply(client, azl::LoadComposition{show(hashOf(client, root / "clips/a.mp4"), hashOf(client, root / "b.mp4"))});
        harness.step(3);
        saved = snapshot(client);
        reply = composition(client, "save", root / "show.json");
    }

    azl::EngineHarness harness;
    Message saved;
    Message reply;
};

Message readJson(const fs::path& file)
{
    std::ifstream in(file);
    return Message::parse(in);
}

void writeJson(const fs::path& file, const Message& j)
{
    std::ofstream(file) << j.dump(2);
}

} // namespace

TEST_CASE("a composition file saved under the previous format name still loads")
{
    nlohmann::json file = azl::toJson(azl::CompositionFile{azl::defaultComposition()});
    file["format"] = "azylumen.composition";
    CHECK(azl::compositionFileFromJson(file).composition.layers.size() == azl::defaultComposition().layers.size());
    file["format"] = "something.else";
    CHECK_THROWS(azl::compositionFileFromJson(file));
}

TEST_CASE("a saved composition loads back to the same snapshot as one undo step")
{
    TempDir dir;
    Authored authored(dir.path / "media");
    CHECK(authored.reply["warnings"].empty());

    const Message file = readJson(dir.path / "media/show.json");
    CHECK(file["format"] == "nocturne.composition");
    const Message& cell = file["composition"]["layers"][0]["clips"]["1"];
    CHECK(cell["path"] == "clips/a.mp4");
    CHECK(cell["size"].get<uint64_t>() == fs::file_size(dir.path / "media/clips/a.mp4"));
    CHECK(file["composition"]["layers"][0]["playing"]["beat"].is_null());

    Client client(authored.harness.document());
    apply(client, azl::LoadComposition{azl::defaultComposition()});
    CHECK(snapshot(client)["layers"].size() == 1);
    const Message loaded = composition(client, "load", dir.path / "media/show.json");
    CHECK(loaded["warnings"].empty());
    CHECK(withoutBeats(snapshot(client)) == withoutBeats(authored.saved));
    authored.harness.step(10);

    apply(client, azl::SetTempo{140.0});
    client.send(msg::undo());
    client.next();
    client.send(msg::undo());
    REQUIRE(client.next()["cause"] == "undo");
    CHECK(snapshot(client)["layers"].size() == 1);
}

namespace {

Message showFile(Client& client)
{
    client.send(msg::snapshotRequest());
    return client.next().value("file", Message());
}

Message fileJson(const fs::path& path, bool unsaved)
{
    return {{"path", path.string()}, {"unsaved", unsaved}};
}

} // namespace

TEST_CASE("the document belongs to the show file it was saved to or loaded from, unsaved from an edit until undone")
{
    TempDir dir;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const fs::path tonight = dir.path / "tonight.json";
    const fs::path other = dir.path / "other.json";
    CHECK(showFile(client).is_null());

    composition(client, "save", tonight);
    CHECK(showFile(client) == fileJson(tonight, false));
    apply(client, azl::SetTempo{140.0});
    CHECK(showFile(client) == fileJson(tonight, true));
    client.send(msg::undo());
    client.next();
    CHECK(showFile(client) == fileJson(tonight, false));

    apply(client, azl::SetTempo{150.0});
    composition(client, "save", other);
    CHECK(showFile(client) == fileJson(other, false));
    composition(client, "load", tonight);
    CHECK(showFile(client) == fileJson(tonight, false));
}

TEST_CASE("a resumed show file keeps its unsaved edits until the next save")
{
    TempDir dir;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const fs::path tonight = dir.path / "tonight.json";

    harness.document().resumeFile({tonight.string(), true});
    CHECK(showFile(client) == fileJson(tonight, true));
    apply(client, azl::SetTempo{140.0});
    client.send(msg::undo());
    client.next();
    CHECK(showFile(client) == fileJson(tonight, true));
    composition(client, "save", tonight);
    CHECK(showFile(client) == fileJson(tonight, false));
    CHECK(readJson(tonight)["composition"]["bpm"] == 120.0);

    harness.document().resumeFile({tonight.string(), false});
    CHECK(showFile(client) == fileJson(tonight, false));
}

TEST_CASE("a composition file loads on another machine's media root, by path, name or content")
{
    TempDir dir;
    Authored authored(dir.path / "laptop");

    SUBCASE("by path under another root")
    {
        copyMedia("count_240.mp4", dir.path / "mini/clips/a.mp4");
        copyMedia("count_100.mp4", dir.path / "mini/b.mp4");
        fs::copy_file(dir.path / "laptop/show.json", dir.path / "mini/show.json");
        fs::remove_all(dir.path / "laptop");
        azl::EngineHarness mini(kSize);
        mini.displays().attach(display("window-1"));
        mini.displays().attach(display("window-2"));
        Client client(mini.document());
        const Message reply = composition(client, "load", dir.path / "mini/show.json");
        CHECK_MESSAGE(reply["warnings"].empty(), reply.dump());
        CHECK(withoutBeats(snapshot(client)) == withoutBeats(authored.saved));
        mini.step(10);
        CHECK(mini.stats().videoMisses.load() == 0);
    }

    SUBCASE("by name and by content")
    {
        copyMedia("count_240.mp4", dir.path / "elsewhere/deep/a.mp4");
        copyMedia("count_100.mp4", dir.path / "elsewhere/renamed.mp4");
        fs::remove_all(dir.path / "laptop/clips");
        fs::remove(dir.path / "laptop/b.mp4");
        azl::EngineHarness mini(kSize);
        Client client(mini.document());
        const Message reply = composition(client, "load", dir.path / "laptop/show.json", dir.path / "elsewhere");
        CHECK(mentions(reply, "a.mp4: found by name"));
        CHECK(mentions(reply, "b.mp4: found by content"));
        const Message loaded = snapshot(client);
        CHECK(loaded["layers"][0]["clips"] == authored.saved["layers"][0]["clips"]);
    }
}

TEST_CASE("missing media and unknown effects load as empty cells and skipped effects")
{
    TempDir dir;
    Authored authored(dir.path / "media");
    Message file = readJson(dir.path / "media/show.json");
    Message& bottom = file["composition"]["layers"][0];
    bottom["clips"]["3"] = {{"hash", std::string(64, 'f')}, {"name", "gone.mp4"}, {"path", "gone.mp4"}, {"size", 99}};
    bottom["effects"].push_back({{"id", 9}, {"type", "warp"}, {"params", {{"amount", 1}}}});
    bottom["effects"][0]["params"]["wobble"] = 2;
    bottom["effects"][1]["params"]["scale"] = 1e6;
    file["composition"]["modulators"].push_back(
        {{"id", 2}, {"name", "Warp"}, {"target", {{"layer", 1}, {"effect", 9}, {"param", "amount"}}}});
    file["composition"]["macros"][0]["targets"].push_back({{"layer", 1}, {"effect", 9}, {"param", "amount"}});
    writeJson(dir.path / "media/broken.json", file);

    Client client(authored.harness.document());
    const Message reply = composition(client, "load", dir.path / "media/broken.json");
    CHECK(mentions(reply, "gone.mp4: missing"));
    CHECK(mentions(reply, "unknown effect warp"));
    CHECK(mentions(reply, "no parameter wobble"));
    CHECK(mentions(reply, "modulator Warp"));
    const Message loaded = snapshot(client);
    CHECK_FALSE(loaded["layers"][0]["clips"].contains("3"));
    CHECK(loaded["layers"][0]["effects"].size() == 2);
    CHECK(loaded["layers"][0]["effects"][1]["params"]["scale"].get<double>() < 1e6);
    CHECK(loaded["modulators"].size() == 1);
    CHECK(loaded["macros"][0]["targets"].size() == 1);
    authored.harness.step(10);

    client.send({{"type", "composition"}, {"op", "load"}, {"path", (dir.path / "media/none.json").string()}});
    CHECK(client.next()["type"] == "error");
    std::ofstream(dir.path / "media/torn.json") << "{\"format\": \"azy";
    client.send({{"type", "composition"}, {"op", "load"}, {"path", (dir.path / "media/torn.json").string()}});
    CHECK(client.next()["type"] == "error");
}

TEST_CASE("Bloom and Classic Bloom drop out of a show with every macro target, modulator and Cue step on them")
{
    azl::Composition c = azl::defaultComposition();
    c.effects = {{20, "classicBloom", {}}, {21, "colorize", {}}};
    azl::Layer& base = c.layers.front();
    base.name = "base";
    base.effects = {{1, "bloom", {}}, {2, "transform", {}}};
    base.decks[1].effects = std::vector<azl::Effect>{{3, "classicBloom", {}}, {4, "colorize", {}}};
    base.clips[1] = ClipRef{.hash = std::string(64, 'a'), .name = "a.mp4", .effects = {{5, "bloom", {}}}};
    c.layers.push_back({.id = 2, .name = "Group", .group = true,
                        .layers = {{.id = 3, .name = "inner", .effects = {{6, "bloom", {}}, {7, "trails", {}}}}}});
    c.macros.push_back({.id = 1, .name = "Energy", .targets = {{{0, 1, "amount"}, {}, 0.0, 1.0},
                                                               {{0, 2, "scale"}, {}, 0.0, 1.0},
                                                               {{0, 20, "amount"}, {}, 0.0, 1.0}}});
    azl::Modulator onBloom{.id = 14, .name = "Kick"};
    onBloom.target.param = {0, 6, "amount"};
    azl::Modulator onMacro{.id = 15, .name = "Sine"};
    onMacro.target.macro = 1;
    azl::Modulator onTrails{.id = 16, .name = "Feed"};
    onTrails.target.param = {0, 7, "feedback"};
    c.modulators = {onBloom, onMacro, onTrails};
    c.cues = {{.id = 1, .name = "glow", .effects = {{.effect = 1, .step = 0}}},
              {.id = 2, .name = "both", .effects = {{.effect = 3, .step = 1}, {.effect = 7, .step = 0}}},
              {.id = 3, .name = "deck", .at = azl::CueAt{60}, .deck = 1}};

    const std::vector<std::string> dropped = azl::dropRemovedEffects(c);
    CHECK(dropped == std::vector<std::string>{"Classic Bloom (effect 20) on the composition",
                                              "Bloom (effect 1) on base",
                                              "Classic Bloom (effect 3) on base, deck 1",
                                              "Bloom (effect 5) on base, clip a.mp4",
                                              "Bloom (effect 6) on inner",
                                              "macro Energy: its target amount of effect 1",
                                              "macro Energy: its target amount of effect 20",
                                              "modulator 14 Kick (on effect 6)",
                                              "cue 1 glow",
                                              "cue 2 both: its steps of the removed effects"});
    CHECK(c.effects.size() == 1);
    const azl::Layer& kept = c.layers.front();
    CHECK(kept.effects.size() == 1);
    CHECK(kept.decks.at(1).effects->size() == 1);
    CHECK(kept.clips.at(1).effects.empty());
    CHECK(c.layers[1].layers[0].effects.size() == 1);
    REQUIRE(c.macros[0].targets.size() == 1);
    CHECK(c.macros[0].targets[0].param.effect == 2);
    CHECK(c.modulators == std::vector<azl::Modulator>{onMacro, onTrails});
    REQUIRE(c.cues.size() == 2);
    CHECK(c.cues[0].effects == std::vector<azl::CueEffect>{{.effect = 7, .step = 0}});
    CHECK(c.cues[1].deck == 1);
    CHECK(azl::dropRemovedEffects(c).empty());
    CHECK_FALSE(azl::findEffect("bloom"));
    CHECK_FALSE(azl::findEffect("classicBloom"));
    CHECK(azl::removedEffectsNotice({"a", "b"}) == "Bloom and Classic Bloom are removed; dropped from this show: a; b");
}

TEST_CASE("loading a show with Bloom drops it with what drives it and answers with a notice listing the drops")
{
    TempDir dir;
    Authored authored(dir.path / "media");
    Message file = readJson(dir.path / "media/show.json");
    file["composition"]["layers"][0]["effects"].push_back({{"id", 9}, {"type", "bloom"}, {"params", {{"amount", 1}}}});
    file["composition"]["modulators"].push_back(
        {{"id", 2}, {"name", "Glow"}, {"target", {{"layer", 1}, {"effect", 9}, {"param", "amount"}}}});
    writeJson(dir.path / "media/bloom.json", file);

    Client client(authored.harness.document());
    const Message reply = composition(client, "load", dir.path / "media/bloom.json");
    CHECK(reply["notice"] == "Bloom and Classic Bloom are removed; dropped from this show: Bloom (effect 9) on Bottom; "
                             "modulator 2 Glow (on effect 9)");
    CHECK(mentions(reply, "removed effect dropped: Bloom (effect 9) on Bottom"));
    CHECK_FALSE(mentions(reply, "unknown effect"));
    const Message loaded = snapshot(client);
    CHECK(loaded["layers"][0]["effects"] == authored.saved["layers"][0]["effects"]);
    CHECK(loaded["modulators"] == authored.saved["modulators"]);
    CHECK_FALSE(composition(client, "load", dir.path / "media/show.json").contains("notice"));
}

TEST_CASE("the shows listing stays inside its roots and lists the recent shows")
{
    TempDir dir;
    Authored authored(dir.path / "outside");
    azl::EngineHarness engine(kSize, 60.0, dir.path / "lib");
    const fs::path shows = dir.path / "lib/shows";
    REQUIRE(fs::is_directory(shows));
    fs::copy(dir.path / "outside", shows, fs::copy_options::recursive);
    fs::rename(shows / "show.json", shows / "full.json");
    fs::create_directories(dir.path / "lib/other");
    fs::copy_file(dir.path / "outside/show.json", dir.path / "lib/other/partial.json");
    writeJson(shows / "notes.json", {{"not", "a show"}});
    fs::create_symlink(dir.path / "outside/show.json", shows / "link.json");

    Client client(engine.document());
    auto list = [&](const std::optional<fs::path>& folder = std::nullopt) {
        Message m{{"type", "composition"}, {"op", "shows"}};
        if (folder)
            m["folder"] = folder->string();
        client.send(m);
        return client.next();
    };
    auto names = [](const Message& reply) {
        std::set<std::string> found;
        for (const Message& show : reply["shows"])
            found.insert(show["name"].get<std::string>());
        return found;
    };

    Message reply = list();
    REQUIRE_MESSAGE(reply["op"] == "shows", reply.dump());
    CHECK(names(reply) == std::set<std::string>{"full"});
    CHECK(reply["shows"][0]["mediaPresent"] == true);
    CHECK(reply["shows"][0]["size"].get<uint64_t>() == fs::file_size(shows / "full.json"));
    CHECK(reply["shows"][0]["modified"].get<double>() > 0.0);

    composition(client, "load", dir.path / "lib/other/partial.json");
    reply = list(dir.path / "lib/other");
    CHECK(names(reply) == std::set<std::string>{"partial"});
    CHECK(reply["shows"][0]["mediaPresent"] == false);

    composition(client, "load", dir.path / "outside/show.json");
    reply = list();
    CHECK(names(reply) == std::set<std::string>{"full", "partial"});
    REQUIRE(reply["recent"].size() == 1);
    CHECK(fs::equivalent(reply["recent"][0].get<std::string>(), dir.path / "lib/other/partial.json"));
    Client remote(engine.document(), false);
    remote.send({{"type", "composition"}, {"op", "shows"}});
    const Message remoteReply = remote.next();
    CHECK(remoteReply["recent"].empty());
    CHECK(names(remoteReply) == std::set<std::string>{"full"});
    CHECK(list(shows / "../..")["type"] == "error");
    CHECK(list(dir.path / "outside")["type"] == "error");
    CHECK(list(fs::path("relative"))["type"] == "error");
}

TEST_CASE("a screen whose display this machine never saw moves to the first free attached display")
{
    TempDir dir;
    Authored authored(dir.path / "media");
    azl::EngineHarness mini(kSize);
    mini.displays().attach(display("dell-mini"));
    Client client(mini.document());
    const Message reply = composition(client, "load", dir.path / "media/show.json");
    CHECK(mentions(reply, "display window-1 is not attached; showing on dell-mini"));
    CHECK(mentions(reply, "display window-2 is not attached; parked"));
    const Message loaded = snapshot(client);
    CHECK(loaded["screens"][0]["display"] == "dell-mini");
    CHECK(loaded["screens"][1]["display"] == "window-2");
    mini.step(3);
    CHECK(!mini.displays().capture("dell-mini").rgba.empty());
}

TEST_CASE("a screen whose display is known but switched off stays parked and attaches when it powers on")
{
    TempDir dir;
    Authored authored(dir.path / "media");
    azl::EngineHarness mini(kSize);
    mini.displays().attach(display("window-1"));
    mini.displays().attach(display("dell-mini"));
    mini.displays().detach("window-1");
    Client client(mini.document());

    SUBCASE("parked, then attached by its id")
    {
        const Message reply = composition(client, "load", dir.path / "media/show.json");
        CHECK(mentions(reply, "display window-1 is known here but not attached; parked until it attaches"));
        CHECK(mentions(reply, "display window-2 is not attached; showing on dell-mini"));
        const Message loaded = snapshot(client);
        CHECK(loaded["screens"][0]["display"] == "window-1");
        CHECK(loaded["screens"][1]["display"] == "dell-mini");
        REQUIRE(mini.outputs().find("window-1"));
        CHECK(!mini.outputs().find("window-1")->attached());

        mini.displays().attach(display("window-1"));
        CHECK(mini.outputs().find("window-1")->attached());
        mini.step(3);
        CHECK(!mini.displays().capture("window-1").rgba.empty());
    }

    SUBCASE("moved when asked to remap")
    {
        const Message reply = composition(client, "load", dir.path / "media/show.json", {}, true);
        CHECK(mentions(reply, "display window-1 is not attached; showing on dell-mini"));
        CHECK(mentions(reply, "display window-2 is not attached; parked"));
        CHECK(snapshot(client)["screens"][0]["display"] == "dell-mini");
    }
}

namespace {

fs::path enginePath()
{
    return bundleBinary("nocturne");
}

// Runs the engine for a moment on `file` with `state`, as the supervisor would start it, and returns its output.
std::string runEngine(const fs::path& dir, const fs::path& file)
{
    std::vector<std::string> args = {enginePath().string(), "--offscreen", "--size", "320x180", "--audio-input",
                                     "off", "--port", "7491", "--seconds", "1.5", "--library", (dir / "library").string(),
                                     "--state", (dir / "state").string(), "--composition", file.string()};
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, (dir / "out.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = 0;
    REQUIRE(posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    std::ifstream in(dir / "out.txt");
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeShow(const fs::path& file, double bpm)
{
    azl::Composition c = azl::defaultComposition();
    c.bpm = bpm;
    c.screens.push_back({.id = 1, .name = "Main", .display = "laptop-hdmi", .slices = {{.id = 1}}});
    azl::saveCompositionFile(file, {c});
}

double stateBpm(const fs::path& dir)
{
    return readJson(dir / "state/composition.json")["composition"]["bpm"];
}

} // namespace

namespace {

std::vector<fs::path> backups(const fs::path& dir, const std::string& kind)
{
    std::vector<fs::path> found;
    if (fs::exists(dir / "state/backups"))
        for (const auto& entry : fs::directory_iterator(dir / "state/backups"))
            if (entry.path().filename().string().ends_with("-" + kind + ".json"))
                found.push_back(entry.path());
    return found;
}

} // namespace

TEST_CASE("--composition starts the engine on the file; the saved state wins until another show is started")
{
    TempDir dir;
    const fs::path file = dir.path / "show.json";
    writeShow(file, 133.0);

    std::string out = runEngine(dir.path, file);
    INFO(out);
    CHECK(out.find("composition: loaded") != std::string::npos);
    CHECK(out.find("display laptop-hdmi is not attached; showing on window-1") != std::string::npos);
    Message state = readJson(dir.path / "state/composition.json");
    CHECK(state["composition"]["bpm"] == 133.0);
    CHECK(state["composition"]["screens"][0]["display"] == "window-1");
    CHECK(state["source"] == azl::sha256File(file));
    CHECK(state["show"] == file.string());

    state["composition"]["bpm"] = 140.0;
    writeJson(dir.path / "state/composition.json", state);
    out = runEngine(dir.path, file);
    CHECK(out.find("is the show the saved state holds; continuing the saved state") != std::string::npos);
    CHECK(stateBpm(dir.path) == 140.0);

    SUBCASE("a changed file of the same show keeps the saved state")
    {
        writeShow(file, 150.0);
        out = runEngine(dir.path, file);
        CHECK(out.find("the file changed since; nocturne-ctl load reloads it") != std::string::npos);
        CHECK(stateBpm(dir.path) == 140.0);
    }
    SUBCASE("another show is loaded, the saved state backed up first")
    {
        const fs::path other = dir.path / "other.json";
        writeShow(other, 160.0);
        out = runEngine(dir.path, other);
        CHECK(out.find("composition: loaded") != std::string::npos);
        CHECK(stateBpm(dir.path) == 160.0);
        const std::vector<fs::path> before = backups(dir.path, "load");
        REQUIRE(before.size() == 1);
        CHECK(readJson(before.front())["composition"]["bpm"] == 140.0);
    }
    SUBCASE("a state that predates recording the show keeps it while the file has the content it started from")
    {
        state = readJson(dir.path / "state/composition.json");
        state.erase("show");
        writeJson(dir.path / "state/composition.json", state);
        out = runEngine(dir.path, file);
        CHECK(out.find("continuing the saved state") != std::string::npos);
        CHECK(stateBpm(dir.path) == 140.0);

        state.erase("show");
        writeJson(dir.path / "state/composition.json", state);
        writeShow(file, 150.0);
        out = runEngine(dir.path, file);
        CHECK(out.find("composition: loaded") != std::string::npos);
        CHECK(stateBpm(dir.path) == 150.0);
    }
}

TEST_CASE("a restart keeps the show file the saved state belongs to, and whether it has unsaved edits")
{
    TempDir dir;
    const fs::path started = dir.path / "show.json";
    writeShow(started, 133.0);
    runEngine(dir.path, started);
    Message state = readJson(dir.path / "state/composition.json");
    CHECK(state["file"] == fileJson(started, false));

    const fs::path tonight = dir.path / "tonight.json";
    state["file"] = fileJson(tonight, true);
    writeJson(dir.path / "state/composition.json", state);
    const std::string out = runEngine(dir.path, started);
    INFO(out);
    CHECK(out.find("continuing the saved state") != std::string::npos);
    CHECK(readJson(dir.path / "state/composition.json")["file"] == fileJson(tonight, true));

    SUBCASE("a state that predates recording the file belongs to the show it started from, with edits")
    {
        state.erase("file");
        writeJson(dir.path / "state/composition.json", state);
        runEngine(dir.path, started);
        CHECK(readJson(dir.path / "state/composition.json")["file"] == fileJson(started, true));
    }
}
