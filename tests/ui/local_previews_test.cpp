#include "engine/render_thread.hpp"
#include "fake_s3.hpp"
#include "net/websocket.hpp"
#include "sync/node_sync.hpp"
#include "sync/udp_clock.hpp"
#include "test_client.hpp"
#include "ui/app/bottom_panes.hpp"
#include "ui/app/look.hpp"
#include "ui/app/session.hpp"
#include "upload/upload_states.hpp"

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace {

using azl::ClipRef;
using azl::ui::Session;

constexpr azl::EngineConfig kSize{.width = 320, .height = 180};

bool pumpUntil(const std::function<bool()>& done, double seconds = 30.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if (done())
            return true;
    }
    return false;
}

void pump(int ms)
{
    pumpUntil([] { return false; }, ms / 1000.0);
}

// The engine machine: a live main engine with Node Sync and its clock responder, served on a localhost port.
struct EngineMachine {
    TempDir media;
    azl::EngineHarness harness{kSize, 30.0};
    azl::ClockResponder clock{0, azl::engineTime};
    azl::SyncRole role{harness.document(), harness.engine(), harness.library(), azl::engineTime,
                       azl::SyncRole::Options{.role = azl::Role::Main, .name = "main", .clockPort = clock.port()}};
    azl::WebSocketServer server{harness.document(), 0};
    Client editor{harness.document()};

    std::string add(const std::string& file)
    {
        const Message added = addMedia(editor, copyMedia(file, media.path / file));
        REQUIRE(added["entry"]["status"] == "ok");
        return added["entry"]["hash"];
    }
    void place(uint64_t layer, uint64_t column, const std::string& hash)
    {
        apply(editor, azl::SetSlot{layer, column, ClipRef{hash, "clip", {}}});
    }
    uint64_t addLayer()
    {
        return apply(editor, azl::AddLayer{0, std::nullopt, azl::Layer{.name = "Top"}})["layer"]["id"];
    }
    void triggerColumn(uint64_t column)
    {
        editor.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
        REQUIRE(editor.next()["op"] == "triggerColumn");
    }
};

// This machine's library, holding only the files given.
struct LaptopLibrary {
    TempDir root;

    explicit LaptopLibrary(const std::vector<std::string>& files)
    {
        azl::MediaLibrary library(root.path);
        for (const std::string& file : files)
            library.add(copyMedia(file, root.path / "files" / file));
    }
};

std::unique_ptr<Session> localSession(EngineMachine& machine, const LaptopLibrary& laptop)
{
    return std::make_unique<Session>("127.0.0.1", machine.server.port(), true, Session::LocalRender::On,
                                     azl::ui::LocalPreviews::Options{.library = laptop.root.path,
                                                                     .width = kSize.width,
                                                                     .height = kSize.height});
}

juce::Colour engineColour(const azl::Image& image, double x, double y)
{
    const uint8_t* p = pixel(image, uint32_t(x * image.width), uint32_t(y * image.height));
    return juce::Colour(p[0], p[1], p[2]);
}

juce::Colour previewColour(const juce::Image& image, double x, double y)
{
    return image.getPixelAt(int(x * image.getWidth()), int(y * image.getHeight()));
}

bool near(juce::Colour a, juce::Colour b, int tolerance = 16)
{
    return std::abs(a.getRed() - b.getRed()) <= tolerance && std::abs(a.getGreen() - b.getGreen()) <= tolerance &&
           std::abs(a.getBlue() - b.getBlue()) <= tolerance;
}

// The clips' bottom half holds two solid patches, the same in every frame, so pictures of different moments compare.
bool sameAsEngine(EngineMachine& machine, const juce::Image& preview)
{
    if (!preview.isValid())
        return false;
    const azl::Image screen = machine.harness.readScreen();
    for (double x : {0.25, 0.75})
        if (!near(engineColour(screen, x, 0.75), previewColour(preview, x, 0.75)))
            return false;
    return true;
}

} // namespace

