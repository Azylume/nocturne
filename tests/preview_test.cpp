#include "effects/effect_registry.hpp"
#include "engine/gpu_profile.hpp"
#include "net/websocket.hpp"
#include "protocol/preview_surface.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <IOSurface/IOSurface.h>

#include <cstdio>
#include <future>
#include <set>
#include <thread>

namespace {

Message nextOfType(Client& client, const std::string& type, const std::string& op)
{
    for (;;) {
        Message m = client.next();
        if (m.value("type", "") == type && m.value("op", "") == op)
            return m;
    }
}

// The change the edit caused, past the pictures the client's watch sends meanwhile.
Message applyWatching(Client& client, const azl::Edit& edit)
{
    client.send(msg::command(edit));
    Message m = client.next();
    while (m.value("type", "") == "preview")
        m = client.next();
    REQUIRE_MESSAGE(m["type"] == "change", m.dump());
    return m;
}

bool isJpeg(const Message& bytes)
{
    const auto& data = bytes.get_binary();
    return data.size() > 100 && data[0] == 0xFF && data[1] == 0xD8;
}

Message watch(const std::vector<std::string>& sources, const std::string& via)
{
    return {{"type", "preview"}, {"op", "watch"}, {"sources", sources}, {"via", via},
            {"width", 160},      {"height", 90},  {"fps", 60}};
}

} // namespace

TEST_CASE("the media library hands out a JPEG still of a clip, and an error for an unknown hash")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    const std::string hash = added["entry"]["hash"];

    client.send({{"type", "library"}, {"op", "thumbnail"}, {"hash", hash}, {"width", 160}});
    const Message still = nextOfType(client, "library", "thumbnail");
    CHECK(still["hash"] == hash);
    REQUIRE(still.contains("jpeg"));
    CHECK(isJpeg(still["jpeg"]));

    client.send({{"type", "library"}, {"op", "thumbnail"}, {"hash", "0000"}});
    const Message missing = nextOfType(client, "library", "thumbnail");
    CHECK(missing.contains("error"));
    CHECK_FALSE(missing.contains("jpeg"));
}

TEST_CASE("a remote client watches the composition and a layer as JPEG pictures of new frames")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t layer = client.snapshot["composition"]["layers"][0]["id"];
    harness.step();

    client.send(watch({"composition", "layer:" + std::to_string(layer), "layer:999"}, "jpeg"));
    std::set<std::string> seen;
    uint64_t firstFrame = 0;
    while (seen.size() < 2) {
        const Message frame = nextOfType(client, "preview", "frame");
        CHECK(isJpeg(frame["jpeg"]));
        CHECK(frame["width"] == 160);
        seen.insert(frame["source"].get<std::string>());
        firstFrame = frame["frame"];
    }
    CHECK(seen == std::set<std::string>{"composition", "layer:" + std::to_string(layer)});

    harness.step();
    const Message later = nextOfType(client, "preview", "frame");
    CHECK(later["frame"].get<uint64_t>() >= firstFrame);

    client.send(watch({}, "jpeg"));
}

TEST_CASE("a local client reads preview pictures straight from shared IOSurfaces")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    harness.step();
    const azl::Image screen = harness.readScreen();

    client.send(watch({"composition"}, "surface"));
    const Message surfaces = nextOfType(client, "preview", "surfaces");
    CHECK(surfaces["source"] == "composition");
    REQUIRE(surfaces["surfaces"].size() == 3);
    const Message frame = nextOfType(client, "preview", "frame");
    CHECK_FALSE(frame.contains("jpeg"));

    IOSurfaceRef surface = IOSurfaceLookup(surfaces["surfaces"][frame["surface"].get<size_t>()].get<IOSurfaceID>());
    REQUIRE(surface);
    CFTypeRef marked = IOSurfaceCopyValue(surface, azl::previewFrameKey());
    REQUIRE(marked);
    int64_t markedFrame = -1;
    CFNumberGetValue(static_cast<CFNumberRef>(marked), kCFNumberSInt64Type, &markedFrame);
    CFRelease(marked);
    CHECK(markedFrame == frame["frame"].get<int64_t>());
    CHECK(IOSurfaceGetWidth(surface) == 160);
    CHECK(IOSurfaceGetHeight(surface) == 90);
    // The same picture as the screen at a smaller size: compare the mean colour (BGRA there, RGBA here).
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
    double small[3] = {}, full[3] = {};
    for (size_t y = 0; y < 90; ++y)
        for (size_t x = 0; x < 160; ++x)
            for (int c = 0; c < 3; ++c)
                small[c] += base[y * IOSurfaceGetBytesPerRow(surface) + x * 4 + (2 - c)] / (160.0 * 90.0);
    for (size_t i = 0; i < screen.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            full[c] += screen.rgba[i + c] / double(screen.width * screen.height);
    CHECK(full[0] + full[1] + full[2] > 30.0);
    for (int c = 0; c < 3; ++c)
        CHECK(std::abs(small[c] - full[c]) < 12.0);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    CFRelease(surface);

    client.send(watch({}, "surface"));
}

