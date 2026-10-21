#include "test_client.hpp"
#include "test_paths.hpp"

#include "net/websocket.hpp"
#include "output/ycbcr_converter.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {

constexpr azl::EngineConfig kSmall{.width = 640, .height = 360};

struct Movie {
    FourCharCode codec = 0;
    std::vector<double> times;
    std::vector<azl::Image> frames;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
Movie decode(const fs::path& path)
{
    Movie movie;
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path.c_str())] options:nil];
    NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    REQUIRE(tracks.count == 1);
    auto format = (__bridge CMFormatDescriptionRef)tracks[0].formatDescriptions.firstObject;
    movie.codec = CMFormatDescriptionGetMediaSubType(format);
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
    AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:tracks[0]
                         outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)}];
    [reader addOutput:output];
    REQUIRE([reader startReading]);
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        if (pixels) {
            movie.times.push_back(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)));
            CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            const auto width = uint32_t(CVPixelBufferGetWidth(pixels));
            const auto height = uint32_t(CVPixelBufferGetHeight(pixels));
            const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
            const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
            azl::Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* bgra = base + y * stride + x * 4;
                    uint8_t* rgba = &image.rgba[(size_t(y) * width + x) * 4];
                    rgba[0] = bgra[2];
                    rgba[1] = bgra[1];
                    rgba[2] = bgra[0];
                    rgba[3] = 255;
                }
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            movie.frames.push_back(std::move(image));
        }
        CFRelease(sample);
    }
    return movie;
}
#pragma clang diagnostic pop

double meanError(const azl::Image& a, const azl::Image& b)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    uint64_t sum = 0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        if (i % 4 != 3)
            sum += uint64_t(std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    return double(sum) / double(a.rgba.size() / 4 * 3);
}

// The answer to the request; a client's earlier requests leave "state" broadcasts in its queue, so ask for the state
// with a fresh client.
Message recordReply(Client& client, const Message& request)
{
    client.send(request);
    for (;;) {
        Message m = client.next();
        if (m["type"] == "error" || (m["type"] == "record" && m["op"] == request["op"]))
            return m;
    }
}

// Counts the "stopped" messages every client gets.
class Notices {
public:
    explicit Notices(azl::DocumentServer& server)
        : server_(server), id_(server.connect([this](const Message& m) {
              if (m.value("type", "") == "record" && m.value("op", "") == "stopped") {
                  std::lock_guard lock(mutex_);
                  stopped_.push_back(m);
              }
          }))
    {
        server.receive(id_, msg::hello());
    }
    ~Notices() { server_.disconnect(id_); }

    std::vector<Message> stopped()
    {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    // The recorder reports itself stopped just before it broadcasts the notice.
    std::vector<Message> awaitStopped()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (stopped().empty() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return stopped();
    }

private:
    azl::DocumentServer& server_;
    azl::ClientId id_;
    std::mutex mutex_;
    std::vector<Message> stopped_;
};

azl::Composition keystoned()
{
    return {.layers = {{.id = 1, .name = "Layer 1"}},
            .screens = {{.id = 1,
                         .name = "Screen 1",
                         .slices = {{.id = 1, .corners = {0.2, 0.1, 0.8, 0.1, 0.95, 0.9, 0.05, 0.9}}}}}};
}

// Records `frames` frames of the moving test card and returns what each frame showed on the target.
// Without "precise" the encoder is handed 8-bit input, or BGRA where it names no format the recorder writes (the M2
// Pro's ProRes); which format depends on the machine's encoder.
std::vector<azl::Image> recordSequence(azl::EngineHarness& harness, Client& client, const Message& start, int frames)
{
    const Message started = recordReply(client, start);
    REQUIRE_MESSAGE(started["type"] == "record", started.dump());
    CHECK(started["recording"] == true);
    MESSAGE("the encoder takes ", started["pixels"]);
    if (!start.value("precise", false))
        CHECK((started["pixels"] == "420v" || started["pixels"] == "2vuy" || started["pixels"] == "BGRA"));
    std::vector<azl::Image> shown;
    const bool screen = start.value("target", "composition") != "composition";
    for (int i = 0; i < frames; ++i) {
        harness.step();
        harness.recorder().waitUntilWritten();
        shown.push_back(screen ? harness.readScreen(1) : harness.readScreen());
    }
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["recording"] == false);
    CHECK(stopped["frames"] == frames);
    CHECK(stopped["dropped"] == 0);
    return shown;
}

void checkMovie(const fs::path& file, const std::vector<azl::Image>& shown, FourCharCode codec, double tolerance)
{
    const Movie movie = decode(file);
    CHECK(movie.codec == codec);
    REQUIRE(movie.frames.size() == shown.size());
    double worst = 0.0;
    for (size_t i = 0; i < shown.size(); ++i) {
        CHECK(movie.times[i] == doctest::Approx(double(i) / 60.0).epsilon(1e-4));
        worst = std::max(worst, meanError(movie.frames[i], shown[i]));
    }
    MESSAGE("worst mean error per channel ", worst);
    CHECK(worst < tolerance);
    // The marker moves, so each frame is closer to its own picture than to one 20 frames away.
    CHECK(meanError(movie.frames.back(), shown.back()) < meanError(movie.frames.back(), shown[shown.size() - 21]));
}

struct DiskImage {
    DiskImage(int megabytes)
    {
        image = dir.path / "disk.dmg";
        mount = dir.path / "mount";
        fs::create_directories(mount);
        const std::string create = "hdiutil create -quiet -size " + std::to_string(megabytes) +
                                   "m -fs HFS+ -volname azlrec '" + image.string() + "'";
        REQUIRE(std::system(create.c_str()) == 0);
        const std::string attach =
            "hdiutil attach -quiet -nobrowse -mountpoint '" + mount.string() + "' '" + image.string() + "'";
        REQUIRE(std::system(attach.c_str()) == 0);
    }
    ~DiskImage()
    {
        const std::string detach = "hdiutil detach -quiet -force '" + mount.string() + "'";
        std::system(detach.c_str());
    }

    TempDir dir;
    fs::path image;
    fs::path mount;
};

