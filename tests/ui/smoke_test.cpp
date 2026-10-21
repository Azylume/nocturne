#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <functional>

namespace {

fs::path shotsDir()
{
    const char* dir = std::getenv("NOCTURNE_UI_SHOTS");
    return dir ? dir : NOCTURNE_UI_SHOTS;
}

void writePng(const juce::Image& image, const std::string& name)
{
    fs::create_directories(shotsDir());
    juce::File file(juce::String((shotsDir() / name).string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

// Renders frames and runs the UI's message loop until `done`, or fails after `seconds`.
bool runUntil(azl::EngineHarness& harness, const std::function<bool()>& done, double seconds = 30.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if (done())
            return true;
    }
    return false;
}

bool showsSomething(const juce::Image& image)
{
    if (!image.isValid())
        return false;
    const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < image.getHeight(); y += 4)
        for (int x = 0; x < image.getWidth(); x += 4)
            if (data.getPixelColour(x, y).getBrightness() > 0.2f)
                return true;
    return false;
}

int countColour(const juce::Image& image, juce::Colour colour)
{
    int count = 0;
    const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < image.getHeight(); y += 2)
        for (int x = 0; x < image.getWidth(); x += 2)
            count += data.getPixelColour(x, y) == colour;
    return count;
}

uint64_t addLayer(Client& client, const std::string& name, uint64_t parent = 0, bool group = false)
{
    azl::Layer layer;
    layer.name = name;
    layer.group = group;
    return apply(client, azl::AddLayer{parent, std::nullopt, layer})["layer"]["id"];
}

} // namespace

TEST_CASE("the UI connects to an engine, renders the grid with stills and live previews, and triggers from a click")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_240.mp4", "count_100.mp4"};
    std::vector<azl::ClipRef> clips;
    for (const char* file : files) {
        const Message added = addMedia(client, copyMedia(file, media.path / file));
        clips.push_back({added["entry"]["hash"], added["entry"]["name"]});
    }
    const uint64_t base = 1;
    const uint64_t mid = addLayer(client, "mid");
    const uint64_t group = addLayer(client, "Top", 0, true);
    const uint64_t high = addLayer(client, "high", group);
    apply(client, azl::SetLayerBlend{mid, azl::BlendMode::Screen});
    apply(client, azl::SetLayerOpacity{mid, 0.7});
    apply(client, azl::SetLayerAutopilot{high, azl::AutopilotMode::Random, 4});
    const auto columns = harness.document().document().decks[0].columns;
    for (size_t c = 0; c < columns.size(); ++c) {
        apply(client, azl::SetSlot{base, columns[c].id, clips[c % clips.size()]});
        if (c % 2 == 0)
            apply(client, azl::SetSlot{mid, columns[c].id, clips[(c + 1) % clips.size()]});
        if (c != 1)
            apply(client, azl::SetSlot{high, columns[c].id, clips[(c + 2) % clips.size()]});
    }
    apply(client, azl::AddColumn{1, std::nullopt, azl::Column{0, "Extra"}, {azl::Cell{base, 0, clips[3]}}});
    azl::Screen screen{.name = "Projector",
                       .slices = {azl::Slice{.name = "Keystone",
                                             .corners = {0.08, 0.05, 0.92, 0.12, 0.97, 0.95, 0.03, 0.88}}}};
    apply(client, azl::AddScreen{std::nullopt, screen});
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
    harness.step(4);

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);

    auto everyStill = [&] {
        for (const azl::ClipRef& clip : clips)
            if (!session.still(clip.hash).isValid())
                return false;
        return true;
    };
    REQUIRE(runUntil(harness, [&] {
        return session.client().synced() && everyStill() && session.preview("composition").isValid() &&
               session.client().playing(base, columns[0].id);
    }));

    CHECK(window.grid().stack().rows().size() == 4);
    CHECK(window.grid().stack().slots() == 12);
    session.selectClip({mid, columns[0].id});
    REQUIRE(runUntil(harness, [&] { return showsSomething(session.preview(session.previewSource())); }));
    CHECK(showsSomething(session.preview("composition")));
    window.resized();
    const juce::Image perform = window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f);
    writePng(perform, "perform.png");
    CHECK(countColour(perform, azl::ui::Look::palette().live) > 200);

    // Clicking a thumbnail triggers its clip, on the next bar.
    session.clickClip({base, columns[2].id});
    REQUIRE(runUntil(harness, [&] {
        const azl::Layer* layer = session.layer(base);
        return layer && layer->playing && layer->playing->column == columns[2].id;
    }));
    CHECK(runUntil(harness, [&] { return session.client().playing(base, columns[2].id); }));

    // A second click on a queued clip takes the trigger back; the layer stays on the clip it shows.
    session.clickClip({base, columns[3].id});
    REQUIRE(runUntil(harness, [&] { return session.client().queued(base, columns[3].id); }));
    session.clickClip({base, columns[3].id});
    REQUIRE(runUntil(harness, [&] {
        const azl::Layer* layer = session.layer(base);
        return layer && layer->playing && layer->playing->column == columns[2].id;
    }));
    CHECK(session.client().playing(base, columns[2].id));

    // Esc takes back every queued trigger.
    session.clickClip({base, columns[0].id});
    session.clickClip({mid, columns[2].id});
    REQUIRE(runUntil(harness, [&] {
        return session.client().queued(base, columns[0].id) && session.client().queued(mid, columns[2].id);
    }));
    session.cancelQueued();
    REQUIRE(runUntil(harness, [&] {
        return !session.client().queued(base, columns[0].id) && !session.client().queued(mid, columns[2].id);
    }));
    CHECK(session.layer(base)->playing->column == columns[2].id);

    window.setMapMode(true);
    const uint64_t screenId = session.composition().screens.back().id;
    session.setMapScreen(screenId);
    REQUIRE(runUntil(harness, [&] { return showsSomething(session.preview("screen:" + std::to_string(screenId))); }));
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "map.png");
}