TEST_CASE("the engine counts the GPU time and passes of preview watches apart, and none once they end")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t layer = client.snapshot["composition"]["layers"][0]["id"];
    harness.step();
    auto& stats = harness.stats();
    CHECK(stats.previewPasses == 0);
    CHECK(harness.previewWatchers().empty());

    client.send(watch({"composition", "layer:" + std::to_string(layer)}, "jpeg"));
    nextOfType(client, "preview", "frame");
    // The preview passes' timestamps count once their ticks are calibrated, half a second in.
    const uint64_t rendered = stats.rendered, gpu = stats.gpuMicros;
    harness.runLive(1.0);
    const double frames = double(stats.rendered - rendered);
    MESSAGE("GPU " << (stats.gpuMicros - gpu) / 1000.0 / frames << " ms/frame, of it previews "
                   << stats.previewGpuMicros / 1000.0 / frames << " ms in the frame and "
                   << harness.previewPaintStats().gpuMicros / 1000.0 / frames << " ms painted");
    CHECK(stats.previewPasses == 1);
    CHECK(stats.previewGpuMicros > 0);
    CHECK(stats.previewGpuMicros < stats.gpuMicros);
    CHECK(harness.previewPaintStats().pictures > 0);
    CHECK(harness.previewPaintStats().gpuMicros > 0);
    CHECK(harness.previewWatchers() == std::map<std::string, size_t>{{"composition", 1}, {"layer", 1}});

    client.send(watch({}, "jpeg"));
    harness.runLive(0.2);
    CHECK(stats.previewPasses == 0);
    CHECK(harness.previewWatchers().empty());
    const uint64_t frameGpu = stats.previewGpuMicros, paintGpu = harness.previewPaintStats().gpuMicros;
    harness.runLive(0.3);
    CHECK(stats.previewGpuMicros == frameGpu);
    CHECK(harness.previewPaintStats().gpuMicros == paintGpu);
}

TEST_CASE("without counter sampling the preview timer falls back to the command buffer's GPU time")
{
    struct NoCounters {
        NoCounters() { azl::GpuProfile::forceNoCounterSampling(true); }
        ~NoCounters() { azl::GpuProfile::forceNoCounterSampling(false); }
    } noCounters;
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    CHECK_FALSE(azl::GpuProfile(device.get()).enabled());
    CHECK(std::string(azl::GpuProfile(device.get(), 1, true).mode()) == "command buffer");

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t layer = client.snapshot["composition"]["layers"][0]["id"];
    harness.step();
    auto& stats = harness.stats();
    client.send(watch({"composition", "layer:" + std::to_string(layer)}, "jpeg"));
    nextOfType(client, "preview", "frame");
    const uint64_t rendered = stats.rendered, gpu = stats.gpuMicros, previewGpu = stats.previewGpuMicros;
    harness.runLive(0.5);
    CHECK(stats.rendered - rendered > 10);
    CHECK(stats.previewPasses == 1);
    CHECK(stats.previewGpuMicros > previewGpu);
    CHECK(stats.previewGpuMicros - previewGpu < stats.gpuMicros - gpu);
    client.send(watch({}, "jpeg"));
}