// Steps until the recording ends by itself.
void recordUntilEnded(azl::EngineHarness& harness)
{
    for (int i = 0; i < 3000 && harness.recorder().status().recording; ++i) {
        harness.step();
        harness.recorder().waitUntilWritten();
    }
    REQUIRE_FALSE(harness.recorder().status().recording);
}

} // namespace

TEST_CASE("the composition records to HEVC and decodes back to the frames rendered")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    const fs::path file = dir.path / "composition.mov";
    const auto shown = recordSequence(
        harness, client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()}}, 60);
    CHECK(fs::exists(file));
    CHECK_FALSE(fs::exists(dir.path / ".composition.partial.mov"));
    checkMovie(file, shown, kCMVideoCodecType_HEVC, 4.0);
}

TEST_CASE("precise recording hands the encoders their own precision")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    for (const std::string codec : {"hevc", "prores"}) {
        CAPTURE(codec);
        const fs::path file = dir.path / (codec + ".mov");
        const auto shown = recordSequence(
            harness, client,
            {{"type", "record"}, {"op", "start"}, {"codec", codec}, {"precise", true}, {"path", file.string()}}, 30);
        CHECK(recordReply(client, {{"type", "record"}, {"op", "state"}})["precise"] == true);
        checkMovie(file, shown, codec == "hevc" ? kCMVideoCodecType_HEVC : kCMVideoCodecType_AppleProRes422,
                   codec == "hevc" ? 4.0 : 2.0);
    }
}

TEST_CASE("a screen records through its mapping to ProRes 422")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall, 60.0, {}, keystoned());
    Client client(harness.document());
    const fs::path file = dir.path / "screen.mov";
    const auto shown =
        recordSequence(harness, client,
                       {{"type", "record"}, {"op", "start"}, {"target", "screen:1"}, {"codec", "prores"},
                        {"path", file.string()}},
                       60);
    checkMovie(file, shown, kCMVideoCodecType_AppleProRes422, 2.0);
}

TEST_CASE("recording settings, state and refusals go through the protocol")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());

    Message state = recordReply(client, {{"type", "record"}, {"op", "state"}});
    CHECK(state["recording"] == false);
    CHECK(state["codec"] == "hevc");
    CHECK(state["target"] == "composition");

    state = recordReply(client, {{"type", "record"}, {"op", "set"}, {"codec", "prores"}, {"path", dir.path.string()}});
    CHECK(state["codec"] == "prores");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"codec", "h264"}})["type"] == "error");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "start"}, {"target", "screen:9"}})["type"] == "error");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"target", "composition"}})["type"] == "record");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "start"}, {"path", (dir.path / "x.mp4").string()}})["type"] ==
          "error");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"path", dir.path.string()}})["type"] == "record");

    state = recordReply(client, {{"type", "record"}, {"op", "start"}});
    REQUIRE(state["type"] == "record");
    CHECK(state["recording"] == true);
    const fs::path file = state["file"].get<std::string>();
    CHECK(file.parent_path() == dir.path);
    CHECK(file.extension() == ".mov");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "start"}})["type"] == "error");
    harness.step(5);
    harness.recorder().waitUntilWritten();
    state = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(state["frames"] == 5);
    CHECK(fs::exists(file));
    CHECK(recordReply(client, {{"type", "record"}, {"op", "start"}, {"path", file.string()}})["type"] == "error");
}

TEST_CASE("a missing or unwritable recording folder refuses the start with the reason, and the state names it")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    const fs::path missing = dir.path / "unplugged";
    Message state = recordReply(client, {{"type", "record"}, {"op", "set"}, {"path", missing.string()}});
    CHECK(state["folder"] == missing.string());
    CHECK(state["folderProblem"].get<std::string>().find("does not exist") != std::string::npos);
    Message refused = recordReply(client, {{"type", "record"}, {"op", "start"}});
    REQUIRE(refused["type"] == "error");
    CHECK(refused["message"] == "the recording folder " + missing.string() + " does not exist (is its disk connected?)");
    CHECK_FALSE(fs::exists(missing));
    refused = recordReply(client, {{"type", "record"}, {"op", "start"}, {"path", (missing / "take.mov").string()}});
    CHECK(refused["message"].get<std::string>().find("does not exist") != std::string::npos);

    const fs::path locked = dir.path / "locked";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    state = recordReply(client, {{"type", "record"}, {"op", "set"}, {"path", locked.string()}});
    CHECK(state["folderProblem"] == "cannot write to the recording folder " + locked.string());
    refused = recordReply(client, {{"type", "record"}, {"op", "start"}});
    REQUIRE(refused["type"] == "error");
    CHECK(refused["message"] == "cannot write to the recording folder " + locked.string());
    CHECK(fs::is_empty(locked));
    fs::permissions(locked, fs::perms::owner_all);

    state = recordReply(client, {{"type", "record"}, {"op", "set"}, {"path", locked.string()}});
    CHECK(state["folderProblem"] == "");
    CHECK(recordReply(client, {{"type", "record"}, {"op", "start"}})["recording"] == true);
    harness.step(3);
    harness.recorder().waitUntilWritten();
    CHECK(recordReply(client, {{"type", "record"}, {"op", "stop"}})["frames"] == 3);
}

TEST_CASE("the engine lists a folder's subfolders and the mounted volumes with free space, never files")
{
    TempDir root;
    const fs::path folder = root.path / "shows";
    const fs::path volumes = root.path / "Volumes";
    for (const fs::path& made : {folder / "beta", folder / "Alpha", folder / ".hidden", volumes / "Stage SSD"})
        fs::create_directories(made);
    std::ofstream(folder / "notes.txt") << "not listed";
    fs::create_symlink(root.path, volumes / "Macintosh HD");

    const Message list = azl::listFolders(folder, volumes);
    const fs::path real = fs::weakly_canonical(folder);
    CHECK(list["path"] == real.string());
    CHECK(list["parent"] == real.parent_path().string());
    CHECK(list["writable"] == true);
    CHECK(list["freeMB"].get<uint64_t>() > 0);
    REQUIRE(list["folders"].size() == 2);
    CHECK(list["folders"][0]["name"] == "Alpha");
    CHECK(list["folders"][0]["path"] == (real / "Alpha").string());
    CHECK(list["folders"][1]["name"] == "beta");
    REQUIRE(list["volumes"].size() == 2);
    CHECK(list["volumes"][0]["name"] == "Macintosh HD");
    CHECK(list["volumes"][1]["name"] == "Stage SSD");
    CHECK(list["volumes"][1]["totalMB"].get<uint64_t>() > 0);
    CHECK(list.dump().find("notes.txt") == std::string::npos);
    CHECK_THROWS(azl::listFolders(folder / "notes.txt", volumes));

    azl::EngineHarness harness(kSmall);
    harness.recordService().setVolumes(volumes);
    Client client(harness.document());
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "set"}, {"path", folder.string()}})["type"] == "record");
    Message reply = recordReply(client, {{"type", "record"}, {"op", "folders"}});
    CHECK(reply["path"] == real.string());
    CHECK(reply["folders"].size() == 2);
    CHECK(reply["volumes"].size() == 2);
    reply = recordReply(client, {{"type", "record"}, {"op", "folders"}, {"path", (folder / "Alpha").string()}});
    CHECK(reply["path"] == (real / "Alpha").string());
    CHECK(reply["folders"].empty());
    CHECK(recordReply(client, {{"type", "record"}, {"op", "folders"}, {"path", (folder / "gone").string()}})["type"] ==
          "error");
}

