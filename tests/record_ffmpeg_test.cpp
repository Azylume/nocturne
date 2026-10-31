// The recorder's movies off Apple: the twins of record_test.mm's movie checks, through recording::Writer and the
// FFmpeg encoder, fed the pictures the GPU's Y'CbCr pass will hand it.
#include "ffmpeg_movies.hpp"
#include "test_files.hpp"

#include "output/movie_encoder_ffmpeg.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace {

namespace rec = azl::recording;

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;
constexpr double kFps = 60.0;
// The engine's clock at the first recorded frame; the movie starts there, not at 0.
constexpr double kStart = 100.0;
constexpr uint64_t kGB = uint64_t(1) << 30;

uint32_t fourcc(const char* code)
{
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
           uint8_t(code[3]);
}

rec::Space plenty(const fs::path&)
{
    return {500 * kGB, 1000 * kGB};
}

struct Options {
    Options(std::string codec, fs::path path) : codec(std::move(codec)), path(std::move(path)) {}

    std::string codec;
    fs::path path;
    bool precise = false;
    bool audio = false;
    uint64_t maxBytes = 0;
    double maxSeconds = 0.0;
    uint64_t reserveBytes = 0;
    // The FFmpeg encoder to use; empty: the recorder's own choice.
    std::string only;
    rec::SpaceProvider space = plenty;
    std::chrono::milliseconds spaceInterval{1000};
};

// A recording driven like the engine drives it: a picture per frame filled and queued, audio at its capture times.
class Recording {
public:
    explicit Recording(const Options& options)
    {
        rec::Settings settings;
        settings.codec = options.codec;
        settings.path = options.path;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.precise = options.precise;
        settings.maxBytes = options.maxBytes;
        settings.maxSeconds = options.maxSeconds;
        settings.reserveBytes = options.reserveBytes;
        settings.reservePercent = 0.0;
        rec::AudioPlan audio;
        if (options.audio)
            audio = {"on", "", "test tone"};
        writer_ = std::make_unique<rec::Writer>(settings, audio, options.space, options.spaceInterval,
                                                [this](const rec::Ended& e) { ended(e); }, nullptr);
        writer_->open(
            [&](rec::MovieEncoder::Counts& counts) {
                auto encoder = std::make_unique<rec::FfmpegEncoder>(options.codec, kWidth, kHeight, kFps,
                                                                    options.precise, counts, options.only);
                encoder_ = encoder.get();
                return encoder;
            },
            std::chrono::microseconds(0));
        writer_->start(1e9, std::nullopt, std::chrono::microseconds(0), std::chrono::microseconds(0));
    }

    ~Recording()
    {
        writer_->close();
        writer_->join();
    }

    static double timeOf(int frame) { return kStart + frame / kFps; }

    // The frame's picture, queued as the GPU would once finished; false when no picture was free (dropped).
    // `recorderMs` gets the time spent in the recorder's calls, the part the render thread pays.
    bool frame(int index, double* recorderMs = nullptr)
    {
        using Clock = std::chrono::steady_clock;
        const auto took = [](Clock::time_point since) {
            return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
        };
        Clock::time_point began = Clock::now();
        std::shared_ptr<azl::YCbCrPicture> picture = encoder_->newPicture();
        if (!picture)
            writer_->drop();
        double spent = took(began);
        if (picture) {
            fill(*picture, card(kWidth, kHeight, index));
            began = Clock::now();
            writer_->begin();
            writer_->queued(picture, timeOf(index), true);
            spent += took(began);
        }
        if (recorderMs)
            *recorderMs = spent;
        return bool(picture);
    }