TEST_CASE("a UI rendering its own previews watches nothing on the engine, and its monitors show the engine's picture")
{
    EngineMachine machine;
    const std::string clip = machine.add("count_240.mp4");
    machine.place(1, 1, clip);
    machine.triggerColumn(1);
    machine.harness.startLive(30.0);
    LaptopLibrary laptop({"count_240.mp4"});
    auto session = localSession(machine, laptop);
    REQUIRE(session->localPreviews());
    session->selectLayer(1);

    REQUIRE(pumpUntil([&] { return session->client().synced() && session->localPreviews()->synced(); }));
    REQUIRE(pumpUntil([&] { return sameAsEngine(machine, session->preview("composition")); }));
    REQUIRE(pumpUntil([&] { return sameAsEngine(machine, session->preview("layer:1")); }));
    CHECK(previewColour(session->preview("composition"), 0.25, 0.75).getRed() > 150);
    CHECK(machine.harness.previewWatchers().empty());
    CHECK(machine.harness.previewPaintStats().pictures.load() == 0);

    SUBCASE("an edit on the engine shows in the local monitors")
    {
        apply(machine.editor, azl::SetLayerOpacity{1, 0.0});
        CHECK(pumpUntil([&] {
            return previewColour(session->preview("composition"), 0.25, 0.75).getRed() < 20 &&
                   sameAsEngine(machine, session->preview("composition"));
        }));
        CHECK(machine.harness.previewWatchers().empty());
    }

    SUBCASE("a remote UI's previews are painted by the engine")
    {
        Session remote("127.0.0.1", machine.server.port(), true, Session::LocalRender::Off);
        CHECK_FALSE(remote.localPreviews());
        CHECK(pumpUntil([&] { return remote.preview("composition").isValid(); }));
        CHECK(machine.harness.previewWatchers() == std::map<std::string, size_t>{{"composition", 1}});
        CHECK(machine.harness.previewPaintStats().pictures.load() > 0);
    }
    machine.harness.stopLive();
}

TEST_CASE("media missing on this machine falls back to the engine's previews, per source and per clip")
{
    EngineMachine machine;
    const std::string present = machine.add("count_240.mp4");
    const std::string missing = machine.add("count_100.mp4");
    const uint64_t top = machine.addLayer();
    machine.place(1, 1, present);
    machine.place(top, 1, missing);
    machine.place(1, 2, missing);
    machine.place(top, 2, present);
    machine.triggerColumn(1);
    machine.harness.enableClipPreviews();
    machine.harness.startLive(30.0);
    LaptopLibrary laptop({"count_240.mp4"});
    auto session = localSession(machine, laptop);
    REQUIRE(session->localPreviews());
    REQUIRE(pumpUntil([&] { return session->client().synced() && session->localPreviews()->synced(); }));
    auto watchers = [&] { return machine.harness.previewWatchers(); };

    SUBCASE("a layer with local media renders here; the composition, which shows the missing clip, on the engine")
    {
        session->selectLayer(1);
        CHECK(pumpUntil([&] { return watchers() == std::map<std::string, size_t>{{"composition", 1}}; }));
        CHECK(pumpUntil([&] {
            return session->preview("layer:1").isValid() && session->preview("composition").isValid();
        }));
        CHECK(sameAsEngine(machine, session->preview("composition")));
        session->selectLayer(top);
        CHECK(pumpUntil([&] {
            return watchers() == std::map<std::string, size_t>{{"composition", 1}, {"layer", 1}};
        }));
    }

    SUBCASE("the selected clip's preview: a clip of local media renders here, one of missing media on the engine")
    {
        session->selectClip({1, 2});
        CHECK(pumpUntil([&] { return watchers().contains("clip"); }));
        CHECK(pumpUntil([&] { return session->clipPreview(missing).isValid(); }));
        session->selectClip({top, 2});
        CHECK(pumpUntil([&] { return !watchers().contains("clip"); }));
        CHECK(pumpUntil([&] { return session->clipPreview(present).isValid(); }));
    }

    SUBCASE("when the missing clip stops showing, the composition comes back to this machine")
    {
        machine.editor.send({{"type", "trigger"}, {"op", "stop"}, {"layer", top}});
        CHECK(pumpUntil([&] { return watchers().empty(); }));
        CHECK(pumpUntil([&] { return sameAsEngine(machine, session->preview("composition")); }));
    }
    machine.harness.stopLive();
}