TEST_CASE("a full disk stops the recording cleanly with one notice, and the show goes on")
{
    DiskImage disk(24);
    azl::EngineHarness harness(kSmall);
    harness.recorder().setSpaceCheck({}, std::chrono::milliseconds(5));
    Client client(harness.document());
    Notices notices(harness.document());

    SUBCASE("below the reserve, the movie is finished and kept")
    {
        const fs::path file = disk.mount / "full.mov";
        REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "prores"},
                                     {"path", file.string()}, {"reserveMB", 8}})["type"] == "record");
        recordUntilEnded(harness);
        const auto stopped = notices.awaitStopped();
        REQUIRE(stopped.size() == 1);
        CHECK(stopped[0]["reason"] == "disk full");
        CHECK(stopped[0]["kept"] == true);
        CHECK(stopped[0].contains("notice"));
        const uint64_t frames = stopped[0]["frames"];
        CHECK(frames > 30);
        CHECK(decode(file).frames.size() == frames);
    }
    SUBCASE("with no reserve, the write that fails ends it and leaves no partial file")
    {
        const fs::path file = disk.mount / "overflow.mov";
        REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "prores"},
                                     {"path", file.string()}, {"reserveMB", 0}})["type"] == "record");
        recordUntilEnded(harness);
        const auto stopped = notices.awaitStopped();
        REQUIRE(stopped.size() == 1);
        CHECK(stopped[0]["reason"] == "disk full");
        CHECK(stopped[0]["kept"] == false);
        CHECK_FALSE(fs::exists(file));
        CHECK_FALSE(fs::exists(disk.mount / ".overflow.partial.mov"));
    }

    const uint64_t rendered = harness.stats().rendered;
    harness.step(30);
    CHECK(harness.stats().rendered == rendered + 30);
    CHECK(notices.stopped().size() == 1);
    Client fresh(harness.document());
    CHECK(recordReply(fresh, {{"type", "record"}, {"op", "state"}})["recording"] == false);
}

namespace {

constexpr uint64_t kGB = uint64_t(1) << 30;

// A disk of 1000 GB whose free space the test sets, checked every millisecond.
struct FakeDisk {
    explicit FakeDisk(azl::Recorder& recorder)
    {
        recorder.setSpaceCheck([this](const fs::path&) { return azl::Recorder::Space{free.load(), 1000 * kGB}; },
                               std::chrono::milliseconds(1));
    }
    std::atomic<uint64_t> free{100 * kGB};
};

// Steps one frame at a time, so the recorder's buffers never run out.
void stepWritten(azl::EngineHarness& harness, int frames)
{
    for (int i = 0; i < frames; ++i) {
        harness.step();
        harness.recorder().waitUntilWritten();
    }
}

template <typename Done>
bool waitFor(Done done)
{
    for (int i = 0; i < 2000 && !done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done();
}

std::vector<fs::path> movies(const fs::path& folder)
{
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(folder))
        found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

std::pair<int, std::string> runCtl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    const int status = pclose(pipe);
    return {WEXITSTATUS(status), out};
}

} // namespace

TEST_CASE("the disk reserve: a start below it is refused, twice it warns, below it the recording stops with one notice")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    FakeDisk disk(harness.recorder());
    Client client(harness.document());
    Notices notices(harness.document());
    const fs::path file = dir.path / "reserve.mov";
    // The larger of 1 GB and 1 % of 1000 GB.
    const Message settings{{"type", "record"}, {"op", "set"},          {"codec", "hevc"},
                           {"path", file.string()}, {"reserveMB", 1024}, {"reservePercent", 1}};
    REQUIRE(recordReply(client, settings)["type"] == "record");

    disk.free = 9 * kGB;
    const Message refused = recordReply(client, {{"type", "record"}, {"op", "start"}});
    REQUIRE(refused["type"] == "error");
    CHECK(refused["message"].get<std::string>().find("below the recording reserve of 10240 MB") != std::string::npos);
    CHECK_FALSE(fs::exists(file));

    disk.free = 100 * kGB;
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}})["recording"] == true);
    REQUIRE(waitFor([&] { return harness.recorder().status().space == azl::Recorder::SpaceLevel::Ok; }));
    stepWritten(harness, 10);

    disk.free = 19 * kGB;
    REQUIRE(waitFor([&] { return harness.recorder().status().space == azl::Recorder::SpaceLevel::Low; }));
    stepWritten(harness, 10);
    Client fresh(harness.document());
    const Message low = recordReply(fresh, {{"type", "record"}, {"op", "state"}});
    CHECK(low["recording"] == true);
    CHECK(low["space"] == "low");
    CHECK(low["freeMB"] == 19 * 1024);
    CHECK(low["stopsAtMB"] == 10240);
    CHECK(notices.stopped().empty());

    disk.free = 9 * kGB;
    REQUIRE(waitFor([&] { return !harness.recorder().status().recording; }));
    const auto stopped = notices.awaitStopped();
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0]["reason"] == "disk full");
    CHECK(stopped[0]["kept"] == true);
    CHECK(stopped[0].contains("notice"));
    CHECK(stopped[0]["frames"] == 20);
    CHECK(stopped[0]["dropped"] == 0);
    CHECK(decode(file).frames.size() == 20);
    CHECK(recordReply(fresh, {{"type", "record"}, {"op", "state"}})["space"] == "full");

    const uint64_t rendered = harness.stats().rendered;
    harness.step(30);
    CHECK(harness.stats().rendered == rendered + 30);
    CHECK(notices.stopped().size() == 1);
}

