#include "clips.hpp"
#include "media/media_library.hpp"

TEST_CASE("a clip added to the Media Library plays on a layer at the frame for its time")
{
    TempDir media;
    const fs::path file = copyMedia("count_709_tv.mp4", media.path / "count.mp4");
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());

    const Message added = addMedia(client, file);
    REQUIRE(added["type"] == "library");
    CHECK(added["entry"]["hash"] == azl::sha256File(file));
    CHECK(added["entry"]["name"] == "count.mp4");
    CHECK(added["entry"]["status"] == "ok");

    const Message change = setClip(client, added["entry"]["hash"], "count.mp4");
    CHECK(change["op"] == "setLayerClip");
    CHECK(change["clip"]["hash"] == added["entry"]["hash"]);

    // Clip time starts at the first frame rendered with the clip; a 30 fps clip at 60 fps shows frame floor(k / 2).
    harness.step();
    const azl::Image first = harness.readScreen();
    CHECK(shownFrame(first) == 0);
    checkPatches(first);
    CHECK(frameAfterStep(harness, 73) == 36);
    checkPatches(harness.readScreen());
}

TEST_CASE("a looping clip shows its last frame, then wraps to frame 0")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));

    CHECK(frameAfterStep(harness) == 0);
    CHECK(frameAfterStep(harness) == 1);
    CHECK(frameAfterStep(harness, 57) == 58);
    CHECK(frameAfterStep(harness) == 59);
    CHECK(frameAfterStep(harness) == 0);
    CHECK(frameAfterStep(harness) == 1);
    CHECK(frameAfterStep(harness, 58) == 59);
    CHECK(frameAfterStep(harness) == 0);
}

TEST_CASE("a BT.601 full-range clip converts with its own matrix and range")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    playMedia(client, copyMedia("count_601_pc.mp4", media.path / "count601.mp4"));

    CHECK(frameAfterStep(harness, 21) == 10);
    checkPatches(harness.readScreen());
}

TEST_CASE("a renamed and moved file is still found by its hash")
{
    TempDir media, library;
    const fs::path original = copyMedia("count_709_tv.mp4", media.path / "original.mp4");
    const fs::path moved = media.path / "sub" / "renamed.mp4";
    std::string hash;
    {
        azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, library.path);
        Client client(harness.document());
        hash = addMedia(client, original)["entry"]["hash"];
        fs::create_directories(moved.parent_path());
        fs::rename(original, moved);

        REQUIRE(setClip(client, hash, "original.mp4")["type"] == "change");
        CHECK(frameAfterStep(harness, 5) == 2);
        checkPatches(harness.readScreen());

        client.send({{"type", "library"}, {"op", "list"}});
        const Message list = client.next();
        REQUIRE(list["entries"].size() == 1);
        CHECK(list["entries"][0]["path"] == moved.string());
        CHECK(list["entries"][0]["name"] == "renamed.mp4");
        CHECK(list["entries"][0]["status"] == "ok");
    }

    azl::MediaLibrary reopened(library.path);
    const auto entry = reopened.find(hash);
    REQUIRE(entry);
    CHECK(entry->path == moved);

    fs::remove(moved);
    CHECK(reopened.find(hash)->status == azl::MediaStatus::Missing);
}