TEST_CASE("the selected clip moves on the Preview Monitor while it is not playing; grid cells keep their stills")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    std::vector<azl::ClipRef> clips;
    for (const char* file : {"count_240.mp4", "count_100.mp4"}) {
        const Message added = addMedia(client, copyMedia(file, media.path / file));
        clips.push_back({added["entry"]["hash"], added["entry"]["name"]});
    }

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    const uint64_t layer = window.grid().stack().rows().front().layer;
    const auto columns = harness.document().document().decks[0].columns;
    apply(client, azl::SetSlot{layer, columns[0].id, clips[0]});
    apply(client, azl::SetSlot{layer, columns[1].id, clips[1]});
    REQUIRE(runUntil(harness, [&] { return session.layer(layer) && session.layer(layer)->clips.size() == 2; }));
    CHECK_FALSE(runUntil(harness, [&] { return session.clipPreview(clips[0].hash).isValid(); }, 2.0));

    // Selected and not playing: its preview takes on new pictures over time; the other clip is not watched.
    session.selectClip({layer, columns[0].id});
    std::set<std::vector<uint32_t>> seen;
    auto pixelsOf = [](const juce::Image& image) {
        const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
        std::vector<uint32_t> pixels;
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                pixels.push_back(data.getPixelColour(x, y).getARGB());
        return pixels;
    };
    CHECK(runUntil(harness, [&] {
        if (const juce::Image picture = session.clipPreview(clips[0].hash); picture.isValid())
            seen.insert(pixelsOf(picture));
        return seen.size() >= 3;
    }));
    CHECK_FALSE(session.clipPreview(clips[1].hash).isValid());
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "clip-previews.png");

    // Once it plays, the monitor shows the layer live and the clip is no longer watched.
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
    REQUIRE(runUntil(harness, [&] { return session.client().playing(layer, columns[0].id); }));
    CHECK(runUntil(harness, [&] { return !session.clipPreview(clips[0].hash).isValid(); }));

    // Selecting the clip that is not playing watches that one instead.
    session.selectClip({layer, columns[1].id});
    CHECK(runUntil(harness, [&] { return session.clipPreview(clips[1].hash).isValid(); }));
}