TEST_CASE("past its maximum length a recording goes on in the next numbered file without losing a frame")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    const fs::path file = dir.path / "roll.mov";
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()},
                                 {"maxSeconds", 1.0}})["file"] == (dir.path / "roll 001.mov").string());
    constexpr int kFrames = 150;
    stepWritten(harness, kFrames);
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["frames"] == kFrames);
    CHECK(stopped["dropped"] == 0);
    CHECK(stopped["file"] == (dir.path / "roll 003.mov").string());

    const std::vector<fs::path> files = movies(dir.path);
    REQUIRE(files == std::vector<fs::path>{dir.path / "roll 001.mov", dir.path / "roll 002.mov",
                                           dir.path / "roll 003.mov"});
    const std::vector<size_t> expected{60, 60, 30};
    for (size_t i = 0; i < files.size(); ++i) {
        CAPTURE(files[i]);
        const Movie movie = decode(files[i]);
        REQUIRE(movie.frames.size() == expected[i]);
        CHECK(movie.times.front() == doctest::Approx(0.0));
        CHECK(movie.times.back() == doctest::Approx(double(expected[i] - 1) / 60.0).epsilon(1e-4));
    }
}

TEST_CASE("past its maximum size a recording goes on in the next numbered file without losing a frame")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "prores"},
                                 {"path", (dir.path / "size.mov").string()}, {"maxMB", 2}})["type"] == "record");
    constexpr int kFrames = 400;
    stepWritten(harness, kFrames);
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["frames"] == kFrames);
    CHECK(stopped["dropped"] == 0);

    const std::vector<fs::path> files = movies(dir.path);
    MESSAGE(files.size(), " files");
    REQUIRE(files.size() >= 2);
    size_t frames = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "size %03zu.mov", i + 1);
        CHECK(files[i] == dir.path / name);
        frames += decode(files[i]).frames.size();
    }
    CHECK(frames == size_t(kFrames));
}

TEST_CASE("nocturne-ctl sets the reserve in MB or percent and the maximum size and length")
{
    azl::EngineHarness harness(kSmall);
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    CHECK(runCtl(port, "record reserve 512").first == 0);
    CHECK(runCtl(port, "record reserve 5%").first == 0);
    CHECK(runCtl(port, "record max-size 4096").first == 0);
    CHECK(runCtl(port, "record max-length 600").first == 0);
    const auto maxTotal = [&] {
        Client fresh(harness.document());
        return recordReply(fresh, {{"type", "record"}, {"op", "state"}})["maxTotalSeconds"];
    };
    CHECK(maxTotal() == 8 * 3600.0);
    CHECK(runCtl(port, "record max-total 3600").first == 0);
    CHECK(maxTotal() == 3600.0);
    CHECK(runCtl(port, "record max-total off").first == 0);
    CHECK(runCtl(port, "record gig-grace 600").first == 0);
    Client client(harness.document());
    const Message state = recordReply(client, {{"type", "record"}, {"op", "state"}});
    CHECK(state["maxTotalSeconds"] == 0.0);
    CHECK(state["gigGraceSeconds"] == 600.0);
    CHECK(state["reserveMB"] == 512);
    CHECK(state["reservePercent"] == 5.0);
    CHECK(state["maxMB"] == 4096);
    CHECK(state["maxSeconds"] == 600.0);
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"reservePercent", 95}})["type"] == "error");
}

namespace {

constexpr double kNow = 1'800'000'000.0;

int utcZone(int64_t)
{
    return 0;
}

Message startedAt(azl::EngineHarness& harness, Client& client, double now, Message start)
{
    harness.setWallClock(now, utcZone);
    start["type"] = "record";
    start["op"] = "start";
    start["audio"] = false;
    const Message started = recordReply(client, start);
    REQUIRE_MESSAGE(started["recording"] == true, started.dump());
    return started;
}

} // namespace

TEST_CASE("a recording finishes cleanly at its maximum total length, counted across its rolled-over files")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    Notices notices(harness.document());
    const Message started = startedAt(harness, client, kNow,
                                      {{"path", (dir.path / "total.mov").string()}, {"maxSeconds", 0.5},
                                       {"maxTotalSeconds", 1.0}});
    CHECK(started["stopsAt"] == kNow + 1.0);
    CHECK(started["stopsFor"] == "max length");
    CHECK(started["stopsAtClock"] == azl::clockText(azl::localMinute(int64_t(kNow) + 1, utcZone)));

    recordUntilEnded(harness);
    const auto stopped = notices.awaitStopped();
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0]["reason"] == "max length");
    CHECK(stopped[0]["kept"] == true);
    CHECK(stopped[0]["notice"] == "Recording finished: it reached its maximum length.");
    CHECK(stopped[0]["frames"] == 60);
    CHECK(stopped[0]["dropped"] == 0);
    const std::vector<fs::path> files = movies(dir.path);
    REQUIRE(files == std::vector<fs::path>{dir.path / "total 001.mov", dir.path / "total 002.mov"});
    CHECK(decode(files[0]).frames.size() == 30);
    CHECK(decode(files[1]).frames.size() == 30);
}