TEST_CASE("corrupt and unsupported files are flagged and never disturb playback")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const Message good = playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    CHECK(frameAfterStep(harness, 10) == 4);

    struct Broken {
        const char* file;
        const char* status;
    };
    std::vector<std::string> brokenHashes;
    for (const Broken& broken : {Broken{"truncated.mp4", "corrupt"}, Broken{"truncated_faststart.mp4", "corrupt"},
                                 Broken{"audio_only.m4a", "unsupported"}}) {
        const Message added = addMedia(client, copyMedia(broken.file, media.path / broken.file));
        INFO(broken.file << ": " << added.dump());
        REQUIRE(added["type"] == "library");
        CHECK(added["entry"]["status"] == broken.status);
        CHECK_FALSE(added["entry"]["reason"].get<std::string>().empty());
        brokenHashes.push_back(added["entry"]["hash"]);
        CHECK(frameAfterStep(harness, 2) == 5 + int(brokenHashes.size()) - 1);
    }

    // A layer switched to a clip that cannot play keeps its last picture, and every client hears why.
    for (const std::string& hash : brokenHashes) {
        REQUIRE(setClip(client, hash)["type"] == "change");
        CHECK(frameAfterStep(harness, 3) == 7);
        const Message failed = client.next();
        CHECK(failed["type"] == "error");
        CHECK(failed["hash"] == hash);
        CHECK(failed["layer"] == 1);
        checkPatches(harness.readScreen());
    }

    REQUIRE(setClip(client, good["hash"])["type"] == "change");
    CHECK(frameAfterStep(harness) == 0);
    CHECK(frameAfterStep(harness, 4) == 2);
}

TEST_CASE("a layer keeps the test card until its clip delivers a frame")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    harness.step();
    checkTestCard(harness.readScreen());

    REQUIRE(setClip(client, kUnknownHash)["type"] == "change");
    harness.step();
    checkTestCard(harness.readScreen());
}

TEST_CASE("setLayerClip is an undoable edit seen by every client")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client a(harness.document());
    Client b(harness.document());
    CHECK(a.snapshot["composition"]["layers"][0]["clip"].is_null());
    const Message entry = playMedia(a, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));

    const Message change = b.next();
    CHECK(change["op"] == "setLayerClip");
    CHECK(change["clip"]["hash"] == entry["hash"]);
    CHECK(change["clip"]["name"] == "count.mp4");
    CHECK(frameAfterStep(harness, 3) == 1);

    b.send(msg::undo());
    CHECK(b.next()["clip"].is_null());
    CHECK(a.next()["cause"] == "undo");
    harness.step();
    checkTestCard(harness.readScreen());

    b.send(msg::redo());
    CHECK(b.next()["clip"]["hash"] == entry["hash"]);
    CHECK(frameAfterStep(harness) == 0);

    b.send(msg::command(azl::SetLayerClip{1, std::nullopt}));
    CHECK(b.next()["clip"].is_null());

    b.send(msg::command(azl::SetLayerClip{1, azl::ClipRef{"not-a-hash", "x", {}}}));
    CHECK(b.next()["type"] == "error");
    Client late(harness.document());
    CHECK(late.snapshot["composition"]["layers"][0]["clip"].is_null());
}

TEST_CASE("library add rejects relative and unreadable paths")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    CHECK(addMedia(client, "relative.mp4")["type"] == "error");
    CHECK(addMedia(client, media.path / "absent.mp4")["type"] == "error");
    client.send({{"type", "library"}, {"op", "explode"}});
    CHECK(client.next()["type"] == "error");
}

namespace {

double meanDifference(const azl::Image& a, const azl::Image& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            sum += std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c]));
    return sum / double(a.rgba.size() / 4 * 3);
}

const azl::Transport kTimelineClip{azl::SyncMode::Timeline};

} // namespace

TEST_CASE("a clip with phone-style rotation metadata shows upright")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    auto picture = [&](const char* name) {
        playMedia(client, copyMedia(name, media.path / name), kTimelineClip);
        harness.step();
        return harness.readScreen();
    };
    const azl::Image stored = picture("count_709_tv.mp4");
    const azl::Image upright = picture("upright.mp4");
    const azl::Image rotated = picture("rotated.mp4");
    INFO("rotated vs upright " << meanDifference(rotated, upright) << ", vs as stored " << meanDifference(rotated, stored));
    CHECK(meanDifference(rotated, upright) < 3.0);
    CHECK(meanDifference(rotated, stored) > 30.0);
}