    // A frame with its audio, then waits for the writer, as the harness's waitUntilWritten does.
    void frameWritten(int index, const std::vector<float>& sound = {}, double rate = 48000.0)
    {
        if (!sound.empty())
            writer_->heard(sound.data(), sound.size(), timeOf(index), rate);
        for (int i = 0; i < 1000 && !writer_->done; ++i) {
            if (frame(index))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        writer_->waitIdle(false);
    }

    rec::Ended stop()
    {
        writer_->waitIdle(true);
        writer_->close();
        writer_->join();
        return awaitEnded();
    }

    rec::Ended awaitEnded()
    {
        std::unique_lock lock(mutex_);
        REQUIRE(ended_changed_.wait_for(lock, std::chrono::seconds(30), [&] { return ended_.has_value(); }));
        return *ended_;
    }

    rec::Writer& writer() { return *writer_; }
    rec::FfmpegEncoder& encoder() { return *encoder_; }

private:
    void ended(const rec::Ended& e)
    {
        writer_->done = true;
        std::lock_guard lock(mutex_);
        ended_ = e;
        ended_changed_.notify_all();
    }

    std::unique_ptr<rec::Writer> writer_;
    rec::FfmpegEncoder* encoder_ = nullptr;
    std::mutex mutex_;
    std::condition_variable ended_changed_;
    std::optional<rec::Ended> ended_;
};

// With NOCTURNE_RECORD_OUT set, keeps a copy of the movie there as NAME.mov, to play or probe it elsewhere.
void keep(const fs::path& movie, const std::string& name)
{
    if (const char* out = std::getenv("NOCTURNE_RECORD_OUT"))
        fs::copy_file(movie, fs::path(out) / (name + ".mov"), fs::copy_options::overwrite_existing);
}

// Records `frames` frames and checks each decodes back to the card it was, at its time. Returns the FFmpeg encoder.
std::string recordAndCheck(const Options& options, int frames, AVCodecID codec, uint32_t tag, double tolerance)
{
    Recording recording(options);
    const std::string encoder = recording.encoder().name();
    MESSAGE("encoder ", encoder, ", hardware ", recording.encoder().hardware().value_or(false));
    for (int i = 0; i < frames; ++i)
        recording.frameWritten(i);
    const rec::Ended ended = recording.stop();
    CHECK(ended.reason == "stopped");
    CHECK(ended.kept);
    CHECK(ended.frames == uint64_t(frames));
    CHECK(ended.dropped == 0);
    CHECK(ended.file == options.path);
    CHECK_FALSE(fs::exists(options.path.parent_path() / ("." + options.path.stem().string() + ".partial.mov")));

    keep(options.path, options.codec + (options.precise ? "-precise" : ""));
    const Movie movie = decodeMovie(options.path);
    CHECK(movie.codec == codec);
    CHECK(movie.tag == tag);
    REQUIRE(movie.frames.size() == size_t(frames));
    double worst = 0.0;
    for (int i = 0; i < frames; ++i) {
        CHECK(movie.times[size_t(i)] == doctest::Approx(double(i) / kFps).epsilon(1e-4));
        worst = std::max(worst, meanError(movie.frames[size_t(i)], card(kWidth, kHeight, i)));
    }
    MESSAGE("worst mean error per channel ", worst);
    CHECK(worst < tolerance);
    // The marker moves, so each frame is closer to its own picture than to one 20 frames away.
    CHECK(meanError(movie.frames.back(), card(kWidth, kHeight, frames - 1)) <
          meanError(movie.frames.back(), card(kWidth, kHeight, frames - 21)));
    return encoder;
}

std::vector<fs::path> movies(const fs::path& folder)
{
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(folder))
        found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

// A tone burst in the one frame's worth of samples at `rate`, silence otherwise.
std::vector<float> chunkOf(double rate, bool loud)
{
    std::vector<float> chunk(size_t(rate / kFps));
    if (loud)
        for (size_t k = 0; k < chunk.size(); ++k)
            chunk[k] = float(0.8 * std::sin(2.0 * M_PI * 1000.0 * double(k) / rate));
    return chunk;
}

// A small NTFS volume in a VHDX mounted on a folder, so the disk really fills up. Needs an elevated process.
struct SmallDisk {
    explicit SmallDisk(int megabytes)
    {
        image = dir.path / "disk.vhdx";
        mount = dir.path / "mount";
        fs::create_directories(mount);
        REQUIRE(diskpart("create vdisk file=\"" + image.string() + "\" maximum=" + std::to_string(megabytes) +
                         " type=expandable\nselect vdisk file=\"" + image.string() +
                         "\"\nattach vdisk\ncreate partition primary\nformat fs=ntfs quick label=azlrec\n"
                         "assign mount=\"" + mount.string() + "\"\n"));
    }
    ~SmallDisk() { diskpart("select vdisk file=\"" + image.string() + "\"\ndetach vdisk\n"); }