TEST_CASE("the Preview Monitor's Move idle clips button turns them on and off on the engine and follows its state")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    const azl::ClipRef clip{added["entry"]["hash"], added["entry"]["name"]};

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    const uint64_t layer = window.grid().stack().rows().front().layer;
    const uint64_t column = harness.document().document().decks[0].columns[0].id;
    apply(client, azl::SetSlot{layer, column, clip});
    REQUIRE(runUntil(harness, [&] { return session.layer(layer) && !session.layer(layer)->clips.empty(); }));
    session.selectClip({layer, column});
    juce::TextButton& button = window.preview().liveClipsButton();
    CHECK_FALSE(button.getToggleState());
    CHECK_FALSE(runUntil(harness, [&] { return session.clipPreview(clip.hash).isValid(); }, 1.0));

    button.triggerClick();
    CHECK(runUntil(harness, [&] { return harness.clipPreviews() && session.client().clipPreviews(); }));
    CHECK(button.getToggleState());
    CHECK(runUntil(harness, [&] { return session.clipPreview(clip.hash).isValid(); }));
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "clip-previews-on.png");

    // Turned off elsewhere (nocturne-ctl clippreviews off): the button follows and the engine stops sampling.
    client.send({{"type", "preview"}, {"op", "clips"}, {"on", false}});
    CHECK(runUntil(harness, [&] { return !button.getToggleState(); }));
    CHECK_FALSE(session.client().clipPreviews());
    const uint64_t sampled = harness.clipPreviewStats().pictures;
    CHECK_FALSE(runUntil(harness, [&] { return harness.clipPreviewStats().pictures > sampled + 1; }, 1.0));

    button.triggerClick();
    CHECK(runUntil(harness, [&] { return harness.clipPreviews() && button.getToggleState(); }));
    button.triggerClick();
    CHECK(runUntil(harness, [&] { return !harness.clipPreviews() && !button.getToggleState(); }));
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "clip-previews-off.png");
}

TEST_CASE("the Previews button stops every preview watch, restores them, and is remembered on this machine")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    const azl::ClipRef clip{added["entry"]["hash"], added["entry"]["name"]};
    const juce::File file(juce::String((media.path / "ui.settings").string()));
    auto settings = [&] { return std::make_unique<juce::PropertiesFile>(file, juce::PropertiesFile::Options{}); };
    auto targets = [&] { return harness.previewSizes()["previewTargets"]; };
    azl::WebSocketServer server(harness.document(), 0);
    {
        azl::ui::Session session("127.0.0.1", server.port());
        azl::ui::MainComponent window(session, settings());
        window.setSize(1600, 950);
        REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
        const uint64_t layer = window.grid().stack().rows().front().layer;
        const uint64_t column = harness.document().document().decks[0].columns[0].id;
        apply(client, azl::SetSlot{layer, column, clip});
        REQUIRE(runUntil(harness, [&] { return session.layer(layer) && !session.layer(layer)->clips.empty(); }));
        session.selectClip({layer, column});
        REQUIRE(runUntil(harness, [&] {
            return session.preview("composition").isValid() && session.clipPreview(clip.hash).isValid();
        }));
        CHECK(targets() > 0);
        juce::TextButton& button = window.transport().previewsButton();
        CHECK(button.getToggleState());

        button.triggerClick();
        CHECK(runUntil(harness, [&] { return !session.previewsOn() && targets() == 0; }));
        CHECK_FALSE(button.getToggleState());
        CHECK_FALSE(session.preview("composition").isValid());
        CHECK_FALSE(session.clipPreview(clip.hash).isValid());
        const uint64_t sampled = harness.clipPreviewStats().pictures;
        CHECK_FALSE(runUntil(harness, [&] {
            return targets() > 0 || harness.clipPreviewStats().pictures > sampled + 1 ||
                   session.preview("composition").isValid();
        }, 1.0));
        writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "previews-off.png");

        button.triggerClick();
        CHECK(runUntil(harness, [&] {
            return targets() > 0 && session.preview("composition").isValid() && session.clipPreview(clip.hash).isValid();
        }));
        CHECK(button.getToggleState());
        button.triggerClick();
        CHECK(runUntil(harness, [&] { return targets() == 0; }));
    }
    CHECK_FALSE(settings()->getBoolValue("previews", true));

    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session, settings());
    CHECK_FALSE(session.previewsOn());
    CHECK_FALSE(window.transport().previewsButton().getToggleState());
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    CHECK_FALSE(runUntil(harness, [&] { return targets() > 0; }, 1.0));
}