TEST_CASE("variable frame rate clips play in order, and a hole in the stream shows the next frame, never stalls")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());

    playMedia(client, copyMedia("count_vfr.mp4", media.path / "vfr.mp4"), kTimelineClip);
    std::vector<int> frames;
    for (int i = 0; i < 100; ++i)
        frames.push_back(frameAfterStep(harness));
    CHECK(std::is_sorted(frames.begin(), frames.end()));
    CHECK(frames.back() > 40);

    playMedia(client, copyMedia("count_gap.mp4", media.path / "gap.mp4"), kTimelineClip);
    const auto started = std::chrono::steady_clock::now();
    frames.clear();
    for (int i = 0; i < 60; ++i)
        frames.push_back(frameAfterStep(harness));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    // Render frame k shows clip time k / 60; 10/30 s to 20/30 s has no frame of its own.
    CHECK(frames[18] == 9);
    CHECK(frames[24] == 20);
    CHECK(frames[38] == 20);
    CHECK(frames[42] == 21);
    CHECK(harness.decodeStats().errors == 0);
}

namespace {

// A stuck read given up on frees its reader once it returns; later tests count readers.
bool readersBackTo(int64_t readers)
{
    const auto started = std::chrono::steady_clock::now();
    while (azl::liveAssetReaders() > readers && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return azl::liveAssetReaders() == readers;
}

} // namespace

TEST_CASE("a read stuck past its deadline is given up on, and the clip plays on with a new reader")
{
    // AVAssetReader can block in copyNextSampleBuffer for good, as on the Mac mini with the reference show on Random Jump.
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto stats = std::make_shared<azl::DecodeStats>();
    const int64_t readers = azl::liveAssetReaders();
    {
        azl::VideoDecoder decoder(device.get(), file.string(), {.stats = stats});
        decoder.injectStall(20, std::chrono::seconds(3));
        const auto started = std::chrono::steady_clock::now();
        for (int index = 0; index < 60; ++index)
            REQUIRE(decoder.waitFrame(index, true));
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500));
        CHECK(stats->stalls == 1);
        CHECK(stats->errors == 0);
        CHECK_FALSE(decoder.broken());
    }
    CHECK(readersBackTo(readers));
}

TEST_CASE("closing a decoder whose read is stuck returns at once, and the read ending later touches nothing of it")
{
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto stats = std::make_shared<azl::DecodeStats>();
    const int64_t readers = azl::liveAssetReaders();
    auto decoder = std::make_unique<azl::VideoDecoder>(
        device.get(), file.string(), azl::VideoDecoder::Options{.stats = stats, .readDeadline = std::chrono::seconds(30)});
    decoder->injectStall(5, std::chrono::milliseconds(1500));
    decoder->frame(0, true);
    const auto started = std::chrono::steady_clock::now();
    while (decoder->sizes().first < 5 && std::chrono::steady_clock::now() - started < std::chrono::seconds(3))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(decoder->sizes().first == 5);

    const auto closing = std::chrono::steady_clock::now();
    decoder.reset();
    CHECK(std::chrono::steady_clock::now() - closing < std::chrono::milliseconds(200));
    CHECK(stats->cachedBytes == 0);
    CHECK(stats->stalls == 0);

    // The stuck read returns on a cancelled reader, and the reader is freed.
    CHECK(readersBackTo(readers));
}