    bool diskpart(const std::string& script)
    {
        const fs::path file = dir.path / "diskpart.txt";
        std::ofstream(file) << script;
        return std::system(("diskpart /s \"" + file.string() + "\" > \"" + (dir.path / "diskpart.log").string() +
                            "\"").c_str()) == 0;
    }

    TempDir dir;
    fs::path image;
    fs::path mount;
};

// The volume's real free space, checked as often as the Mac's full-disk case does.
Options onSmallDisk(Options options)
{
    options.space = rec::volumeSpace;
    options.spaceInterval = std::chrono::milliseconds(5);
    return options;
}

// Records until the recording ends by itself.
rec::Ended recordUntilEnded(Recording& recording)
{
    for (int i = 0; i < 3000 && !recording.writer().done; ++i)
        recording.frameWritten(i);
    REQUIRE(recording.writer().done);
    return recording.awaitEnded();
}

} // namespace

TEST_CASE("off Apple, frames record to HEVC and decode back to the pictures handed in")
{
    TempDir dir;
    recordAndCheck(Options("hevc", dir.path / "composition.mov"), 60, AV_CODEC_ID_HEVC, fourcc("hvc1"),
                   4.0);
}

TEST_CASE("off Apple, frames record to ProRes 422 and decode back to the pictures handed in")
{
    TempDir dir;
    recordAndCheck(Options("prores", dir.path / "screen.mov"), 60, AV_CODEC_ID_PRORES, fourcc("apcn"), 2.0);
}

TEST_CASE("off Apple, precise recording hands the encoders 10 bits where they take them")
{
    TempDir dir;
    Options hevc("hevc", dir.path / "hevc.mov");
    hevc.precise = true;
    const std::string encoder = recordAndCheck(hevc, 30, AV_CODEC_ID_HEVC, fourcc("hvc1"), 4.0);
    if (encoder == "hevc_nvenc") {
        CHECK(decodeMovie(hevc.path).profile == AV_PROFILE_HEVC_MAIN_10);
    } else {
        MESSAGE(encoder, " takes 8 bits only: precise HEVC records Main");
        CHECK(decodeMovie(hevc.path).profile == AV_PROFILE_HEVC_MAIN);
    }
    Options prores("prores", dir.path / "prores.mov");
    prores.precise = true;
    recordAndCheck(prores, 30, AV_CODEC_ID_PRORES, fourcc("apcn"), 2.0);
    CHECK(decodeMovie(prores.path).format == AV_PIX_FMT_YUV422P10LE);
}

TEST_CASE("off Apple without NVENC, HEVC records through Media Foundation's encoder")
{
    TempDir dir;
    std::optional<Recording> recording;
    try {
        Options options("hevc", dir.path / "mf.mov");
        options.maxSeconds = 0.5;
        options.only = "hevc_mf";
        recording.emplace(options);
    } catch (const std::exception& e) {
        FAIL("no Media Foundation HEVC encoder: ", e.what());
    }
    MESSAGE("hevc_mf, hardware ", recording->encoder().hardware().value_or(false));
    for (int i = 0; i < 60; ++i)
        recording->frameWritten(i);
    const rec::Ended ended = recording->stop();
    CHECK(ended.frames == 60);
    CHECK(ended.dropped == 0);
    const std::vector<fs::path> files = movies(dir.path);
    REQUIRE(files.size() == 2);
    size_t frames = 0;
    for (const fs::path& file : files) {
        CAPTURE(file);
        const Movie movie = decodeMovie(file);
        REQUIRE_FALSE(movie.keys.empty());
        CHECK(movie.keys.front());
        CHECK(movie.codec == AV_CODEC_ID_HEVC);
        frames += movie.frames.size();
        CHECK(meanError(movie.frames.back(), card(kWidth, kHeight, int(frames) - 1)) < 6.0);
    }
    CHECK(frames == 60);
}