namespace {

Message clipWatch(const std::vector<std::string>& hashes, const std::string& via, double fps = 30)
{
    std::vector<std::string> sources;
    for (const std::string& hash : hashes)
        sources.push_back("clip:" + hash);
    return {{"type", "preview"}, {"op", "watch"}, {"watch", "clips"}, {"sources", sources}, {"via", via},
            {"width", 160}, {"height", 90}, {"fps", fps}};
}

// The watch a UI sends for the clip it has selected in a cell.
Message cellWatch(const std::string& hash, uint64_t column, const std::string& via, double fps = 30)
{
    Message watch = clipWatch({hash}, via, fps);
    watch["clip"] = {{"layer", 1}, {"column", column}};
    return watch;
}

std::string addedHash(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["entry"]["status"] == "ok");
    return added["entry"]["hash"];
}

} // namespace

TEST_CASE("a clip that is not playing has a moving JPEG preview, sampled without the render's decoders")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    const std::string other = addedHash(client, copyMedia("count_100.mp4", media.path / "other.mp4"));
    harness.step();

    // A watch serves one clip, the first it names.
    client.send(watch({"composition"}, "jpeg"));
    client.send(clipWatch({hash, other}, "jpeg"));
    std::set<std::vector<uint8_t>> pictures;
    bool composition = false;
    for (int i = 0; i < 400 && (pictures.size() < 4 || !composition); ++i) {
        const Message frame = nextOfType(client, "preview", "frame");
        if (frame["source"] == "composition") {
            composition = true;
            continue;
        }
        REQUIRE(frame["source"] == "clip:" + hash);
        CHECK(isJpeg(frame["jpeg"]));
        CHECK(frame["width"] == 160);
        CHECK(frame["height"] == 90);
        pictures.insert(frame["jpeg"].get_binary());
    }
    CHECK(composition);
    CHECK(pictures.size() >= 4);
    CHECK(harness.decodeStats().frames == 0);
    CHECK(harness.clipPreviewStats().pictures > 0);

    client.send(clipWatch({}, "jpeg"));
    client.send(watch({}, "jpeg"));
}

TEST_CASE("a local client reads moving clip previews from shared IOSurfaces")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));

    client.send(clipWatch({hash}, "surface"));
    const Message surfaces = nextOfType(client, "preview", "surfaces");
    CHECK(surfaces["source"] == "clip:" + hash);
    REQUIRE(surfaces["surfaces"].size() == 3);
    std::set<std::vector<uint8_t>> pictures;
    for (int i = 0; i < 400 && pictures.size() < 3; ++i) {
        const Message frame = nextOfType(client, "preview", "frame");
        CHECK_FALSE(frame.contains("jpeg"));
        IOSurfaceRef surface = IOSurfaceLookup(surfaces["surfaces"][frame["surface"].get<size_t>()].get<IOSurfaceID>());
        REQUIRE(surface);
        CHECK(IOSurfaceGetWidth(surface) == 160);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
        const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
        // The top half shows the frame index as bits.
        pictures.insert(std::vector<uint8_t>(base, base + IOSurfaceGetBytesPerRow(surface) * 40));
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        CFRelease(surface);
    }
    CHECK(pictures.size() >= 3);

    client.send(clipWatch({}, "surface"));
}

TEST_CASE("clip previews are off by default: a clip watch gets nothing and samples nothing")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    harness.step();

    client.send(clipWatch({hash}, "jpeg"));
    client.send(watch({"composition"}, "jpeg"));
    for (int i = 0; i < 20; ++i) {
        harness.step();
        CHECK(nextOfType(client, "preview", "frame")["source"] == "composition");
    }
    CHECK(harness.clipPreviewStats().pictures == 0);
    CHECK(harness.clipPreviewStats().busyMicros == 0);

    client.send(watch({}, "jpeg"));
}

