#include "net/websocket.hpp"
#include "output/h264_stream.hpp"
#include "output/return_video.hpp"
#include "test_client.hpp"

#include <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <cmath>
#include <thread>

namespace {

namespace rv = azl::return_video;
using Clock = std::chrono::steady_clock;

// Whether this machine's hardware H.264 encoder takes the size: the Intel Mac mini's refuses less than 640 pixels of
// width, and the software encoder takes over.
bool hardwareEncodes(int width, int height)
{
    const void* keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef spec = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    VTCompressionSessionRef session = nullptr;
    const bool made = VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, spec, nullptr, nullptr,
                                                 nullptr, nullptr, &session) == noErr;
    CFRelease(spec);
    if (session) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
    }
    return made;
}

azl::Composition twoOutputs(azl::ClipRef clip = {})
{
    azl::Composition composition = azl::defaultComposition();
    composition.screens.push_back(
        {.id = 1, .name = "Projector", .display = "proj-a", .slices = {{.id = 1, .name = "Slice 1"}}});
    composition.screens.push_back({.id = 2,
                                   .name = "TV",
                                   .display = "tv-b",
                                   .testCard = true,
                                   .slices = {{.id = 2, .name = "Slice 2"}}});
    return composition;
}

void attach(azl::EngineHarness& harness, uint32_t width, uint32_t height)
{
    for (const char* id : {"proj-a", "tv-b"})
        harness.displays().attach({id, id, width, height, 60.0});
}

azl::ClipRef noise()
{
    return {.name = "noise", .effects = {{0, "noise", {{"scale", 32.0}, {"speed", 4.0}, {"contrast", 2.0}}}}};
}

void playNoise(Client& client)
{
    apply(client, azl::SetSlot{1, 1, noise()});
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 1}});
    while (client.next().value("op", "") != "triggerClip") {
    }
}

// An in-process Director: keeps every frame per output and the watch replies, never blocking the engine.
struct Viewer {
    explicit Viewer(azl::DocumentServer& server) : server(server)
    {
        id = server.connect([this](const Message& m) {
            if (m.value("type", "") != "video")
                return;
            std::lock_guard lock(mutex);
            if (m.value("op", "") == "watch")
                watches.push_back(m);
            if (const auto packet = rv::packetOf(m)) {
                bytes[m.value("display", "")] += packet->data.size() + packet->config.size();
                frames[m.value("display", "")].push_back(*packet);
            }
        });
        server.receive(id, msg::hello());
    }
    ~Viewer() { server.disconnect(id); }

    void watch(bool on) { server.receive(id, {{"type", "video"}, {"op", "watch"}, {"on", on}}); }
    size_t count(const std::string& display)
    {
        std::lock_guard lock(mutex);
        return frames[display].size();
    }
    std::map<std::string, uint64_t> sent()
    {
        std::lock_guard lock(mutex);
        return bytes;
    }
    void waitFor(const std::string& display, size_t n)
    {
        for (int i = 0; i < 300 && count(display) < n; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        REQUIRE(count(display) >= n);
    }
    // The newest picture of the output, decoded from its first frame on.
    azl::H264Decoder::Picture decoded(const std::string& display)
    {
        std::lock_guard lock(mutex);
        azl::H264Decoder decoder;
        std::optional<azl::H264Decoder::Picture> last;
        for (const rv::Packet& packet : frames[display])
            if (auto picture = decoder.decode(packet))
                last = std::move(picture);
        REQUIRE(last);
        return *last;
    }

    azl::DocumentServer& server;
    azl::ClientId id = 0;
    std::mutex mutex;
    std::vector<Message> watches;
    std::map<std::string, std::vector<rv::Packet>> frames;
    std::map<std::string, uint64_t> bytes;
};

// Mean brightness of each cell of a coarse grid, RGB in that order, robust to scaling and coding detail.
std::vector<double> cells(const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra)
{
    constexpr int kAcross = 16, kDown = 9;
    std::vector<double> sums(kAcross * kDown * 3, 0.0), counts(kAcross * kDown, 0.0);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const int cell = int(y * kDown / height) * kAcross + int(x * kAcross / width);
            const uint8_t* p = pixels + (size_t(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c)
                sums[cell * 3 + c] += p[bgra ? 2 - c : c];
            counts[cell] += 1.0;
        }
    for (size_t i = 0; i < sums.size(); ++i)
        sums[i] /= counts[i / 3];
    return sums;
}

double difference(const std::vector<double>& a, const std::vector<double>& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::abs(a[i] - b[i]);
    return sum / double(a.size());
}

} // namespace

