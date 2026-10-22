#include "test_client.hpp"
#include "fake_s3.hpp"
#include "upload/media_sync.hpp"

#include <doctest.h>

#include <atomic>
#include <functional>
#include <thread>

namespace {

using namespace std::chrono_literals;

int utc(int64_t) { return 0; }

// The night of 2026-10-16, 22:00 to 05:00 UTC.
const azl::GigWindow kNight{"2026-10-16", 22 * 60, 5 * 60};
const double kStart = double(azl::unixAt(*azl::dateDays("2026-10-16"), 22 * 60, utc));
const double kAfternoon = kStart - 6 * 3600.0;

bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 20s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (done())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

azl::ClipRef clip(const std::string& hash, const std::string& name)
{
    return {.hash = hash, .name = name};
}

// Deck "Opening" holds columns 1-2, "Headliner" columns 3-4. Layer 1 has `a` in column 1 and `b` in column 3; layer 2
// a text clip in column 2 and `c` in column 4.
azl::Composition twoDecks(const std::string& a, const std::string& b, const std::string& c)
{
    azl::Composition composition = azl::defaultComposition();
    composition.decks = {{1, "Opening", {{1, "A"}, {2, "B"}}}, {2, "Headliner", {{3, "C"}, {4, "D"}}}};
    composition.layers.resize(2);
    composition.layers[0].id = 1;
    composition.layers[1].id = 2;
    composition.layers[0].clips = {{1, clip(a, "a.mp4")}, {3, clip(b, "b.mp4")}};
    composition.layers[1].clips = {{2, azl::ClipRef{.name = "Hello", .text = azl::TextClip{}}}, {4, clip(c, "c.mov")}};
    composition.gigs = {kNight};
    return composition;
}

// A venue engine whose composition needs three files, all in the fake bucket, and its media sync on a fake clock.
struct Venue {
    // `firstClip` replaces the first clip's hash in the composition.
    explicit Venue(const std::string& firstClip = "")
        : objects{readAll(mediaDir() / "count_100.mp4"), readAll(mediaDir() / "count_240.mp4"), bytes(4500)},
          hashes{azl::sha256Hex(objects[0]), azl::sha256Hex(objects[1]), azl::sha256Hex(objects[2])},
          harness({.width = 320, .height = 180}, 60.0, dir.path / "library",
                  twoDecks(firstClip.empty() ? hashes[0] : firstClip, hashes[1], hashes[2]))
    {
        for (size_t i = 0; i < objects.size(); ++i)
            s3.putObject(hashes[i], objects[i]);
    }

    ~Venue() { harness.onMediaStatus(nullptr); }

    // `initial` is the override the venue kept from before.
    void start(azl::MediaSyncOptions options = {}, bool withClient = true,
               azl::SyncOverride initial = azl::SyncOverride::Auto)
    {
        if (initial != azl::SyncOverride::Auto) {
            options.settings = dir.path / "media-sync.json";
            writeAll(options.settings, std::string("{\"override\": \"") + azl::overrideName(initial) + "\"}");
        }
        options.folder = dir.path / "downloads";
        options.every = 50ms;
        options.retry = 50ms;
        options.zone = utc;
        if (withClient)
            options.client = std::make_shared<azl::S3Client>(s3.config());
        sync = std::make_unique<azl::MediaSync>(
            harness.document(), harness.library(), [this] { return harness.engine().composition(); },
            [this] { return now.load(); }, options, [this](const std::string& hash) {
                std::lock_guard lock(mutex);
                found.push_back(hash);
            });
        harness.onMediaStatus([this] { return sync->status(); });
    }

    // Object `i` as if copied in by cable.
    void import(size_t i)
    {
        const fs::path file = dir.path / "usb" / ("clip" + std::to_string(i) + ".mp4");
        fs::create_directories(file.parent_path());
        writeAll(file, objects[i]);
        harness.library().add(file);
    }

    Message status() const { return sync->status(); }

    bool reaches(const std::string& state)
    {
        return waitFor([&] { return status()["state"] == state; });
    }