TEST_CASE("a recording started inside a Gig window finishes at the window's end plus the grace")
{
    TempDir dir;
    azl::Composition venue;
    venue.gigs = {{.date = "2026-10-16", .start = 20 * 60, .end = 2 * 60}};
    azl::EngineHarness harness(kSmall, 60.0, {}, venue);
    Client client(harness.document());
    Notices notices(harness.document());
    const double end = double(*azl::dateDays("2026-10-17") * 86400 + 2 * 3600);

    SUBCASE("inside the window")
    {
        const Message started = startedAt(harness, client, end - 0.5,
                                          {{"path", (dir.path / "gig.mov").string()}, {"gigGraceSeconds", 1.0}});
        CHECK(started["stopsAt"] == end + 1.0);
        CHECK(started["stopsFor"] == "gig over");
        CHECK(started["stopsAtClock"] == "02:00");
        recordUntilEnded(harness);
        const auto stopped = notices.awaitStopped();
        REQUIRE(stopped.size() == 1);
        CHECK(stopped[0]["reason"] == "gig over");
        CHECK(stopped[0]["kept"] == true);
        CHECK(stopped[0]["notice"] == "Recording finished: the gig is over.");
        CHECK(stopped[0]["frames"] == 90);
        CHECK(decode(dir.path / "gig.mov").frames.size() == 90);
    }
    SUBCASE("the maximum length when it comes first")
    {
        const Message started = startedAt(harness, client, end - 3600,
                                          {{"path", (dir.path / "short.mov").string()}, {"maxTotalSeconds", 600}});
        CHECK(started["stopsAt"] == end - 3000);
        CHECK(started["stopsFor"] == "max length");
        CHECK(started["stopsAtClock"] == "01:10");
        recordReply(client, {{"type", "record"}, {"op", "stop"}});
    }
    SUBCASE("outside the window only the maximum length applies")
    {
        const Message started = startedAt(harness, client, end + 60, {{"path", (dir.path / "after.mov").string()}});
        CHECK(started["stopsAt"] == end + 60 + 8 * 3600);
        CHECK(started["stopsFor"] == "max length");
        CHECK(started["stopsAtClock"] == "10:01");
        recordReply(client, {{"type", "record"}, {"op", "stop"}});
    }
}

TEST_CASE("with the maximum total length off, a recording outside a Gig window runs until stopped")
{
    TempDir dir;
    azl::EngineHarness harness(kSmall);
    Client client(harness.document());
    Notices notices(harness.document());
    const Message started = startedAt(harness, client, kNow,
                                      {{"path", (dir.path / "open.mov").string()}, {"maxTotalSeconds", 0}});
    CHECK(started["stopsAt"].is_null());
    CHECK(started["stopsAtClock"] == "");
    CHECK(started["stopsFor"] == "");
    stepWritten(harness, 120);
    CHECK(harness.recorder().status().recording);
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["frames"] == 120);
    const auto ended = notices.awaitStopped();
    REQUIRE(ended.size() == 1);
    CHECK(ended[0]["reason"] == "stopped");
    CHECK_FALSE(ended[0].contains("notice"));
    Client fresh(harness.document());
    CHECK(recordReply(fresh, {{"type", "record"}, {"op", "state"}})["stopsAt"].is_null());
}

TEST_CASE("the engine's recording reserve defaults to 20 GB or 10 % of the disk, whichever is larger")
{
    const azl::Recorder::Settings settings;
    CHECK(azl::Recorder::reserveOf(settings, 100 * kGB) == 20 * kGB);
    CHECK(azl::Recorder::reserveOf(settings, 1000 * kGB) == 100 * kGB);
    CHECK(azl::Recorder::reserveOf({.reserveBytes = 0, .reservePercent = 0.0}, 1000 * kGB) == 0);
}

TEST_CASE("recording never stalls rendering: a slow writer drops recorded frames, not rendered ones")
{
    TempDir dir;
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    constexpr double kSeconds = 3.0;
    auto live = [&] {
        harness.stats().maxCpuMicros = 0;
        const uint64_t rendered = harness.stats().rendered;
        const uint64_t skipped = harness.stats().skipped;
        harness.runLive(kSeconds);
        return std::array<uint64_t, 3>{harness.stats().rendered - rendered, harness.stats().skipped - skipped,
                                       harness.stats().maxCpuMicros.load()};
    };

    const auto plain = live();

    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"path", (dir.path / "a.mov").string()}})["type"] ==
            "record");
    const auto recording = live();
    const Message normal = recordReply(client, {{"type", "record"}, {"op", "stop"}});

    harness.recorder().setWriteDelay(std::chrono::milliseconds(60));
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"path", (dir.path / "b.mov").string()}})["type"] ==
            "record");
    const auto slow = live();
    const Message slowed = recordReply(client, {{"type", "record"}, {"op", "stop"}});

    MESSAGE("rendered / skipped / max cpu us: plain ", plain[0], " / ", plain[1], " / ", plain[2], ", recording ",
            recording[0], " / ", recording[1], " / ", recording[2], " (", normal["frames"], " recorded, ",
            normal["dropped"], " dropped), slow writer ", slow[0], " / ", slow[1], " / ", slow[2], " (",
            slowed["frames"], " recorded, ", slowed["dropped"], " dropped)");
    CHECK(recording[0] >= plain[0] * 95 / 100);
    CHECK(slow[0] >= plain[0] * 95 / 100);
    CHECK(recording[1] == plain[1]);
    CHECK(slow[1] == plain[1]);
    CHECK(normal["frames"].get<uint64_t>() + normal["dropped"].get<uint64_t>() >= recording[0] * 95 / 100);
    CHECK(slowed["dropped"].get<uint64_t>() > slowed["frames"].get<uint64_t>());
    CHECK(slowed["frames"].get<uint64_t>() > 0);
}

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::vector<float> decodeAudio(const fs::path& path, double& rate)
{
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path.c_str())] options:nil];
    NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    REQUIRE(tracks.count == 1);
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
    AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:tracks[0]
                         outputSettings:@{AVFormatIDKey : @(kAudioFormatLinearPCM),
                                          AVLinearPCMBitDepthKey : @32,
                                          AVLinearPCMIsFloatKey : @YES,
                                          AVLinearPCMIsNonInterleaved : @NO,
                                          AVLinearPCMIsBigEndianKey : @NO,
                                          AVNumberOfChannelsKey : @1}];
    [reader addOutput:output];
    REQUIRE([reader startReading]);
    std::vector<float> samples;
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
        rate = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample))->mSampleRate;
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        const size_t bytes = CMBlockBufferGetDataLength(block);
        const size_t at = samples.size();
        samples.resize(at + bytes / sizeof(float));
        CMBlockBufferCopyDataBytes(block, 0, bytes, samples.data() + at);
        CFRelease(sample);
    }
    return samples;
}
#pragma clang diagnostic pop

} // namespace