TEST_CASE("a playing text or generator cell is live in the grid and the Preview Monitor shows its layer")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t layer = 1;
    const auto columns = harness.document().document().decks[0].columns;
    apply(client, azl::SetQuantise{0});
    const azl::TextClip text{.text = "TEXT", .size = 0.5, .color = {1.0, 0.0, 0.0, 1.0}};
    const azl::Effect solid{0, "solid", {{"red", 0.0}, {"green", 1.0}, {"blue", 0.0}}};
    apply(client, azl::SetSlot{layer, columns[0].id, azl::ClipRef{.name = "Text", .text = text}});
    apply(client, azl::SetSlot{layer, columns[1].id, azl::ClipRef{.name = "solid", .effects = {solid}}});

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    auto near = [](juce::Colour a, juce::Colour b) {
        return std::abs(a.getRed() - b.getRed()) < 30 && std::abs(a.getGreen() - b.getGreen()) < 30 &&
               std::abs(a.getBlue() - b.getBlue()) < 30;
    };
    auto monitorShows = [&](juce::Colour colour) {
        juce::Component& monitor = window.preview();
        const juce::Image image = monitor.createComponentSnapshot(monitor.getLocalBounds(), true, 1.0f);
        const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
        int count = 0;
        for (int y = 0; y < image.getHeight(); y += 2)
            for (int x = 0; x < image.getWidth(); x += 2)
                count += near(data.getPixelColour(x, y), colour);
        return count > 500;
    };

    for (size_t i = 0; i < 2; ++i) {
        session.selectClip({layer, columns[i].id});
        client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", columns[i].id}});
        CHECK(runUntil(harness, [&] {
            return session.client().playing(layer, columns[i].id) && !session.client().queued(layer, columns[i].id);
        }));
        CHECK(runUntil(harness, [&] { return monitorShows(i == 0 ? juce::Colours::red : juce::Colour(0, 255, 0)); }));
    }
    CHECK_FALSE(session.client().playing(layer, columns[0].id));
}

TEST_CASE("the UI remembers the last engine host by name, and says when a name does not resolve")
{
    TempDir dir;
    const juce::File file(juce::String((dir.path / "ui.settings").string()));
    auto settings = [&] { return std::make_unique<juce::PropertiesFile>(file, juce::PropertiesFile::Options{}); };
    CHECK(azl::ui::rememberedHost(*settings(), "") == "127.0.0.1");
    CHECK(azl::ui::rememberedHost(*settings(), "mini.tailnet.ts.net") == "mini.tailnet.ts.net");
    CHECK(azl::ui::rememberedHost(*settings(), "") == "mini.tailnet.ts.net");
    CHECK(azl::ui::rememberedHost(*settings(), "localhost") == "localhost");
    CHECK(azl::ui::rememberedHost(*settings(), "") == "localhost");

    azl::EngineHarness harness({.width = 960, .height = 540});
    azl::ui::Session session("nocturne-no-such-host.invalid", azl::kDefaultPort, true,
                             azl::ui::Session::LocalRender::Off);
    CHECK(runUntil(harness, [&] { return !session.connectionError().empty(); }));
    CHECK(session.connectionError().find("cannot resolve host") != std::string::npos);
    CHECK_FALSE(session.client().connected());
}