TEST_CASE("off Apple, past its maximum length a recording goes on in the next numbered file without losing a frame")
{
    TempDir dir;
    Options options("hevc", dir.path / "roll.mov");
    options.maxSeconds = 1.0;
    Recording recording(options);
    CHECK(recording.writer().currentFile() == dir.path / "roll 001.mov");
    constexpr int kFrames = 150;
    for (int i = 0; i < kFrames; ++i)
        recording.frameWritten(i);
    const rec::Ended ended = recording.stop();
    CHECK(ended.frames == kFrames);
    CHECK(ended.dropped == 0);
    CHECK(ended.file == dir.path / "roll 003.mov");

    const std::vector<fs::path> files = movies(dir.path);
    REQUIRE(files == std::vector<fs::path>{dir.path / "roll 001.mov", dir.path / "roll 002.mov",
                                           dir.path / "roll 003.mov"});
    const std::vector<size_t> expected{60, 60, 30};
    for (size_t i = 0; i < files.size(); ++i) {
        CAPTURE(files[i]);
        const Movie movie = decodeMovie(files[i]);
        REQUIRE(movie.frames.size() == expected[i]);
        CHECK(movie.keys.front());
        CHECK(movie.times.front() == doctest::Approx(0.0));
        CHECK(movie.times.back() == doctest::Approx(double(expected[i] - 1) / kFps).epsilon(1e-4));
    }
}

TEST_CASE("off Apple, past its maximum size a recording goes on in the next numbered file without losing a frame")
{
    TempDir dir;
    Options options("prores", dir.path / "size.mov");
    options.maxBytes = uint64_t(2) << 20;
    Recording recording(options);
    constexpr int kFrames = 400;
    for (int i = 0; i < kFrames; ++i)
        recording.frameWritten(i);
    const rec::Ended ended = recording.stop();
    CHECK(ended.frames == kFrames);
    CHECK(ended.dropped == 0);

    const std::vector<fs::path> files = movies(dir.path);
    MESSAGE(files.size(), " files");
    REQUIRE(files.size() >= 2);
    size_t frames = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "size %03zu.mov", i + 1);
        CHECK(files[i] == dir.path / name);
        CHECK(fs::file_size(files[i]) < (uint64_t(3) << 20));
        frames += decodeMovie(files[i]).frames.size();
    }
    CHECK(frames == size_t(kFrames));
}

TEST_CASE("off Apple, the recording's audio track is AAC at 48 kHz in sync with the frames whatever the input rate")
{
    constexpr int kFrames = 120;
    constexpr int kClickFrame = 60;
    for (const double rate : {192000.0, 96000.0, 48000.0, 44100.0}) {
        CAPTURE(rate);
        TempDir dir;
        const fs::path file = dir.path / "audio.mov";
        Options options("hevc", file);
        options.audio = true;
        Recording recording(options);
        CHECK(recording.writer().audioPlan.state == "on");
        for (int i = 0; i < kFrames; ++i)
            recording.frameWritten(i, chunkOf(rate, i == kClickFrame), rate);
        const rec::Ended ended = recording.stop();
        CHECK(ended.frames == kFrames);
        CHECK(recording.writer().audioDropped == 0);
        keep(file, "audio-" + std::to_string(int(rate)));

        const Movie movie = decodeMovie(file);
        REQUIRE(movie.audioTracks == 1);
        CHECK(movie.audioCodec == AV_CODEC_ID_AAC);
        REQUIRE(movie.audioRate == 48000);
        REQUIRE(movie.frames.size() == size_t(kFrames));
        MESSAGE("audio ", movie.audioSeconds, " s, video ", movie.videoSeconds, " s");
        CHECK(movie.videoSeconds == doctest::Approx(kFrames / kFps).epsilon(1e-3));
        CHECK(std::abs(movie.audioSeconds - movie.videoSeconds) <= 1.0 / kFps);
        CHECK(double(movie.audio.size()) / movie.audioRate == doctest::Approx(kFrames / kFps).epsilon(0.03));
        const auto loud = std::find_if(movie.audio.begin(), movie.audio.end(), [](float s) { return std::abs(s) > 0.2f; });
        REQUIRE(loud != movie.audio.end());
        const double heard = double(loud - movie.audio.begin()) / movie.audioRate;
        MESSAGE("click heard at ", heard, " s; its frame shows at ", movie.times[kClickFrame], " s");
        CHECK(std::abs(heard - movie.times[kClickFrame]) < 0.005);
        float peak = 0.0f;
        for (float s : movie.audio)
            peak = std::max(peak, std::abs(s));
        CHECK(peak == doctest::Approx(0.8).epsilon(0.15));
    }
}