TEST_CASE("the audio input records as a track aligned with the frames")
{
    TempDir dir;
    constexpr double kRate = 48000.0;
    constexpr int kFrames = 90;
    constexpr int kClickFrame = 45;
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::EngineHarness harness(kSmall);
    harness.recorder().setAudio(&analysis);
    Client client(harness.document());

    // The input runs before the recording starts, and the movie starts at the first recorded frame, not at time 0.
    std::vector<float> chunk(size_t(kRate / 60.0));
    analysis.push(chunk.data(), chunk.size(), 0.0, kRate);
    analysis.pump();
    harness.step(30);
    const fs::path file = dir.path / "audio.mov";
    const Message started =
        recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()}});
    REQUIRE(started["type"] == "record");
    CHECK(started["audioTrack"] == true);
    for (int i = 0; i < kFrames; ++i) {
        for (size_t k = 0; k < chunk.size(); ++k)
            chunk[k] = i == kClickFrame ? float(0.8 * std::sin(2.0 * M_PI * 1000.0 * double(k) / kRate)) : 0.0f;
        analysis.push(chunk.data(), chunk.size(), harness.time(), kRate);
        analysis.pump();
        harness.step();
        harness.recorder().waitUntilWritten();
    }
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["frames"] == kFrames);
    CHECK(stopped["audioDropped"] == 0);

    const Movie movie = decode(file);
    REQUIRE(movie.times.size() == size_t(kFrames));
    CHECK(movie.times[0] == doctest::Approx(0.0));
    double rate = 0.0;
    const std::vector<float> samples = decodeAudio(file, rate);
    REQUIRE(rate == kRate);
    CHECK(double(samples.size()) / rate == doctest::Approx(kFrames / 60.0).epsilon(0.03));
    const auto loud = std::find_if(samples.begin(), samples.end(), [](float s) { return std::abs(s) > 0.2f; });
    REQUIRE(loud != samples.end());
    const double heard = double(loud - samples.begin()) / rate;
    MESSAGE("click heard at ", heard, " s; its frame shows at ", movie.times[kClickFrame], " s");
    CHECK(std::abs(heard - movie.times[kClickFrame]) < 0.005);
}

namespace {

struct Tracks {
    size_t audio = 0;
    FourCharCode audioFormat = 0;
    double audioRate = 0.0;
    double audioSeconds = 0.0;
    double videoSeconds = 0.0;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
Tracks tracksOf(const fs::path& path)
{
    Tracks tracks;
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(path.c_str())] options:nil];
    NSArray<AVAssetTrack*>* video = [asset tracksWithMediaType:AVMediaTypeVideo];
    REQUIRE(video.count == 1);
    tracks.videoSeconds = CMTimeGetSeconds(video[0].timeRange.duration);
    NSArray<AVAssetTrack*>* audio = [asset tracksWithMediaType:AVMediaTypeAudio];
    tracks.audio = audio.count;
    if (audio.count) {
        auto format = (__bridge CMAudioFormatDescriptionRef)audio[0].formatDescriptions.firstObject;
        const AudioStreamBasicDescription* basic = CMAudioFormatDescriptionGetStreamBasicDescription(format);
        tracks.audioFormat = basic->mFormatID;
        tracks.audioRate = basic->mSampleRate;
        tracks.audioSeconds = CMTimeGetSeconds(audio[0].timeRange.duration);
    }
    return tracks;
}
#pragma clang diagnostic pop

// The "state" broadcast that follows the reply to a request.
Message nextState(Client& client)
{
    for (;;) {
        Message m = client.next();
        if (m["type"] == "record" && m["op"] == "state")
            return m;
    }
}

} // namespace

TEST_CASE("the recording's audio track is AAC at 48 kHz in sync with the frames whatever the input rate")
{
    constexpr int kFrames = 120;
    constexpr int kClickFrame = 60;
    // 192 kHz is the venue's Scarlett Solo, which AAC does not take: the track used to be dropped.
    for (const double rate : {192000.0, 96000.0, 48000.0, 44100.0}) {
        CAPTURE(rate);
        TempDir dir;
        azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
        azl::EngineHarness harness(kSmall);
        harness.recorder().setAudio(&analysis);
        Client client(harness.document());
        std::vector<float> chunk(size_t(rate / 60.0));
        analysis.push(chunk.data(), chunk.size(), 0.0, rate);
        analysis.pump();
        harness.step(10);
        const fs::path file = dir.path / "audio.mov";
        const Message started =
            recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()}});
        REQUIRE_MESSAGE(started["type"] == "record", started.dump());
        CHECK(started["audio"] == "on");
        CHECK(started["audioSource"] == "audio input");
        CHECK(started["audioReason"] == "");
        for (int i = 0; i < kFrames; ++i) {
            for (size_t k = 0; k < chunk.size(); ++k)
                chunk[k] = i == kClickFrame ? float(0.8 * std::sin(2.0 * M_PI * 1000.0 * double(k) / rate)) : 0.0f;
            analysis.push(chunk.data(), chunk.size(), harness.time(), rate);
            analysis.pump();
            harness.step();
            harness.recorder().waitUntilWritten();
        }
        const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
        CHECK(stopped["frames"] == kFrames);
        CHECK(stopped["audioDropped"] == 0);
        CHECK(stopped["audioTrack"] == true);

        const Tracks tracks = tracksOf(file);
        REQUIRE(tracks.audio == 1);
        CHECK(tracks.audioFormat == kAudioFormatMPEG4AAC);
        CHECK(tracks.audioRate == 48000.0);
        MESSAGE("audio ", tracks.audioSeconds, " s, video ", tracks.videoSeconds, " s");
        CHECK(std::abs(tracks.audioSeconds - tracks.videoSeconds) <= 1.0 / 60.0);

        const Movie movie = decode(file);
        double decodedRate = 0.0;
        const std::vector<float> samples = decodeAudio(file, decodedRate);
        REQUIRE(decodedRate == 48000.0);
        const auto loud = std::find_if(samples.begin(), samples.end(), [](float s) { return std::abs(s) > 0.2f; });
        REQUIRE(loud != samples.end());
        const double heard = double(loud - samples.begin()) / decodedRate;
        MESSAGE("click heard at ", heard, " s; its frame shows at ", movie.times[kClickFrame], " s");
        CHECK(std::abs(heard - movie.times[kClickFrame]) < 0.005);
        float peak = 0.0f;
        for (float s : samples)
            peak = std::max(peak, std::abs(s));
        CHECK(peak == doctest::Approx(0.8).epsilon(0.15));
    }
}