TEST_CASE("the return video budget gives every output an equal share at 240 lines, at most 1 Mbit/s in all")
{
    const rv::Plan two = rv::plan({1920, 1080}, 2);
    CHECK(two.width == 426);
    CHECK(two.height == 240);
    CHECK(two.fps == rv::kFps);
    CHECK(two.kbps == 400);
    CHECK(rv::plan({1920, 1080}, 4).kbps == 250);
    CHECK(rv::plan({1920, 1080}, 4).kbps * 4 <= rv::kBudgetKbps);
    CHECK(rv::plan({1024, 768}, 1).width == 320);
    CHECK(rv::plan({1920, 1080}, 0, 60.0).fps == rv::kMaxFps);
}

TEST_CASE("return video packets become a raw H.264 stream, and a slow viewer drops frames until the next key frame")
{
    const rv::Packet packet{.config = {0, 0, 0, 2, 0x67, 1, 0, 0, 0, 1, 0x68}, .data = {0, 0, 0, 3, 0x65, 2, 3}, .key = true};
    CHECK(rv::nalUnits(packet.config).size() == 2);
    CHECK(rv::nalUnits({0, 0, 0, 9, 1}).empty());
    CHECK(rv::annexB(packet) ==
          std::vector<uint8_t>{0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x68, 0, 0, 0, 1, 0x65, 2, 3});
    const Message frame = rv::frameMessage("proj-a", 1, 7, rv::plan({1920, 1080}, 1), packet);
    const auto back = rv::packetOf(azl::decode(azl::encode(frame, azl::Encoding::MessagePack).data(),
                                               azl::encode(frame, azl::Encoding::MessagePack).size(),
                                               azl::Encoding::MessagePack)
                                       .value());
    REQUIRE(back);
    CHECK(back->config == packet.config);
    CHECK(back->data == packet.data);
    CHECK(back->key);

    rv::Gate gate;
    CHECK(gate.pass("a", true, false));
    CHECK(gate.pass("a", false, false));
    CHECK_FALSE(gate.pass("a", false, true));
    CHECK_FALSE(gate.pass("a", false, false));
    CHECK(gate.pass("b", false, false));
    CHECK(gate.pass("a", true, false));
    CHECK(gate.pass("a", false, false));
}

TEST_CASE("a Director watching gets every output as H.264 at 240 lines, in hardware where the encoder takes the size; "
          "the tiles match the outputs")
{
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, twoOutputs());
    attach(harness, 960, 540);
    Client client(harness.document());
    playMedia(client, mediaDir() / "count_240.mp4");
    harness.step(5);

    Viewer viewer(harness.document());
    viewer.watch(true);
    viewer.waitFor("proj-a", 5);
    viewer.waitFor("tv-b", 5);
    {
        std::lock_guard lock(viewer.mutex);
        REQUIRE(!viewer.watches.empty());
        const Message& watch = viewer.watches.front();
        CHECK(watch["on"] == true);
        CHECK(watch["width"] == 426);
        CHECK(watch["height"] == 240);
        CHECK(watch["kbps"] == 400);
        CHECK(watch["outputs"] == Message::array({{{"display", "proj-a"}, {"screen", 1}},
                                                 {{"display", "tv-b"}, {"screen", 2}}}));
        CHECK(viewer.watches.size() == 1);
        for (const auto& [display, packets] : viewer.frames) {
            CHECK(packets.front().key);
            CHECK_FALSE(packets.front().config.empty());
        }
    }
    CHECK(harness.returnVideo().watchers() == 1);

    Client asker(harness.document());
    asker.send({{"type", "video"}, {"op", "state"}});
    const Message state = asker.next();
    CHECK(state["watchers"] == 1);
    CHECK(state["outputs"] == 2);
    MESSAGE("hardware encoder: " << state["hardware"].dump());
    CHECK(state["hardware"] == hardwareEncodes(426, 240));

    const azl::Image projector = harness.readScreen(1), tv = harness.readScreen(2);
    const auto a = viewer.decoded("proj-a"), b = viewer.decoded("tv-b");
    CHECK(a.width == 426);
    CHECK(a.height == 240);
    const auto tileA = cells(a.bgra.data(), a.width, a.height, true);
    const auto tileB = cells(b.bgra.data(), b.width, b.height, true);
    const auto outA = cells(projector.rgba.data(), projector.width, projector.height, false);
    const auto outB = cells(tv.rgba.data(), tv.width, tv.height, false);
    MESSAGE("tile vs its output: " << difference(tileA, outA) << " and " << difference(tileB, outB)
                                   << "; vs the other output: " << difference(tileA, outB) << " and "
                                   << difference(tileB, outA));
    CHECK(difference(tileA, outA) < 8.0);
    CHECK(difference(tileB, outB) < 8.0);
    CHECK(difference(tileA, outB) > 3.0 * difference(tileA, outA));
    CHECK(difference(tileB, outA) > 3.0 * difference(tileB, outB));
}