TEST_CASE("a decode thread left running at close counts nothing more into the shared stats")
{
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto stats = std::make_shared<azl::DecodeStats>();
    const int64_t readers = azl::liveAssetReaders();
    // Each frame read keeps the decode thread busy well past kCloseTimeout, so closing detaches it mid-frame.
    auto decoder = std::make_unique<azl::VideoDecoder>(
        device.get(), file.string(), azl::VideoDecoder::Options{.stats = stats, .frameCost = std::chrono::milliseconds(1500)});
    decoder->frame(0, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(stats->seeks == 1);
    REQUIRE(stats->frames == 0);

    const auto closing = std::chrono::steady_clock::now();
    decoder.reset();
    CHECK(std::chrono::steady_clock::now() - closing < azl::VideoDecoder::kCloseTimeout + std::chrono::milliseconds(200));
    const uint64_t maxRead = stats->maxReadMicros;

    // The detached thread finishes its frame and leaves; it frees its reader.
    CHECK(readersBackTo(readers));
    CHECK(stats->frames == 0);
    CHECK(stats->busyMicros == 0);
    CHECK(stats->seeks == 1);
    CHECK(stats->maxReadMicros == maxRead);
    CHECK(stats->cachedBytes == 0);
}

TEST_CASE("probing a file whose first read is stuck gives up after the deadline and marks it unplayable")
{
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    const int64_t readers = azl::liveAssetReaders();
    const auto started = std::chrono::steady_clock::now();
    const azl::ProbeResult result = azl::probeMedia(file, std::chrono::seconds(4));
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(took >= azl::kProbeDeadline);
    CHECK(took < azl::kProbeDeadline + std::chrono::milliseconds(500));
    CHECK(result.status == azl::MediaStatus::Corrupt);
    CHECK(result.reason.find("stalls the decoder") != std::string::npos);

    // The stuck read returns later, cancels its reader and frees it; the file itself probes fine.
    CHECK(readersBackTo(readers));
    CHECK(azl::probeMedia(file).status == azl::MediaStatus::Ok);
}

TEST_CASE("a layer whose decoder gets stuck in a read plays on within a fraction of a second")
{
    TempDir media;
    const int64_t readers = azl::liveAssetReaders();
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
        harness.startLive();
        const auto started = std::chrono::steady_clock::now();
        while (harness.decodeStats().frames == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(3))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const uint64_t coldStart = harness.stats().videoMisses;
        const uint64_t rendered = harness.stats().rendered;
        // At 30 fps the clip reaches frame 90 three seconds in, and the decoder reads it ahead. The harness's render
        // waits for a frame it missed, so frames not rendered count the time the layer had nothing: a reader stuck for
        // good would cost about two seconds of them.
        harness.injectDecoderStall(1, 90, std::chrono::seconds(3));
        std::this_thread::sleep_for(std::chrono::seconds(4));
        harness.stopLive();
        CHECK(harness.decodeStats().stalls == 1);
        CHECK(harness.stats().videoMisses - coldStart <= 3);
        CHECK(harness.stats().rendered - rendered > 200);
    }
    CHECK(readersBackTo(readers));
}

TEST_CASE("a parked clip decodes its first second at a quarter of real time, and starts playing with no burst")
{
    // A burst of decoding when a column starts delays the GPU's frames past their refresh on the Mac mini.
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto stats = std::make_shared<azl::DecodeStats>();
    azl::VideoDecoder decoder(device.get(), file.string(), {.stats = stats});
    decoder.park(0, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // 30 fps at a quarter of real time: the first frame at once, then about 3 in 400 ms.
    CHECK(stats->frames <= 6);
    decoder.waitIdle();
    const uint64_t parked = stats->frames;
    CHECK(parked == 30);

    REQUIRE(decoder.frame(0, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Its second ahead is there; from now on it reads on at 1.1x real time, a frame or two in 200 ms.
    CHECK(stats->frames - parked <= 10);
}

TEST_CASE("a BPM-synced clip's first-frame target does not lift the pace; a target elsewhere is decoded in the gaps")
{
    // Every BPM-synced clip prefetches its first frame, and Autopilot starts one every bar or two: unpaced, those starts
    // were bursts of 400-450 decoded frames/s that slowed the GPU on the Mac mini.
    TempDir media;
    const fs::path file = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto stats = std::make_shared<azl::DecodeStats>();
    azl::VideoDecoder decoder(device.get(), file.string(), {.stats = stats});
    decoder.park(0, true);
    decoder.waitIdle();
    const uint64_t parked = stats->frames;

    const std::vector<azl::FrameTarget> first{{0, true}};
    REQUIRE(decoder.frame(0, true, first));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(stats->frames - parked <= 12);

    // A target in another GOP is decoded at full speed while the playhead waits for its pace.
    const uint64_t before = stats->frames;
    const std::vector<azl::FrameTarget> jump{{0, true}, {150, true}};
    decoder.frame(1, true, jump);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(stats->frames - before >= 40);
    decoder.waitIdle();
    CHECK(decoder.frame(150, true, jump));
}