TEST_CASE("a live recording rolls over without a dropped frame however slowly a file finishes or an encoder sets up")
{
    TempDir dir;
    constexpr double kRate = 48000.0;
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::EngineHarness harness(kSmall);
    harness.recorder().setAudio(&analysis);
    harness.recorder().setRolloverDelays(std::chrono::milliseconds(1500), std::chrono::milliseconds(500));
    Client client(harness.document());
    std::vector<float> chunk(size_t(kRate / 100.0));
    analysis.push(chunk.data(), chunk.size(), 0.0, kRate);
    analysis.pump();

    harness.startLive();
    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        const double begin = harness.time();
        const auto clock = std::chrono::steady_clock::now();
        int64_t sent = 0;
        while (feeding) {
            const double due = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock).count() * kRate;
            for (; double(sent) + double(chunk.size()) <= due; sent += int64_t(chunk.size()))
                analysis.push(chunk.data(), chunk.size(), begin + double(sent) / kRate, kRate);
            analysis.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const Message started = recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"},
                                                 {"path", (dir.path / "live.mov").string()}, {"maxSeconds", 1.0}});
    REQUIRE_MESSAGE(started["type"] == "record", started.dump());
    CHECK(started["audio"] == "on");
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    feeding = false;
    feeder.join();
    harness.stopLive();

    MESSAGE(stopped["frames"], " frames recorded, ", stopped["dropped"], " dropped");
    CHECK(stopped["dropped"] == 0);
    CHECK(stopped["audioDropped"] == 0);
    const std::vector<fs::path> files = movies(dir.path);
    REQUIRE(files.size() >= 3);
    size_t frames = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        CAPTURE(files[i]);
        char name[32];
        std::snprintf(name, sizeof name, "live %03zu.mov", i + 1);
        CHECK(files[i] == dir.path / name);
        const Movie movie = decode(files[i]);
        REQUIRE_FALSE(movie.times.empty());
        CHECK(movie.times.front() == doctest::Approx(0.0));
        double gap = 0.0;
        for (size_t k = 1; k < movie.times.size(); ++k)
            gap = std::max(gap, movie.times[k] - movie.times[k - 1]);
        CHECK(gap < 2.5 / 60.0);
        if (i + 1 < files.size())
            CHECK(movie.frames.size() == 60);
        frames += movie.frames.size();
        const Tracks tracks = tracksOf(files[i]);
        REQUIRE(tracks.audio == 1);
        MESSAGE(movie.frames.size(), " frames, largest gap ", gap * 1000.0, " ms, audio ", tracks.audioSeconds,
                " s, video ", tracks.videoSeconds, " s");
        CHECK(std::abs(tracks.audioSeconds - tracks.videoSeconds) < 0.05);
    }
    CHECK(frames == stopped["frames"].get<size_t>());
}

TEST_CASE("a recording without an audio input says why and still writes the video")
{
    TempDir dir;
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::EngineHarness harness(kSmall);
    harness.recorder().setAudio(&analysis);
    Client client(harness.document());
    harness.step(5);
    const fs::path file = dir.path / "silent.mov";
    const Message started =
        recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()}});
    REQUIRE_MESSAGE(started["type"] == "record", started.dump());
    CHECK(started["recording"] == true);
    CHECK(started["audio"] == "unavailable");
    CHECK(started["audioReason"] == "no audio input running");
    CHECK(started["audioTrack"] == false);
    const Message broadcast = nextState(client);
    CHECK(broadcast["notice"] == "Recording without audio: no audio input running");
    for (int i = 0; i < 30; ++i) {
        harness.step();
        harness.recorder().waitUntilWritten();
    }
    const Message stopped = recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(stopped["frames"] == 30);
    CHECK(tracksOf(file).audio == 0);
    CHECK(decode(file).frames.size() == 30);

    // The input's own name for why nothing runs.
    harness.recorder().setAudio(&analysis, [] { return azl::Recorder::AudioSource{.reason = "the audio input is off"}; });
    Client fresh(harness.document());
    const Message state = recordReply(fresh, {{"type", "record"}, {"op", "state"}});
    CHECK(state["audio"] == "unavailable");
    CHECK(state["audioReason"] == "the audio input is off");
}

TEST_CASE("record audio off writes no audio track and says off")
{
    TempDir dir;
    constexpr double kRate = 48000.0;
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::EngineHarness harness(kSmall);
    harness.recorder().setAudio(&analysis);
    Client client(harness.document());
    std::vector<float> chunk(size_t(kRate / 60.0), 0.1f);
    analysis.push(chunk.data(), chunk.size(), 0.0, kRate);
    analysis.pump();
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"audio", "off"}})["audio"] == "off");
    const fs::path file = dir.path / "off.mov";
    const Message started =
        recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"}, {"path", file.string()}});
    REQUIRE_MESSAGE(started["type"] == "record", started.dump());
    CHECK(started["audio"] == "off");
    CHECK(started["audioReason"] == "");
    CHECK_FALSE(nextState(client).contains("notice"));
    for (int i = 0; i < 30; ++i) {
        analysis.push(chunk.data(), chunk.size(), harness.time(), kRate);
        analysis.pump();
        harness.step();
        harness.recorder().waitUntilWritten();
    }
    recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(tracksOf(file).audio == 0);
    CHECK(recordReply(client, {{"type", "record"}, {"op", "set"}, {"audio", true}})["audio"] == "on");
}