TEST_CASE("Settings: the recording folder with its free space, another chosen on the engine machine, and the codec")
{
    TempDir root;
    const fs::path first = root.path / "first";
    const fs::path second = root.path / "second";
    for (const fs::path& made : {first, second / "Night 1", root.path / "Volumes" / "Stage SSD"})
        fs::create_directories(made);
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.recordService().setVolumes(root.path / "Volumes");
    Client client(harness.document());
    const auto record = [&](Message request) {
        request["type"] = "record";
        client.send(request);
        for (;;)
            if (Message m = client.next(); m["type"] == "error" || (m["type"] == "record" && m["op"] == request["op"]))
                return m;
    };
    REQUIRE(record({{"op", "set"}, {"path", first.string()}})["type"] == "record");

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    window.transport().settingsButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return window.transport().settings() != nullptr; }));
    azl::ui::SettingsModal* settings = window.transport().settings();
    settings->setNativeChooser(false);
    REQUIRE(runUntil(harness, [&] { return settings->folderText().contains("GB free"); }));
    CHECK(settings->folderText().startsWith(juce::String(first.string())));

    settings->chooseButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return settings->picking() && !settings->listed().path.empty(); }));
    REQUIRE(settings->listed().volumes.size() == 1);
    CHECK(settings->listed().volumes[0].name == "Stage SSD");
    settings->upButton().triggerClick();
    const fs::path real = fs::weakly_canonical(root.path);
    REQUIRE(runUntil(harness, [&] { return settings->listed().path == real.string(); }));
    const auto& folders = settings->listed().folders;
    const auto at = std::find_if(folders.begin(), folders.end(), [](const auto& f) { return f.name == "second"; });
    REQUIRE(at != folders.end());
    settings->enter(int(settings->listed().volumes.size() + size_t(at - folders.begin())));
    REQUIRE(runUntil(harness, [&] { return settings->listed().path == (real / "second").string(); }));
    REQUIRE(settings->listed().folders.size() == 1);
    CHECK(settings->listed().folders[0].name == "Night 1");
    settings->useButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return session.client().record().path == (real / "second").string(); }));
    CHECK_FALSE(settings->picking());

    settings->codecBox().setSelectedId(2);
    REQUIRE(runUntil(harness, [&] { return session.client().record().codec == "prores"; }));
    REQUIRE(runUntil(harness, [&] { return settings->folderText().contains("GB free"); }));
    CHECK(settings->maxTotalField().getText() == "8");
    CHECK(settings->gigGraceField().getText() == "15");
    settings->maxTotalField().setText("6.5");
    settings->gigGraceField().setText("30");
    settings->maxTotalField().onReturnKey();
    REQUIRE(runUntil(harness, [&] { return session.client().record().maxTotalSeconds == 6.5 * 3600; }));
    CHECK(session.client().record().gigGraceSeconds == 1800.0);
    Client fresh(harness.document());
    fresh.send({{"type", "record"}, {"op", "state"}});
    Message state = fresh.next();
    while (state["type"] != "record")
        state = fresh.next();
    CHECK(state["path"] == (real / "second").string());
    CHECK(state["codec"] == "prores");
    CHECK(state["maxTotalSeconds"] == 6.5 * 3600);
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "settings-recording.png");
}