TEST_CASE("off Apple, a full disk stops the recording cleanly")
{
    SmallDisk disk(24);

    SUBCASE("below the reserve, the movie is finished and kept")
    {
        const fs::path file = disk.mount / "full.mov";
        Options options("prores", file);
        options.reserveBytes = uint64_t(8) << 20;
        Recording recording(onSmallDisk(options));
        const rec::Ended ended = recordUntilEnded(recording);
        CHECK(ended.reason == "disk full");
        CHECK(ended.kept);
        MESSAGE(ended.frames, " frames");
        CHECK(ended.frames > 30);
        CHECK(decodeMovie(file).frames.size() == ended.frames);
    }
    SUBCASE("with no reserve, the write that fails ends it and leaves no partial file")
    {
        const fs::path file = disk.mount / "overflow.mov";
        Recording recording(onSmallDisk(Options("prores", file)));
        const rec::Ended ended = recordUntilEnded(recording);
        CHECK(ended.reason == "disk full");
        CHECK_FALSE(ended.kept);
        CHECK_FALSE(fs::exists(file));
        CHECK_FALSE(fs::exists(disk.mount / ".overflow.partial.mov"));
    }
}

TEST_CASE("off Apple, a recording never makes its caller wait: a slow writer drops recorded frames")
{
    TempDir dir;
    Recording recording(Options("hevc", dir.path / "slow.mov"));
    recording.writer().delayMicros = 50000;
    constexpr int kFrames = 60;
    int queued = 0;
    double slowest = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        double spent = 0.0;
        queued += recording.frame(i, &spent);
        slowest = std::max(slowest, spent);
        std::this_thread::sleep_for(std::chrono::microseconds(16667));
    }
    recording.writer().delayMicros = 0;
    const rec::Ended ended = recording.stop();
    MESSAGE(queued, " of ", kFrames, " frames queued, the recorder's slowest call took ", slowest, " ms");
    // Well below the 50 ms a write takes: the call never waited for the writer.
    CHECK(slowest < 10.0);
    CHECK(ended.dropped > 0);
    CHECK(ended.frames + ended.dropped == kFrames);
    CHECK(decodeMovie(dir.path / "slow.mov").frames.size() == ended.frames);
}

TEST_CASE("off Apple, a recording cut off by a killed engine still plays up to its last fragment and is recovered")
{
    TempDir dir;
    TempDir crashed;
    constexpr int kFrames = 300;
    const fs::path file = dir.path / "cut.mov";
    {
        Options options("hevc", file);
        options.audio = true;
        Recording recording(options);
        for (int i = 0; i < kFrames; ++i)
            recording.frameWritten(i, chunkOf(48000.0, false));
        // What a killed engine leaves on disk: the partial file as it is mid-recording.
        fs::copy_file(dir.path / ".cut.partial.mov", crashed.path / ".cut.partial.mov");
        recording.stop();
    }
    CHECK(decodeMovie(file).frames.size() == kFrames);

    std::ofstream(crashed.path / ".garbage.partial.mov") << "not a movie";
    rec::recover(crashed.path, rec::moviePlays);
    CHECK_FALSE(fs::exists(crashed.path / ".cut.partial.mov"));
    CHECK(fs::exists(crashed.path / ".garbage.partial.mov"));
    CHECK_FALSE(fs::exists(crashed.path / "garbage.mov"));
    const Movie movie = decodeMovie(crashed.path / "cut.mov");
    MESSAGE("the cut movie has ", movie.frames.size(), " of ", kFrames, " frames");
    CHECK(movie.frames.size() >= 240);
    CHECK(movie.frames.size() <= size_t(kFrames));
    CHECK(movie.times[0] == doctest::Approx(0.0));
    CHECK(movie.audio.size() > 0);
}