TEST_CASE("Previews off stops the local rendering too; on again, it comes back")
{
    EngineMachine machine;
    machine.place(1, 1, machine.add("count_240.mp4"));
    machine.triggerColumn(1);
    machine.harness.startLive(30.0);
    LaptopLibrary laptop({"count_240.mp4"});
    auto session = localSession(machine, laptop);
    azl::ui::LocalPreviews& local = *session->localPreviews();
    REQUIRE(pumpUntil([&] { return sameAsEngine(machine, session->preview("composition")); }));
    CHECK(local.rendering());

    session->setPreviewsOn(false);
    CHECK_FALSE(local.rendering());
    const uint64_t frames = local.frames();
    pump(500);
    CHECK(local.frames() == frames);
    CHECK_FALSE(session->preview("composition").isValid());
    CHECK(machine.harness.previewWatchers().empty());

    session->setPreviewsOn(true);
    CHECK(local.rendering());
    CHECK(pumpUntil([&] { return sameAsEngine(machine, session->preview("composition")); }));
    CHECK(pumpUntil([&] { return local.frames() > frames + 10; }));
    CHECK(machine.harness.previewWatchers().empty());
    machine.harness.stopLive();
}

namespace {

std::unique_ptr<Session> directorSession(EngineMachine& machine, const LaptopLibrary& laptop,
                                         azl::ui::DirectorMedia::Options media)
{
    return std::make_unique<Session>("127.0.0.1", machine.server.port(), true, Session::LocalRender::On,
                                     azl::ui::LocalPreviews::Options{.library = laptop.root.path,
                                                                     .width = kSize.width,
                                                                     .height = kSize.height},
                                     std::move(media));
}

} // namespace

TEST_CASE("a clip missing here gets one still from the engine, which paints nothing for it; a restart asks no more")
{
    EngineMachine machine;
    const std::string present = machine.add("count_240.mp4");
    const std::string missing = machine.add("count_100.mp4");
    machine.place(1, 1, present);
    machine.place(1, 2, missing);
    machine.triggerColumn(1);
    machine.harness.startLive(30.0);
    LaptopLibrary laptop({"count_240.mp4"});
    TempDir director;
    const azl::ui::DirectorMedia::Options media{.stills = director.path / "stills"};
    auto session = directorSession(machine, laptop, media);
    REQUIRE(pumpUntil([&] { return session->client().synced() && session->localPreviews()->synced(); }));

    CHECK(pumpUntil([&] { return session->still(missing).isValid() && session->still(present).isValid(); }));
    pump(300);
    CHECK(machine.harness.stills().asked.load() == 1);
    CHECK(machine.harness.stills().rendered.load() == 1);
    CHECK(machine.harness.previewWatchers().empty());
    CHECK(machine.harness.previewPaintStats().pictures.load() == 0);

    session.reset();
    auto again = directorSession(machine, laptop, media);
    REQUIRE(pumpUntil([&] { return again->client().synced(); }));
    CHECK(again->still(missing).isValid());
    CHECK(pumpUntil([&] { return again->still(present).isValid(); }));
    pump(300);
    CHECK(machine.harness.stills().asked.load() == 1);
    CHECK(machine.harness.previewWatchers().empty());
    machine.harness.stopLive();
}

TEST_CASE("choosing a missing clip fetches it from the bucket and its previews move here without a restart")
{
    EngineMachine machine;
    const std::string missing = machine.add("count_240.mp4");
    machine.place(1, 1, missing);
    machine.triggerColumn(1);
    machine.harness.startLive(30.0);
    FakeS3 s3;
    s3.putObject(missing, readAll(mediaDir() / "count_240.mp4"));
    LaptopLibrary laptop({});
    TempDir director;
    auto session = directorSession(machine, laptop,
                                   {.keepLocal = director.path / "keep-local.json",
                                    .stills = director.path / "stills",
                                    .fetched = director.path / "fetched",
                                    .bucket = [&] { return std::make_shared<azl::S3Client>(s3.config()); }});
    session->selectLayer(1);
    auto watchers = [&] { return machine.harness.previewWatchers(); };
    const std::map<std::string, size_t> onEngine{{"composition", 1}, {"layer", 1}};
    REQUIRE(pumpUntil([&] { return session->localPreviews()->synced() && watchers() == onEngine; }));
    CHECK(session->media().where(missing) == azl::ui::DirectorMedia::Where::Remote);

    session->keepClip(missing, true);
    CHECK(pumpUntil([&] { return session->media().where(missing) == azl::ui::DirectorMedia::Where::Local; }));
    CHECK(pumpUntil([&] { return watchers().empty(); }));
    CHECK(pumpUntil([&] { return sameAsEngine(machine, session->preview("composition")); }));
    const fs::path fetched = azl::fetchedFile(director.path / "fetched", missing);
    CHECK(fs::exists(fetched));
    CHECK(s3.count("get") > 0);

    SUBCASE("not kept any more, the fetched copy is deleted and the engine paints its previews again")
    {
        session->keepClip(missing, false);
        CHECK_FALSE(fs::exists(fetched));
        CHECK(pumpUntil([&] { return watchers() == onEngine; }));
        CHECK(s3.objects().contains(missing));
    }
    machine.harness.stopLive();
}