    std::string downloaded(size_t i) { return readAll(harness.library().find(hashes[i])->path); }

    TempDir dir;
    FakeS3 s3;
    std::vector<std::string> objects;
    std::vector<std::string> hashes;
    azl::EngineHarness harness;
    std::atomic<double> now{kAfternoon};
    std::mutex mutex;
    std::vector<std::string> found;
    std::unique_ptr<azl::MediaSync> sync;
};

Message deckMissing(uint64_t deck, const std::string& name, int missing)
{
    return {{"deck", deck}, {"name", name}, {"missing", missing}};
}

Message machineMedia(Client& client)
{
    client.send({{"type", "machine"}, {"op", "status"}});
    for (;;)
        if (Message m = client.next(); m["type"] == "machine" || m["type"] == "error")
            return m["media"];
}

} // namespace

TEST_SUITE_BEGIN("media sync");

TEST_CASE("the needed set is every deck's media with its decks; text and generator clips need none")
{
    azl::Composition composition = twoDecks("aa", "bb", "cc");
    composition.layers[1].clips[3] = clip("aa", "a again.mp4");
    const std::vector<azl::NeededMedia> needed = azl::neededMedia(composition);
    CHECK(needed == std::vector<azl::NeededMedia>{{"aa", "a.mp4", {1, 2}}, {"bb", "b.mp4", {2}}, {"cc", "c.mov", {2}}});

    const azl::Readiness ready = azl::readiness(composition, needed, {"aa", "bb", "cc"});
    CHECK(ready.ready());
    CHECK(ready.needed == 3);
    CHECK(ready.decks.empty());
    const azl::Readiness two = azl::readiness(composition, needed, {"aa"});
    CHECK(two.missing == 2);
    CHECK(two.decks == std::vector<azl::DeckMissing>{{2, "Headliner", 2}});
    const azl::Readiness none = azl::readiness(composition, needed, {});
    CHECK(none.decks == std::vector<azl::DeckMissing>{{1, "Opening", 1}, {2, "Headliner", 3}});
}

TEST_CASE("downloads are allowed outside the Gig window, or when forced; a forced block wins")
{
    const std::vector<azl::GigWindow> gigs{kNight};
    using enum azl::SyncOverride;
    CHECK(azl::syncAllowed(gigs, kAfternoon, Auto, utc));
    CHECK_FALSE(azl::syncAllowed(gigs, kStart, Auto, utc));
    CHECK_FALSE(azl::syncAllowed(gigs, kStart + 6.9 * 3600, Auto, utc));
    CHECK(azl::syncAllowed(gigs, kStart + 7 * 3600, Auto, utc));
    CHECK(azl::syncAllowed(gigs, kStart, Allow, utc));
    CHECK_FALSE(azl::syncAllowed(gigs, kAfternoon, Block, utc));
}

TEST_CASE("only the missing objects come down, hash-verified, into the Media Library")
{
    Venue venue;
    venue.import(0);
    venue.start();
    REQUIRE(venue.reaches("ready"));
    const Message status = venue.status();
    CHECK(status["ready"] == true);
    CHECK(status["needed"] == 3);
    CHECK(status["missing"] == 0);
    CHECK(venue.s3.count("head") == 2);
    CHECK(venue.s3.count("get") == 2);
    for (size_t i : {1u, 2u}) {
        CHECK(venue.harness.library().find(venue.hashes[i])->path.parent_path() == venue.dir.path / "downloads");
        CHECK(venue.downloaded(i) == venue.objects[i]);
    }
    CHECK(venue.harness.library().find(venue.hashes[1])->status == azl::MediaStatus::Ok);
    CHECK(venue.harness.library().find(venue.hashes[1])->path.extension() == ".mp4");
    CHECK(venue.harness.library().find(venue.hashes[2])->path.extension() == ".mov");
    std::lock_guard lock(venue.mutex);
    CHECK(std::set<std::string>(venue.found.begin(), venue.found.end()) ==
          std::set<std::string>{venue.hashes[1], venue.hashes[2]});
}