TEST_CASE("clip previews turn on and off live: off stops sampling and serving, on serves the same watch again")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    harness.step();
    const Message clips = {{"type", "preview"}, {"op", "clips"}};
    const auto turn = [&](bool on) {
        Message message = clips;
        message["on"] = on;
        client.send(message);
        CHECK(nextOfType(client, "preview", "clips")["on"] == on);
        CHECK(harness.clipPreviews() == on);
    };
    const auto clipFrames = [&](int wanted) {
        int seen = 0;
        for (int i = 0; i < 400 && seen < wanted; ++i)
            if (nextOfType(client, "preview", "frame")["source"] == "clip:" + hash)
                ++seen;
        return seen;
    };

    client.send(clips);
    CHECK(nextOfType(client, "preview", "clips")["on"] == false);
    client.send(clipWatch({hash}, "jpeg"));
    turn(true);
    CHECK(clipFrames(3) == 3);

    turn(false);
    const uint64_t sampled = harness.clipPreviewStats().pictures;
    client.send(watch({"composition"}, "jpeg"));
    for (int i = 0; i < 20; ++i) {
        harness.step();
        CHECK(nextOfType(client, "preview", "frame")["source"] == "composition");
    }
    // One picture may have been under way.
    CHECK(harness.clipPreviewStats().pictures <= sampled + 1);
    client.send(watch({}, "jpeg"));

    turn(true);
    CHECK(clipFrames(3) == 3);
    CHECK(harness.clipPreviewStats().pictures > sampled + 1);

    client.send(clipWatch({}, "jpeg"));
}