namespace {

CVPixelBufferRef pixelBuffer(OSType format, uint32_t width, uint32_t height)
{
    NSDictionary* attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                                 (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    CVPixelBufferRef buffer = nullptr;
    REQUIRE(CVPixelBufferCreate(nullptr, width, height, format, (__bridge CFDictionaryRef)attributes, &buffer) ==
            kCVReturnSuccess);
    CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    return buffer;
}

// Every sample of every plane, in 8-bit codes; 16-bit formats keep their fractions.
std::vector<double> samples(CVPixelBufferRef buffer)
{
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    const bool wide = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || format == kCVPixelFormatType_422YpCbCr16;
    std::vector<double> out;
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const bool planar = CVPixelBufferIsPlanar(buffer);
    const size_t planes = planar ? CVPixelBufferGetPlaneCount(buffer) : 1;
    for (size_t p = 0; p < planes; ++p) {
        const auto* base = static_cast<const uint8_t*>(planar ? CVPixelBufferGetBaseAddressOfPlane(buffer, p)
                                                              : CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(buffer, p) : CVPixelBufferGetBytesPerRow(buffer);
        const size_t rows = planar ? CVPixelBufferGetHeightOfPlane(buffer, p) : CVPixelBufferGetHeight(buffer);
        const size_t count = planar ? CVPixelBufferGetWidthOfPlane(buffer, p) * (p ? 2 : 1)
                                    : CVPixelBufferGetWidth(buffer) * 2;
        for (size_t y = 0; y < rows; ++y) {
            const uint8_t* row = base + y * stride;
            for (size_t i = 0; i < count; ++i)
                out.push_back(wide ? double(row[2 * i] | row[2 * i + 1] << 8) / 256.0 : double(row[i]));
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return out;
}

} // namespace

TEST_CASE("the recorder writes the encoder's Y'CbCr format as VideoToolbox's own conversion would")
{
    constexpr uint32_t kWidth = 128;
    constexpr uint32_t kHeight = 64;
    // Smooth colour ramps with sharp bars across them, as a composition has both.
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* p = &bgra[(size_t(y) * kWidth + x) * 4];
            const bool bar = (x / 5) % 3 == 0;
            p[0] = uint8_t(bar ? 255 - x : y * 4);
            p[1] = uint8_t(bar ? 30 : x * 2);
            p[2] = uint8_t(bar ? y * 3 : 255 - x * 2);
            p[3] = 255;
        }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:kWidth
                                                                                   height:kHeight
                                                                                mipmapped:NO];
    id<MTLTexture> source = [device newTextureWithDescriptor:desc];
    [source replaceRegion:MTLRegionMake2D(0, 0, kWidth, kHeight) mipmapLevel:0 withBytes:bgra.data()
              bytesPerRow:kWidth * 4];
    CVPixelBufferRef picture = pixelBuffer(kCVPixelFormatType_32BGRA, kWidth, kHeight);
    CVPixelBufferLockBaseAddress(picture, 0);
    for (uint32_t y = 0; y < kHeight; ++y)
        std::memcpy(static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(picture)) + y * CVPixelBufferGetBytesPerRow(picture),
                    &bgra[size_t(y) * kWidth * 4], kWidth * 4);
    CVPixelBufferUnlockBaseAddress(picture, 0);
    VTPixelTransferSessionRef transfer = nullptr;
    REQUIRE(VTPixelTransferSessionCreate(nullptr, &transfer) == noErr);
    id<MTLCommandQueue> queue = [device newCommandQueue];

    for (const OSType format : {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_422YpCbCr8,
                                kCVPixelFormatType_422YpCbCr16}) {
        CAPTURE(format);
        CVPixelBufferRef theirs = pixelBuffer(format, kWidth, kHeight);
        REQUIRE(VTPixelTransferSessionTransferImage(transfer, picture, theirs) == noErr);
        CVPixelBufferRef ours = pixelBuffer(format, kWidth, kHeight);
        azl::YCbCrConverter converter((__bridge MTL::Device*)device, format);
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        REQUIRE(converter.encode((__bridge MTL::CommandBuffer*)cmd, (__bridge MTL::Texture*)source, ours));
        [cmd commit];
        [cmd waitUntilCompleted];

        const std::vector<double> a = samples(theirs);
        const std::vector<double> b = samples(ours);
        REQUIRE(a.size() == b.size());
        double worst = 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(a[i] - b[i]));
            sum += std::abs(a[i] - b[i]);
        }
        MESSAGE("format ", format, ": worst sample difference ", worst, ", mean ", sum / double(a.size()));
        CHECK(worst <= 1);
        CHECK(sum / double(a.size()) < 0.2);
        CFRelease(theirs);
        CFRelease(ours);
    }
    VTPixelTransferSessionInvalidate(transfer);
    CFRelease(transfer);
    CFRelease(picture);
}

TEST_CASE("a recording cut off by a killed engine still plays up to its last fragment and is recovered at start")
{
    TempDir dir;
    TempDir crashed;
    constexpr double kRate = 48000.0;
    constexpr int kFrames = 300;
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::EngineHarness harness(kSmall);
    harness.recorder().setAudio(&analysis);
    Client client(harness.document());
    std::vector<float> chunk(size_t(kRate / 60.0));
    analysis.push(chunk.data(), chunk.size(), 0.0, kRate);
    analysis.pump();
    const fs::path file = dir.path / "cut.mov";
    REQUIRE(recordReply(client, {{"type", "record"}, {"op", "start"}, {"codec", "hevc"},
                                 {"path", file.string()}})["audioTrack"] == true);
    for (int i = 0; i < kFrames; ++i) {
        analysis.push(chunk.data(), chunk.size(), harness.time(), kRate);
        analysis.pump();
        harness.step();
        harness.recorder().waitUntilWritten();
    }
    // What a killed engine leaves on disk: the partial file as it is mid-recording.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    fs::copy_file(dir.path / ".cut.partial.mov", crashed.path / ".cut.partial.mov");
    recordReply(client, {{"type", "record"}, {"op", "stop"}});
    CHECK(decode(file).frames.size() == kFrames);

    std::ofstream(crashed.path / ".garbage.partial.mov") << "not a movie";
    azl::Recorder::recover(crashed.path);
    CHECK_FALSE(fs::exists(crashed.path / ".cut.partial.mov"));
    CHECK(fs::exists(crashed.path / ".garbage.partial.mov"));
    CHECK_FALSE(fs::exists(crashed.path / "garbage.mov"));
    const Movie movie = decode(crashed.path / "cut.mov");
    MESSAGE("the cut movie has ", movie.frames.size(), " of ", kFrames, " frames");
    CHECK(movie.frames.size() >= 240);
    CHECK(movie.frames.size() <= size_t(kFrames));
    CHECK(movie.times[0] == doctest::Approx(0.0));
    double rate = 0.0;
    CHECK(decodeAudio(crashed.path / "cut.mov", rate).size() > 0);
}