TEST_CASE("inside the Gig window nothing downloads; a forced allow downloads and a forced block stops a download")
{
    Venue venue;
    venue.import(0);
    venue.import(1);
    venue.now = kStart + 600;
    venue.start({.chunk = 500, .bytesPerSecond = 9000});
    REQUIRE(venue.reaches("waiting"));
    CHECK(venue.status()["inWindow"] == true);
    CHECK(venue.status()["allowed"] == false);
    CHECK(venue.status()["missing"] == 1);
    std::this_thread::sleep_for(200ms);
    CHECK(venue.s3.count("get") == 0);

    venue.sync->setMode(azl::SyncOverride::Allow);
    REQUIRE(waitFor([&] {
        const Message download = venue.status()["download"];
        return download.is_object() && download["bytes"] >= 1000;
    }));
    CHECK(venue.status()["state"] == "downloading");
    venue.sync->setMode(azl::SyncOverride::Block);
    REQUIRE(venue.reaches("blocked"));
    CHECK(venue.status()["download"].is_null());
    const int gets = venue.s3.count("get");
    std::this_thread::sleep_for(300ms);
    CHECK(venue.s3.count("get") == gets);
    CHECK(fs::file_size(venue.dir.path / "downloads" / (venue.hashes[2] + ".part")) < 4500);
    CHECK(venue.status()["ready"] == false);

    venue.sync->setMode(azl::SyncOverride::Auto);
    REQUIRE(venue.reaches("waiting"));
    venue.now = kStart + 7 * 3600;
    venue.sync->wake();
    REQUIRE(venue.reaches("ready"));
    CHECK(venue.downloaded(2) == venue.objects[2]);
    const std::vector<std::string> ranges = venue.s3.ranges();
    CHECK(std::count(ranges.begin(), ranges.end(), "bytes=0-499") == 1);
    CHECK(ranges.size() == 9);
}

TEST_CASE("the override is set over the protocol, reaches every client and is kept on the venue")
{
    TempDir settings;
    const fs::path file = settings.path / "media-sync.json";
    {
        Venue venue;
        venue.now = kStart;
        venue.start({.settings = file});
        Client director(venue.harness.document());
        Client other(venue.harness.document());
        director.send({{"type", "mediaSync"}, {"op", "set"}, {"override", "block"}});
        const Message reply = director.next();
        CHECK(reply["type"] == "mediaSync");
        CHECK(reply["op"] == "status");
        CHECK(reply["override"] == "block");
        CHECK(other.next()["override"] == "block");
        director.send({{"type", "mediaSync"}, {"op", "set"}, {"override", "sometimes"}});
        CHECK(director.next()["type"] == "error");
        REQUIRE(venue.reaches("blocked"));
        CHECK(venue.s3.count("get") == 0);
    }
    Venue again;
    again.now = kStart;
    again.start({.settings = file});
    CHECK(again.sync->mode() == azl::SyncOverride::Block);
    Client director(again.harness.document());
    director.send({{"type", "mediaSync"}, {"op", "status"}});
    CHECK(director.next()["override"] == "block");
}

TEST_CASE("a corrupted object is discarded and fetched again; one that never verifies is an error")
{
    Venue venue;
    venue.s3.corrupt(venue.hashes[2], 1);
    venue.start();
    REQUIRE(venue.reaches("ready"));
    CHECK(venue.downloaded(2) == venue.objects[2]);
    CHECK(venue.s3.count("get") == 4);

    Venue bad;
    bad.s3.corrupt(bad.hashes[0], 100);
    bad.start({.attempts = 2});
    REQUIRE(waitFor([&] { return bad.status()["state"] == "error" && bad.status()["missing"] == 1; }));
    CHECK(bad.status()["error"].get<std::string>().find("arrived with another hash 2 times") != std::string::npos);
    CHECK(bad.status()["missing"] == 1);
    CHECK_FALSE(bad.harness.library().find(bad.hashes[0]));
}