namespace {

struct ShowCost {
    double seconds = 0.0;
    double fps = 0.0;
    uint64_t skipped = 0;
    uint64_t misses = 0;
    double decodeBusy = 0.0;
    double gpuMs = 0.0;
    double gapMs = 0.0;
    uint64_t pictures = 0;
    double sampleBusy = 0.0;
    double largestPicture = 0.0;
    uint64_t effectPictures = 0;
    double effectGpuMs = 0.0;
    uint64_t previewCompiles = 0;
};

// Plays a clip live at 1080p for 4 s while a client watches the clip preview of another clip with Trails and Colorize
// on it, as the UI does for the selected clip, and adds Edge Detection to that clip halfway; previews on or off.
ShowCost showCost(bool previews)
{
    TempDir media;
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    if (previews)
        harness.enableClipPreviews();
    Client client(harness.document());
    const fs::path playing = copyMedia("count_240.mp4", media.path / "playing.mp4");
    playMedia(client, playing);
    const std::string selected = addedHash(client, copyMedia("count_100.mp4", media.path / "selected.mp4"));
    apply(client, azl::SetSlot{1, 2, azl::ClipRef{.hash = selected, .name = "selected",
                                                  .effects = {{0, "trails", {{"feedback", 0.8}}},
                                                              {0, "colorize", {{"hue", 0.3}}}}}});
    harness.engine().waitForCompiles();
    harness.step();

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto& stats = harness.stats();
    const uint64_t skipped = stats.skipped, misses = stats.videoMisses, rendered = stats.rendered;
    const uint64_t gpu = stats.gpuMicros, decodeBusy = harness.decodeStats().busyMicros;
    stats.maxGapMicros = 0;
    ShowCost cost;
    // The sampler's busy time grows a picture at a time; the largest step is at most the one picture whose rest can
    // fall after the window.
    const auto watchFor = [&](std::chrono::milliseconds span) {
        const auto end = std::chrono::steady_clock::now() + span;
        uint64_t busy = harness.clipPreviewStats().busyMicros;
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const uint64_t now = harness.clipPreviewStats().busyMicros;
            cost.largestPicture = std::max(cost.largestPicture, (now - busy) / 1e6);
            busy = now;
        }
    };
    const auto started = std::chrono::steady_clock::now();
    client.send(cellWatch(selected, 2, "jpeg", 4));
    watchFor(std::chrono::seconds(2));
    Client editor(harness.document());
    apply(editor, azl::AddEffect{1, std::nullopt, {0, "edgeDetection", {}}, 2});
    watchFor(std::chrono::seconds(2));
    cost.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const uint64_t made = stats.rendered - rendered;
    cost.fps = made / cost.seconds;
    cost.skipped = stats.skipped - skipped;
    cost.misses = stats.videoMisses - misses;
    cost.gpuMs = made ? (stats.gpuMicros - gpu) / 1000.0 / made : 0.0;
    cost.decodeBusy = (harness.decodeStats().busyMicros - decodeBusy) / 1e6 / cost.seconds;
    cost.gapMs = stats.maxGapMicros / 1000.0;
    cost.sampleBusy = harness.clipPreviewStats().busyMicros / 1e6 / cost.seconds;
    // The sampler and the effect pictures yield to the show, so on a loaded machine they take longer than the 4 s.
    const auto drawn = [&] {
        return harness.clipPreviewStats().pictures >= azl::PreviewService::kClipPictures &&
               harness.clipEffects()->stats().pictures > 0;
    };
    for (int i = 0; previews && i < 300 && !drawn(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    harness.stopLive();
    client.send(clipWatch({}, "jpeg"));
    cost.pictures = harness.clipPreviewStats().pictures;
    if (azl::ClipEffectsPreview* effects = harness.clipEffects()) {
        cost.effectPictures = effects->stats().pictures;
        cost.effectGpuMs = cost.effectPictures ? effects->stats().gpuMicros / 1000.0 / cost.effectPictures : 0.0;
        cost.previewCompiles = effects->compiler().stats().pipelines;
    }
    return cost;
}

void report(const std::string& name, const ShowCost& c)
{
    MESSAGE(name << ": render " << c.fps << " fps, " << c.skipped << " skipped, " << c.misses << " video misses, GPU "
                 << c.gpuMs << " ms/frame, longest gap " << c.gapMs << " ms, show decode busy " << c.decodeBusy * 100
                 << "%; clip pictures " << c.pictures << ", sampling busy " << c.sampleBusy * 100 << "% (largest picture "
                 << c.largestPicture * 1000 << " ms), "
                 << c.effectPictures << " drawn with effects at " << c.effectGpuMs << " ms GPU each, "
                 << c.previewCompiles << " preview pipelines");
}

} // namespace

TEST_CASE("the selected clip's preview never costs the show: no skipped frames or misses, on or off, a bounded share")
{
    // A loaded machine now and then costs a run a missed frame; a cost the previews cause every time shows again.
    const auto measured = [](bool previews) {
        const ShowCost first = showCost(previews);
        if (first.skipped == 0 && first.misses == 0 && first.fps > 55.0)
            return first;
        report("UNDECIDED: measured again after", first);
        return showCost(previews);
    };
    const ShowCost off = measured(false);
    const ShowCost on = measured(true);
    report("clip previews off", off);
    report("clip previews on", on);
    for (const ShowCost* c : {&off, &on}) {
        CHECK(c->skipped == 0);
        CHECK(c->misses == 0);
        CHECK(c->fps > 55.0);
    }
    CHECK(off.pictures == 0);
    CHECK(on.pictures >= azl::PreviewService::kClipPictures);
    CHECK(on.sampleBusy <= azl::PreviewService::kClipBusyShare + 0.05 + on.largestPicture / on.seconds);
    CHECK(off.effectPictures == 0);
    CHECK(on.effectPictures > 0);
    CHECK(on.previewCompiles > 0);
}

namespace {

void registerRedOnly()
{
    if (!azl::findEffect("testRedOnly"))
        azl::registerEffect({"testRedOnly", azl::StageKind::Pointwise, {}, false,
                             "\n    return float4(c.r, 0.0, 0.0, c.a);\n", {}});
}

struct Channels {
    int red = 0;
    int green = 0;
    int blue = 0;
};

// The brightest red, green and blue in the next clip preview picture read from its IOSurface.
Channels nextPicture(Client& client, const Message& surfaces)
{
    const Message frame = nextOfType(client, "preview", "frame");
    IOSurfaceRef surface = IOSurfaceLookup(surfaces["surfaces"][frame["surface"].get<size_t>()].get<IOSurfaceID>());
    REQUIRE(surface);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
    Channels max;
    for (size_t y = 0; y < IOSurfaceGetHeight(surface); ++y)
        for (size_t x = 0; x < IOSurfaceGetWidth(surface); ++x) {
            const uint8_t* p = base + y * IOSurfaceGetBytesPerRow(surface) + x * 4;
            max = {std::max(max.red, int(p[2])), std::max(max.green, int(p[1])), std::max(max.blue, int(p[0]))};
        }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    CFRelease(surface);
    return max;
}

bool redOnly(const Channels& c)
{
    return c.red > 100 && c.green <= 2 && c.blue <= 2;
}

} // namespace

TEST_CASE("the selected clip's preview shows the clip's own effects, Trails included, and not its layer's")
{
    registerRedOnly();
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    apply(client, azl::AddEffect{1, std::nullopt, {0, "colorize", {{"hue", 0.3}, {"saturation", 1.0}}}});
    apply(client, azl::SetSlot{1, 1, azl::ClipRef{.hash = hash, .name = "count",
                                                  .effects = {{0, "trails", {{"feedback", 0.5}}},
                                                              {0, "testRedOnly", {}}}}});

    client.send(cellWatch(hash, 1, "surface"));
    const Message surfaces = nextOfType(client, "preview", "surfaces");
    int shown = 0;
    for (int i = 0; i < 600 && shown < 4; ++i)
        shown = redOnly(nextPicture(client, surfaces)) ? shown + 1 : 0;
    CHECK(shown == 4);
    CHECK(harness.clipEffects()->stats().pictures >= 4);
    CHECK(harness.decodeStats().frames == 0);

    // The same media watched without naming its cell shows the bare media.
    client.send(clipWatch({hash}, "surface"));
    const Message bare = nextOfType(client, "preview", "surfaces");
    const Channels plain = nextPicture(client, bare);
    CHECK(plain.green > 100);
    client.send(clipWatch({}, "surface"));
}

TEST_CASE("an effect edit on the selected clip recompiles its preview off the render thread, the media showing meanwhile")
{
    registerRedOnly();
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    playMedia(client, copyMedia("count_100.mp4", media.path / "playing.mp4"));
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    apply(client, azl::SetSlot{1, 2, azl::ClipRef{.hash = hash, .name = "count"}});
    harness.engine().waitForCompiles();
    harness.step();

    client.send(cellWatch(hash, 2, "surface", 20));
    const Message surfaces = nextOfType(client, "preview", "surfaces");
    CHECK(nextPicture(client, surfaces).green > 100);

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto& stats = harness.stats();
    const uint64_t skipped = stats.skipped, misses = stats.videoMisses;
    azl::ChainCompiler& compiler = harness.clipEffects()->compiler();
    compiler.hold(true);
    Client editor(harness.document());
    apply(editor, azl::AddEffect{1, std::nullopt, {0, "testRedOnly", {}}, 2});
    const uint64_t rendered = stats.rendered;
    for (int i = 0; i < 10; ++i)
        CHECK_FALSE(redOnly(nextPicture(client, surfaces)));
    CHECK(stats.rendered > rendered + 10);
    CHECK(compiler.stats().pipelines == 0);

    compiler.hold(false);
    bool shown = false;
    for (int i = 0; i < 200 && !shown; ++i)
        shown = redOnly(nextPicture(client, surfaces));
    CHECK(shown);
    CHECK(compiler.stats().pipelines > 0);
    harness.stopLive();
    CHECK(stats.skipped == skipped);
    CHECK(stats.videoMisses == misses);
    client.send(clipWatch({}, "surface"));
}

TEST_CASE("clip preview memory stays bounded across many selections, effect edits and on/off toggles")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    harness.enableClipPreviews();
    Client client(harness.document());
    const std::string hash = addedHash(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    const std::vector<azl::Effect> base = {{0, "trails", {{"feedback", 0.5}}}, {0, "colorize", {{"hue", 0.3}}}};
    std::vector<azl::Effect> edged = base;
    edged.push_back({0, "edgeDetection", {}});
    const auto turn = [&](bool on) {
        client.send({{"type", "preview"}, {"op", "clips"}, {"on", on}});
        CHECK(nextOfType(client, "preview", "clips")["on"] == on);
    };

    std::map<std::string, size_t> warm;
    for (int round = 0; round < 60; ++round) {
        const uint64_t column = 1 + round % 2;
        applyWatching(client, azl::SetSlot{1, column, azl::ClipRef{.hash = hash, .name = "count",
                                                                   .effects = round % 3 ? base : edged}});
        Message selection = cellWatch(hash, column, "surface", 60);
        selection["width"] = 160 + 8 * (round % 12);
        client.send(selection);
        for (int frames = 0; frames < 3;)
            if (nextOfType(client, "preview", "frame")["source"] == "clip:" + hash)
                ++frames;
        if (round % 30 == 14) {
            turn(false);
            turn(true);
        }
        // Same picture width and chain as the last round, so allocated sizes compare like for like.
        if (round == 35)
            warm = harness.previewSizes();
    }
    const std::map<std::string, size_t> end = harness.previewSizes();
    for (const auto& [name, size] : end)
        MESSAGE(name << ": " << warm[name] << " after 36 selections, " << size << " after 60");

    CHECK(warm.at("clipStrips") > 1);
    CHECK(warm.at("clipPictures") > 0);
    CHECK(warm.at("previewEffectKB") > 0);
    CHECK(end.at("previewWatches") == 1);
    CHECK(end.at("previewSurfaces") == 3);
    CHECK(end.at("clipStrips") <= azl::PreviewService::kKeptClips + 1);
    CHECK(end.at("clipPictures") <= (azl::PreviewService::kKeptClips + 1) * azl::PreviewService::kClipPictures);
    CHECK(end.at("previewChains") <= warm.at("previewChains"));
    CHECK(end.at("previewSideTargets") + end.at("previewSideSpares") <=
          warm.at("previewSideTargets") + warm.at("previewSideSpares"));
    CHECK(end.at("previewEffectKB") <= warm.at("previewEffectKB"));
    CHECK(end.at("clipPictureKB") <= warm.at("clipPictureKB") * 3 / 2);

    turn(false);
    const std::map<std::string, size_t> off = harness.previewSizes();
    CHECK(off.at("clipStrips") == 0);
    CHECK(off.at("previewSurfaces") == 0);
    CHECK(off.at("previewEffectKB") == 0);
    client.send(clipWatch({}, "surface"));
}

namespace {

std::pair<int, std::string> runCtl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return {-1, out};
    char buffer[4096];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    const int status = pclose(pipe);
    return {WEXITSTATUS(status), out};
}

template <typename Done>
bool waitFor(Done done, std::chrono::seconds limit = std::chrono::seconds(20))
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace

TEST_CASE("ctl clippreview with the check's layer id and playing column opens one watch: watchers and preview GPU "
          "time rise, and drop when it ends")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    const Message layer = Client(harness.document()).snapshot["composition"]["layers"][0];
    REQUIRE(layer["playing"].is_object());
    const uint64_t id = layer["id"], column = layer["playing"]["column"];
    apply(client, azl::AddEffect{id, std::nullopt, {0, "colorize", {{"hue", 0.3}}}, column});
    harness.engine().waitForCompiles();
    azl::WebSocketServer server(harness.document(), 0);
    const std::string cell = std::to_string(id) + " " + std::to_string(column);
    harness.startLive();