TEST_CASE("Settings: audio input, displays, performance and caches drive the engine machine's settings")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.displays().attach({.id = "dell", .name = "DELL", .width = 1920, .height = 1080});
    harness.displays().attach({.id = "projector", .name = "Projector", .width = 1280, .height = 720});
    Client client(harness.document());
    client.send(msg::command(azl::AddScreen{std::nullopt, {.name = "Screen 1", .display = "dell", .slices = {{}}}}));
    client.next();
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    window.transport().settingsButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return window.transport().settings() != nullptr; }));
    azl::ui::SettingsModal* settings = window.transport().settings();
    using Section = azl::ui::SettingsModal::Section;
    REQUIRE(runUntil(harness, [&] { return settings->offers(Section::Caches); }));

    settings->sectionButton(Section::Audio).triggerClick();
    REQUIRE(runUntil(harness, [&] { return settings->section() == Section::Audio; }));
    REQUIRE(runUntil(harness, [&] { return settings->inputBox().getNumItems() == 4; }));
    CHECK(settings->inputBox().getItemText(3).startsWith("Fake Interface"));
    settings->inputBox().setSelectedId(4);
    REQUIRE(runUntil(harness, [&] { return harness.audioInput().status().uid == "fake-interface"; }));
    REQUIRE(runUntil(harness, [&] { return settings->inputText().startsWith("Fake Interface"); }));

    settings->sectionButton(Section::Displays).triggerClick();
    REQUIRE(runUntil(harness, [&] { return settings->section() == Section::Displays; }));
    REQUIRE(runUntil(harness, [&] { return settings->screenBoxes() == 1 && settings->screenBox(0).getNumItems() == 3; }));
    CHECK(settings->screenBox(0).getText().startsWith("DELL"));
    settings->screenBox(0).setSelectedId(3);
    REQUIRE(runUntil(harness, [&] { return harness.engine().composition()->screens.back().display == "projector"; }));
    CHECK(harness.screenDisplays().roles() == azl::RoleDisplays{{"Screen 1", {"projector"}}});
    CHECK(session.client().history().undo == 1);
    CHECK(session.client().history().undoOp == "addScreen");

    settings->sectionButton(Section::Performance).triggerClick();
    REQUIRE(runUntil(harness, [&] { return settings->section() == Section::Performance; }));
    REQUIRE(runUntil(harness, [&] { return settings->renderScaleBox().getSelectedId() == 100; }));
    settings->renderScaleBox().setSelectedId(83);
    REQUIRE(runUntil(harness, [&] { return harness.engine().renderScale() == 83; }));
    settings->renderScaleBox().setSelectedId(100);
    REQUIRE(runUntil(harness, [&] { return harness.engine().renderScale() == 100; }));
    settings->profileBox().setSelectedId(2);
    REQUIRE(runUntil(harness, [&] { return harness.settings().settings().profile == azl::GuardProfile::Sharp; }));
    settings->protectToggle().setToggleState(true, juce::sendNotification);
    REQUIRE(runUntil(harness, [&] { return harness.settings().settings().protectTransitions; }));
    settings->bakeToggle().setToggleState(true, juce::sendNotification);
    REQUIRE(runUntil(harness, [&] { return harness.engine().bakeStatic(); }));

    settings->sectionButton(Section::Caches).triggerClick();
    REQUIRE(runUntil(harness, [&] { return settings->section() == Section::Caches; }));
    REQUIRE(runUntil(harness, [&] { return settings->preloadField().getText() == "3072"; }));
    CHECK(settings->useText("preloadMB").contains("in use"));
    settings->preloadField().setText("512");
    settings->preloadField().onReturnKey();
    REQUIRE(runUntil(harness, [&] { return harness.settings().settings().preloadMB == 512; }));
    CHECK(settings->overriddenText("preloadMB").isEmpty());
    writePng(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f), "settings-caches.png");
}

TEST_CASE("Settings: an engine without the settings service shows Recording only")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.document().handle("settings", nullptr);
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    window.transport().settingsButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return window.transport().settings() != nullptr; }));
    azl::ui::SettingsModal* settings = window.transport().settings();
    runUntil(harness, [] { return false; }, 0.5);
    CHECK(settings->offers(azl::ui::SettingsModal::Section::Recording));
    CHECK_FALSE(settings->offers(azl::ui::SettingsModal::Section::Audio));
    CHECK_FALSE(settings->sectionButton(azl::ui::SettingsModal::Section::Caches).isVisible());
    settings->showSection(azl::ui::SettingsModal::Section::Caches);
    CHECK(settings->section() == azl::ui::SettingsModal::Section::Recording);
}