TEST_CASE("with no watcher no video is encoded or sent; it stops when the last watcher stops or leaves")
{
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, twoOutputs());
    attach(harness, 960, 540);
    Client client(harness.document());
    harness.step();
    Viewer bystander(harness.document());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(harness.returnVideo().stats().pictures == 0);
    CHECK(harness.returnVideo().stats().bytes == 0);
    CHECK(bystander.sent().empty());

    {
        Viewer viewer(harness.document());
        viewer.watch(true);
        viewer.waitFor("proj-a", 3);
        viewer.watch(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const size_t stopped = viewer.count("proj-a");
        const uint64_t pictures = harness.returnVideo().stats().pictures;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(viewer.count("proj-a") == stopped);
        CHECK(harness.returnVideo().stats().pictures == pictures);
        CHECK(harness.returnVideo().watchers() == 0);
        viewer.watch(true);
        viewer.waitFor("proj-a", stopped + 2);
    }
    for (int i = 0; i < 50 && harness.returnVideo().watchers() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(harness.returnVideo().watchers() == 0);
    const uint64_t pictures = harness.returnVideo().stats().pictures;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(harness.returnVideo().stats().pictures == pictures);
    CHECK(bystander.sent().empty());
}

TEST_CASE("the return video of two outputs of moving noise stays inside its budget over 10 s")
{
    azl::EngineHarness harness({.width = 1920, .height = 1080}, 60.0, {}, twoOutputs());
    attach(harness, 1920, 1080);
    Client client(harness.document());
    playNoise(client);
    harness.step();
    Viewer viewer(harness.document());
    viewer.watch(true);
    viewer.waitFor("proj-a", 1);
    viewer.waitFor("tv-b", 1);
    const auto before = viewer.sent();
    const size_t frames = viewer.count("proj-a");
    harness.runLive(10.0);
    const auto after = viewer.sent();
    double total = 0.0;
    for (const std::string display : {"proj-a", "tv-b"}) {
        const double kbps = double(after.at(display) - before.at(display)) * 8.0 / 1000.0 / 10.0;
        MESSAGE(display << ": " << kbps << " kbit/s");
        CHECK(kbps <= rv::kMaxOutputKbps * 1.1);
        total += kbps;
    }
    MESSAGE("all outputs: " << total << " kbit/s at " << (viewer.count("proj-a") - frames) / 10.0
                            << " pictures/s per output");
    CHECK(total <= rv::kBudgetKbps);
    CHECK(total > 100.0);
    CHECK(viewer.count("proj-a") - frames >= size_t(rv::kFps * 10 * 0.8));
}

namespace {

struct ShowCost {
    double fps = 0.0;
    uint64_t skipped = 0;
    double gpuMs = 0.0;
    double gapMs = 0.0;
    uint64_t presents = 0;
    uint64_t hitches = 0;
    uint64_t pictures = 0;
    double videoGpuMs = 0.0;
    double encodeMs = 0.0;
};

// Plays moving noise at 1080p on two 1080p outputs for 4 s; with no watcher, an in-process one, or one over
// WebSocket that stops reading.
enum class Watcher { None, Reading, Stalled };

ShowCost showCost(Watcher watcher)
{
    azl::EngineHarness harness({.width = 1920, .height = 1080}, 60.0, {}, twoOutputs());
    attach(harness, 1920, 1080);
    Client client(harness.document());
    playNoise(client);
    harness.step();

    std::optional<Viewer> viewer;
    std::unique_ptr<azl::WebSocketServer> server;
    std::unique_ptr<azl::WebSocketClient> stalled;
    std::mutex gate;
    std::unique_lock held(gate, std::defer_lock);
    if (watcher == Watcher::Reading) {
        viewer.emplace(harness.document());
        viewer->watch(true);
    } else if (watcher == Watcher::Stalled) {
        server = std::make_unique<azl::WebSocketServer>(harness.document(), 0);
        held.lock();
        stalled = std::make_unique<azl::WebSocketClient>("127.0.0.1", server->port(), azl::Encoding::MessagePack,
                                                         [&](const Message& m) {
                                                             if (m.value("type", "") == "video")
                                                                 std::lock_guard wait(gate);
                                                         });
        stalled->send(msg::hello());
        stalled->send({{"type", "video"}, {"op", "watch"}, {"on", true}});
    }

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto& stats = harness.stats();
    auto& video = harness.returnVideo().stats();
    const auto presents = [&] {
        std::pair<uint64_t, uint64_t> sum;
        for (azl::DisplayOutput* output : harness.outputs().all()) {
            sum.first += output->stats().presents;
            sum.second += output->stats().hitches;
        }
        return sum;
    };
    const uint64_t rendered = stats.rendered, skipped = stats.skipped, gpu = stats.gpuMicros;
    const uint64_t pictures = video.pictures, videoGpu = video.gpuMicros, encode = video.encodeMicros;
    const auto [presented, hitched] = presents();
    stats.maxGapMicros = 0;
    const auto started = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    ShowCost cost;
    const uint64_t made = stats.rendered - rendered;
    cost.fps = made / seconds;
    cost.skipped = stats.skipped - skipped;
    cost.gpuMs = made ? (stats.gpuMicros - gpu) / 1000.0 / made : 0.0;
    cost.gapMs = stats.maxGapMicros / 1000.0;
    const auto [presentedNow, hitchedNow] = presents();
    cost.presents = presentedNow - presented;
    cost.hitches = hitchedNow - hitched;
    cost.pictures = video.pictures - pictures;
    cost.videoGpuMs = cost.pictures ? (video.gpuMicros - videoGpu) / 1000.0 / cost.pictures : 0.0;
    cost.encodeMs = cost.pictures ? (video.encodeMicros - encode) / 1000.0 / cost.pictures : 0.0;
    harness.stopLive();
    if (held.owns_lock())
        held.unlock();
    stalled.reset();
    server.reset();
    return cost;
}

void report(const std::string& name, const ShowCost& c)
{
    MESSAGE(name << ": render " << c.fps << " fps, " << c.skipped << " skipped, GPU " << c.gpuMs
                 << " ms/frame, longest gap " << c.gapMs << " ms; " << c.presents << " presents, " << c.hitches
                 << " hitches; return video " << c.pictures << " pictures, GPU " << c.videoGpuMs << " ms and encode "
                 << c.encodeMs << " ms each");
}

} // namespace

TEST_CASE("watching the return video, or a viewer that stops reading, never costs the show its frames")
{
    const auto measured = [](Watcher watcher) {
        const ShowCost first = showCost(watcher);
        if (first.skipped == 0 && first.fps > 55.0 && first.hitches * 50 <= first.presents)
            return first;
        report("UNDECIDED: measured again after", first);
        return showCost(watcher);
    };
    const ShowCost off = measured(Watcher::None);
    const ShowCost on = measured(Watcher::Reading);
    const ShowCost stalled = measured(Watcher::Stalled);
    report("no watcher", off);
    report("watching", on);
    report("stalled viewer", stalled);
    for (const ShowCost* c : {&off, &on, &stalled}) {
        CHECK(c->skipped == 0);
        CHECK(c->fps > 55.0);
        CHECK(c->hitches * 20 <= c->presents);
    }
    CHECK(off.pictures == 0);
    CHECK(on.pictures >= uint64_t(2 * rv::kFps * 4 * 0.8));
    CHECK(stalled.pictures >= uint64_t(2 * rv::kFps * 4 * 0.8));
    CHECK(on.gpuMs < off.gpuMs * 1.2 + 0.5);
    CHECK(stalled.gpuMs < off.gpuMs * 1.2 + 0.5);
}