TEST_CASE("a clip whose hash is not a content hash is never downloaded or used as a file name")
{
    Venue venue("../escape");
    venue.start();
    REQUIRE(waitFor([&] { return venue.status()["state"] == "error"; }));
    CHECK(venue.status()["error"].get<std::string>().find("has no media hash") != std::string::npos);
    CHECK_FALSE(fs::exists(venue.dir.path / "escape.part"));
    CHECK_FALSE(fs::exists(venue.dir.path / "escape"));
    for (const auto& entry : fs::recursive_directory_iterator(venue.dir.path))
        CHECK(entry.path().filename().string().find("escape") == std::string::npos);
}

TEST_CASE("a download cut off midway resumes where it stopped")
{
    Venue venue;
    venue.import(0);
    venue.import(1);
    venue.s3.dropAfterGets(2);
    venue.start({.chunk = 1000});
    REQUIRE(venue.reaches("error"));
    CHECK(venue.status()["error"].get<std::string>().find("connection lost") == 0);
    CHECK(fs::file_size(venue.dir.path / "downloads" / (venue.hashes[2] + ".part")) == 2000);

    venue.s3.restore();
    REQUIRE(venue.reaches("ready"));
    CHECK(venue.downloaded(2) == venue.objects[2]);
    CHECK(venue.s3.ranges() == std::vector<std::string>{"bytes=0-999", "bytes=1000-1999", "bytes=2000-2999",
                                                        "bytes=3000-3999", "bytes=4000-4499"});
}

TEST_CASE("files imported by cable count toward readiness; with no token the rest is an error")
{
    Venue venue;
    venue.start({.clientError = "no R2 settings"}, false);
    REQUIRE(venue.reaches("error"));
    CHECK(venue.status()["error"] == "no R2 settings");
    CHECK(venue.status()["missing"] == 3);

    Client director(venue.harness.document());
    for (size_t i = 0; i < 3; ++i) {
        const fs::path file = venue.dir.path / "usb" / ("clip" + std::to_string(i));
        fs::create_directories(file.parent_path());
        writeAll(file, venue.objects[i]);
        director.send({{"type", "library"}, {"op", "add"}, {"path", file.string()}});
        while (director.next()["type"] != "library") {
        }
    }
    venue.sync->wake();
    REQUIRE(venue.reaches("ready"));
    CHECK(venue.status()["missing"] == 0);
    CHECK(venue.s3.count("get") == 0);
}

TEST_CASE("readiness by deck shows in the machine status as objects arrive; the alert comes 2 h before the gig")
{
    Venue venue;
    venue.now = kStart - 2 * 3600 - 60;
    venue.start({}, true, azl::SyncOverride::Block);
    Client director(venue.harness.document());
    REQUIRE(venue.reaches("blocked"));
    Message media = machineMedia(director);
    CHECK(media["ready"] == false);
    CHECK(media["missing"] == 3);
    CHECK(media["decks"] == Message::array({deckMissing(1, "Opening", 1), deckMissing(2, "Headliner", 2)}));
    CHECK(media["alert"].is_null());

    venue.now = kStart - 2 * 3600 + 60;
    CHECK(machineMedia(director)["alert"] == "media not ready, gig at 22:00");

    venue.import(0);
    venue.sync->wake();
    REQUIRE(waitFor([&] { return machineMedia(director)["missing"] == 2; }));
    CHECK(machineMedia(director)["decks"] == Message::array({deckMissing(2, "Headliner", 2)}));

    venue.sync->setMode(azl::SyncOverride::Allow);
    REQUIRE(waitFor([&] { return machineMedia(director)["ready"] == true; }));
    media = machineMedia(director);
    CHECK(media["decks"].empty());
    CHECK(media["alert"].is_null());
    CHECK(media["override"] == "allow");

    venue.sync->setMode(azl::SyncOverride::Block);
    venue.now = kStart + 3600;
    fs::remove(venue.harness.library().find(venue.hashes[1])->path);
    venue.sync->wake();
    REQUIRE(waitFor([&] { return machineMedia(director)["missing"] == 1; }));
    CHECK(machineMedia(director)["alert"] == "media not ready, gig under way");
}

TEST_SUITE_END();