namespace {

bool badgeHas(azl::ui::BrowserPane& browser, juce::Rectangle<int> tile, juce::Colour colour)
{
    const juce::Image image = browser.createComponentSnapshot(browser.getLocalBounds());
    const auto badge = azl::ui::BrowserPane::uploadBadge(tile).reduced(1);
    for (int y = badge.getY(); y < badge.getBottom(); ++y)
        for (int x = badge.getX(); x < badge.getRight(); ++x)
            if (near(image.getPixelAt(x, y), colour, 8))
                return true;
    return false;
}

} // namespace

TEST_CASE("the Files tab's upload badge: local only, uploading with its progress, in the bucket, failed")
{
    using Upload = azl::ui::DirectorMedia::Upload;
    using Kind = azl::UploadState::Kind;
    EngineMachine machine;
    const std::string local = machine.add("count_240.mp4");
    const std::string elsewhere = machine.add("count_100.mp4");
    LaptopLibrary laptop({"count_240.mp4"});
    TempDir director;
    const fs::path uploads = director.path / "states.json";
    auto session = directorSession(machine, laptop, {.stills = director.path / "stills", .uploads = uploads});
    azl::ui::BrowserPane browser(*session);
    browser.setSize(400, 300);
    browser.selectTab(0);
    REQUIRE(pumpUntil([&] { return session->client().synced() && session->client().library().size() == 2; }));
    auto tile = [&](const std::string& hash) {
        for (const auto& item : browser.fileItems())
            if (item.media && item.media->hash == hash)
                return item.bounds;
        FAIL("no tile for " << hash);
        return juce::Rectangle<int>();
    };
    auto tip = [&](const std::string& hash) { return browser.tipAt(azl::ui::BrowserPane::uploadBadge(tile(hash)).getCentre()); };
    const azl::ui::Palette& p = azl::ui::Look::palette();

    CHECK(session->media().upload(local) == Upload::LocalOnly);
    CHECK(tip(local) == "Only on this machine: not uploaded to the bucket yet");
    CHECK(badgeHas(browser, tile(local), p.muted));
    CHECK(session->media().upload(elsewhere) == Upload::Unknown);
    CHECK_FALSE(tip(elsewhere).startsWith("Only on this machine"));

    azl::UploadStates writer(uploads);
    writer.set(local, {.kind = Kind::Uploading, .bytes = 25, .size = 100, .pid = getpid()});
    CHECK(pumpUntil([&] { return session->media().upload(local) == Upload::Uploading; }, 5.0));
    CHECK(tip(local) == "Uploading to the bucket: 25%");
    CHECK(badgeHas(browser, tile(local), p.accent));

    writer.set(local, {.kind = Kind::Uploaded, .bytes = 100, .size = 100});
    writer.set(elsewhere, {.kind = Kind::Failed, .bytes = 10, .size = 100, .error = "connection lost"});
    CHECK(pumpUntil([&] { return session->media().upload(elsewhere) == Upload::Failed; }, 5.0));
    CHECK(session->media().upload(local) == Upload::Uploaded);
    CHECK(tip(local) == "In the bucket: every venue can fetch it");
    CHECK(badgeHas(browser, tile(local), p.live));
    CHECK(tip(elsewhere) == "Upload failed: connection lost");
    CHECK(badgeHas(browser, tile(elsewhere), p.fault));
}