    const auto [offCode, offOut] = runCtl(server.port(), "clippreview " + cell + " 1");
    CHECK_MESSAGE(offCode == 1, offOut.substr(offOut.rfind('}') + 1));
    CHECK(offOut.find("clip previews are off") != std::string::npos);
    CHECK(harness.previewWatchers().empty());

    harness.enableClipPreviews();
    auto watching = std::async(std::launch::async, [&] { return runCtl(server.port(), "clippreview " + cell + " 4"); });
    const uint64_t gpu = harness.clipEffects()->stats().gpuMicros;
    CHECK(waitFor([&] { return harness.previewWatchers() == std::map<std::string, size_t>{{"clip", 1}}; }));
    CHECK(waitFor([&] { return harness.clipEffects()->stats().gpuMicros > gpu; }));
    const auto [code, out] = watching.get();
    CHECK(code == 0);
    const size_t total = out.rfind("clip preview: ");
    REQUIRE(total != std::string::npos);
    CHECK(std::stoull(out.substr(total + 14)) > 0);

    CHECK(waitFor([&] { return harness.previewWatchers().empty(); }));
    harness.stopLive();
    const uint64_t ended = harness.clipEffects()->stats().gpuMicros;
    harness.runLive(0.5);
    CHECK(harness.clipEffects()->stats().gpuMicros == ended);
}
